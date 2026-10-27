# Design: `alt` async synchronization primitives (phase 5)

**Date:** 2026-10-26
**Status:** Draft for review (revision 2)
**Related:** `2026-09-10-coroutines-roadmap.md` (phase 5),
`2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1-2),
`2026-10-17-thread-pool-and-run-loop-design.md` (phase 3)
**Depends on:** PR #8 (`stop_forwarder` holds a `std::stop_source` copy rather than a
pointer into the frame). This phase adds a `std::stop_callback` living inside an
awaiter, which is the same shape as the hazard #8 removed; the branch rebases on #8
rather than building beside it.

> **Revision note.** Revision 1 was wrong in three ways, all found by the builder
> check and all recorded here because the reasoning matters more than the outcome.
> It invented an unreachable use-after-free and fixed that instead of the reachable
> one; it prescribed a teardown order that deadlocks; and it rejected the
> cancellation approach this revision adopts, on two reasons that do not hold. The
> sections below are the corrected design.

## Intent

Phase 5 is the phase a framework actually reaches for. Phases 1-3 built the
coroutine, the cancellation cascade and the schedulers; nothing yet lets a
coroutine wait for *something that is not another coroutine*. A request handler
awaiting an I/O completion, a UI coroutine awaiting a dialog result, two handlers
that must not touch a resource at once, an owner that needs to know every task it
spawned is gone — none of those are expressible today.

| Component | What it is for |
|---|---|
| `async_event` | A signal many coroutines can wait on. Manual reset. |
| `async_latch` | A count coroutines wait to reach zero. Single use. |
| `async_semaphore` | A permit count coroutines acquire and release. |
| `async_mutex` | Mutual exclusion with an RAII guard, blocking no thread. |
| `task_completion_source<T>` | The bridge from a callback API to an awaitable. |
| `task_group` | An owner's point at which every task it spawned is known gone. |

## Goals

- A coroutine can wait for an external event without blocking a thread.
- A callback-based API can be adapted to `co_await` without leaking or racing.
- **A cancelled chain parked on a primitive tears itself down.** Revision 1 gave
  this up; this revision treats it as a goal, because a request handler awaiting a
  completion that never arrives is the ordinary case for a dropped connection, not
  an edge case.
- An owner can tear down a set of tasks and know when that teardown is complete.

## Non-goals

- **Timers and timeouts.** Phase 3 excluded timers; waiting with a deadline needs a
  clock source this library does not have. Race a completion source against a timer
  once `when_any` exists.
- **Fairness beyond FIFO.** No priorities, no barging control.
- **Reader-writer locks, condition variables, barriers.** No stated use case.
- **A thread-blocking fallback.** A caller who wants to block has `std::mutex` and
  `sync_wait`.
- **Alignment with C++26 `std::execution`.** Excluded by the roadmap.

## Anti-goals

- **A primitive that can leave a dangling waiter**, or that can be destroyed out
  from under its own resumption walk.
- **A primitive whose destruction silently leaks coroutine frames.**
- **A lock that fails open.** A waiter released without the thing it waited for must
  not be able to proceed as though it holds it. Revision 1 had exactly this defect.
- **A second queue or waiter protocol.** Phase 3: a second hand-written copy "would
  be a second chance at the same mistake". One shared mechanism, extracted first.
- **Unbounded recursion on hand-off.** Phase 3 rejected an `inline_scheduler`
  because "the only difference would be that it overflows the stack".

## Cancellation

### What the shipped machinery guarantees

1. A frame suspended on a foreign awaitable is **not an observation point**. The
   check fires on entry to an await and at the two resumptions alt owns.
2. A primitive must **never `destroy()`** a handle it was handed. Only
   `abandon_chain` destroys a chain, and only through the root.
3. `cancellation_context` is **linear-chain shaped**: one root, one pointer relayed
   down one chain.

Fact 3 is what revision 1 missed. While a leaf is parked on a primitive, every
frame above it is suspended awaiting that leaf. No other frame in the chain can be
running, so none can reach a suspension point. The "abandoned by another path while
a node is linked" scenario revision 1 built its structural fix around **cannot
occur**.

### The decision: release on stop, from a stop callback

A primitive's awaiter registers a `std::stop_callback` on the awaiting chain's stop
token when it suspends. On a stop request the callback **unlinks the node and
resumes it with `operation_canceled`**. It destroys nothing and owns no frame. The
released frame resumes, runs to its next suspension point, observes the pending
stop, and tears down through the ordinary cascade — the same mechanism this design
already relies on for release-on-destroy, triggered by the stop instead.

The awaiter reaches the context structurally, with no change to
`detail/cancellation.hpp`: `cancellation_checked::await_suspend` is
`template<typename Promise>` and forwards the real typed handle, so an awaiter
declaring `template<typename Promise> … await_suspend(std::coroutine_handle<Promise>)`
can detect `cancellation_context_pointer()` by the same `requires` test
`task_awaiter` uses. `Promise = void` still satisfies `alt::awaitable`.

**Rejected: giving primitives the context so a stop callback can call
`abandon_chain`.** This is the stronger fix and it is still wrong here — it has a
primitive tearing down a frame it does not own (fact 2) from an arbitrary thread.
Revision 1 also gave two reasons that do not hold, recorded so they are not reused:
that calling `abandon_chain` off-thread is itself illegitimate (it is not —
`checked_resume_on::resume_or_abandon` already does), and that each primitive would
have to be known to the cancellation machinery (it would not — see the structural
detection above).

**The real cost of the adopted approach** is the one revision 1 did not name: a
`std::stop_callback` living in an awaiter touches state that the frame's own
destruction is tearing down, which is the member-ordering trap at
`cancellable_task.hpp`'s promise and the hazard PR #8 removed. The rules that make
it survivable are therefore part of the mechanism's contract, below, not left to
each primitive.

### The reachable use-after-free

Resumption must happen outside the lock, and the node must be unlinked before its
handle is resumed, or a resumed body that re-awaits the same primitive on the same
thread deadlocks or reenters. So a satisfier detaches the list into a local, drops
the lock, then walks and resumes. **During that walk the nodes are reachable only
from the local chain** — invisible to the list and to anything that locks it. Two
reachable interleavings:

- **Re-entrant destruction from a resumed waiter.** `set()` detaches `{X, Y}` and
  resumes X; X's body destroys the primitive (a completion handler destroying the
  object that owns the source is the normal pattern). The destructor sees an empty
  list, returns, and destroys the mutex. `set()` then resumes Y, whose node
  destructor locks freed memory.
- **A satisfier racing the destructor.** One thread is mid-walk while another runs
  the destructor, sees an empty list, and destroys the lock out from under the
  first's pending node destructors.

## The shared mechanism

`include/alt/detail/waiter_list.hpp`, header-only.

An intrusive **doubly-linked** list of nodes guarded by a `std::mutex`. Doubly
linked because unlink-by-node must be O(1): it runs on every normal completion and
every cancellation, from a destructor that cannot afford a list walk.

A node is declared inside an awaiter — hence inside the frame — and holds the
suspended handle, a result slot, and an **atomic claim word**. The claim word is
the answer to the detach→resume window: a node is `linked`, `claimed` (detached by
a satisfier, not yet resumed), or `done`. A destructor that finds its node `linked`
unlinks it under the lock. A destructor that finds it `claimed` must not touch the
list at all — the node is already off it, and the satisfier owns it.

This is the invariant stage A exists to establish, so it is stated as contract
rather than left to be inferred:

- **A primitive outlives its own resumption walk.** A satisfier may not be
  destroyed, by a waiter it resumed or by anything else, before the walk completes.
  Enforced where it can be (the walk holds the primitive's own liveness) and
  documented where it cannot.
- **Destruction does not race a concurrent satisfier.** Destroying a primitive
  while another thread is in `set()`/`release()`/`unlock()` is undefined. This is
  the same class of precondition `std::mutex` carries, and it is why `task_group`
  rather than destruction is the teardown point.
- **Last-statement discipline on the satisfier side.** After resuming the final
  waiter, nothing may read `*this`: the resumed body may have destroyed the
  primitive. The library already carries this rule in `resume_on` and
  `checked_resume_on`.
- **The hand-off is bounded, not recursive.** Resuming waiter N+1 from inside
  waiter N's continuation must not nest. The walk is a loop over the detached
  chain, and a primitive that hands ownership directly (`async_mutex`) drains
  iteratively the way `trampoline_scheduler` does, rather than resuming inline.

Waiters resume **on the thread that satisfied them**. This matches `resume_on`'s
philosophy that scheduler hops are explicit; a coroutine needing a particular
scheduler awaits `resume_on` after the primitive. Documented, not hidden.

## The components

Every wait reports whether it was **satisfied** or **released**. Released means the
stop fired or the primitive was destroyed; it never means the thing was acquired.
The result is `std::expected<…, std::error_code>` with
`std::errc::operation_canceled`, following the project's `std::expected` preference
and `thread_pool::create`'s precedent of `std::error_code` for a condition the OS
already names. `await_resume` is `[[nodiscard]]` on every primitive where ignoring
the result would proceed as though the wait succeeded.

**`async_event`** — manual reset. `set()` releases every waiter and leaves the event
set, so a later await is ready without suspending. `reset()`, `is_set()`.

**`async_latch`** — constructed with a count. `count_down(n = 1)`; reaching zero
releases every waiter. `try_wait()`. Single use, matching `std::latch`.

**`async_semaphore`** — constructed with a permit count. Awaiting acquires one,
suspending when none are free; `release(n = 1)` returns permits and releases that
many waiters FIFO. A released waiter holds **no** permit and must not proceed as
though it does — hence `[[nodiscard]]`.

**`async_mutex`** — awaiting yields `std::expected<async_mutex_lock, std::error_code>`.
The guard is move-only, unlocks on destruction, and is only ever constructed
holding the lock, so the fails-open case is unrepresentable rather than merely
documented. FIFO, with ownership handed directly to the next waiter and drained
iteratively.

**`task_completion_source<T>`** — set once, awaited once, written from a foreign
thread. `set_value(…)`, `set_exception(std::exception_ptr)`; a second set asserts in
debug and is a no-op in release. `await_resume` returns
`std::expected<T, std::error_code>` and **rethrows** a stored exception, because an
exception the caller supplied propagates like any other while `operation_canceled`
is alt's own condition. `as_expected` is therefore not meant to be composed over it;
the docs say so. `void` and `T&` follow `result_store`'s existing shape. The source
is **non-movable**; a foreign callback holds a pointer to it, so the owner keeps it
alive and in place, as with `thread_pool`. The awaited value is moved into the
node at claim time, so a resumed coroutine never reads storage the setter may have
freed.

**`task_group`** — owns spawned tasks and is the teardown point.
`spawn(cancellable_task, start_options)`, `request_stop()`, and an awaitable
`join()` that completes when every owned task has finished or been torn down.
`spawn` is thread-safe against a completion landing concurrently and may be called
after a join has begun. `join()` is awaited by one coroutine at a time. Destroying
an unjoined group with live tasks is a debug assert and a documented precondition:
the group does not own the frames, and blocking would deadlock whenever a task
needs the calling thread.

**This requires an additive change to phase 2**, which the author has approved:
`start_options` gains a completion hook, fired from a promise member's destructor
so that it covers normal completion and cancelled teardown alike — the only point
both paths pass through. Without it there is no channel at all: a `cancellable_task`
self-destroys and notifies nothing, and `m_handle` is private with `start` as its
only friend.

## Teardown order

Revision 1 prescribed stop → join → destroy, which deadlocks: if only destruction
releases a parked waiter, `join()` cannot complete until the primitives are gone.
Releasing on stop removes the circularity. The order is:

1. `request_stop()` on the group. Every parked waiter is released with
   `operation_canceled` by its own stop callback.
2. `co_await group.join()` — finite, because every task is now running toward its
   next suspension point rather than parked.
3. Destroy the primitives, then the group.

Declaration order follows from step 3: **declare the primitives before the group**,
so reverse-order destruction destroys the group first. This is the same rule the
schedulers carry, and it is equally invisible to both sanitizers.

An owner that is not itself a coroutine reaches step 2 through
`sync_wait(group.join(), loop)`, which is reentrant on the loop and must catch,
since a `sync_wait` inside a work item that throws terminates. The docs give this
sequence explicitly rather than leaving a UI-thread destructor to invent it.

## Hazards to document

- Waiters resume on the thread that satisfied them; hop with `resume_on` if the
  continuation is thread-sensitive.
- Ignoring a wait result means proceeding without the lock, the permit or the
  value. Every such result is `[[nodiscard]]`, and `async_mutex` makes the bad
  state unrepresentable.
- Destroying a primitive while another thread is satisfying it is undefined.
- Destroying a primitive with waiters releases them, running coroutine bodies
  during the destructor; anything those bodies touch must outlive the primitive.
- A primitive must outlive its own resumption walk, so a resumed body must not
  destroy the primitive that resumed it.
- `task_group` must be joined before destruction; declare primitives before the
  group.
- A `task_completion_source` never set, never destroyed and never cancelled parks
  its waiter forever.

## Staging

**Stage A** — `detail::waiter_list` with the claim protocol and the stop-callback
release, then `async_event`, `async_latch`, `async_semaphore`, `async_mutex`. The
mechanism is extracted and tested first, as `work_queue` was, because its claim
invariant is the whole correctness argument for the four primitives above it.

**Stage B** — the `start_options` completion hook, then `task_completion_source<T>`
and `task_group`.

Two pull requests. Stage A is four thin primitives over one delicate mechanism;
stage B changes shipped phase-2 API and needs its own review surface.

## What is left to the builder

- Node layout and the claim word's exact states and memory ordering, within the
  contract above.
- Whether `async_mutex` is built on `async_semaphore` or standalone; ownership
  tracking may make sharing a false economy.
- Whether `task_group::join` uses a counter or an internal event. A latch is
  single-use and a group accepts spawns after a join begins.
- Which components get a compiled `.cpp`. The four non-template primitives should
  follow phase 3's precedent (no templates in the interface, `<thread>` kept out of
  consumers, code in cppcheck's scope); `task_completion_source<T>` must be
  header-only.
- Test strategy per primitive, including a mutation check per test, a direct test
  that a cancelled parked waiter is released and its chain torn down, and a test
  that a released mutex waiter cannot enter the critical section.

## Calls I made, for the author to check while reading

The author settled the one question that changed the shape of the work: the
`start_options` completion hook. These two were left and are recorded as the
designer's calls rather than held open, since neither blocks the plan and both are
cheap to reverse before stage B.

1. **`task_group`'s completion accounting is built as a reusable internal** — a
   counter plus an internal event in `detail/`, rather than logic private to
   `task_group`. `task_group` and phase 4's `when_all` overlap heavily: both are
   "several live children of one owner", which the single-`root` cancellation
   context does not express. Designing the accounting as a shared internal keeps
   that option open at near-zero cost, without committing phase 4 to anything.
   Reverse by inlining it if phase 4 takes a different shape.
2. **`task_completion_source` keeps the `std::expected` + rethrow split.** An
   exception the caller supplied propagates like any other exception;
   `operation_canceled` is alt's own condition and travels in the `expected`. This
   deliberately introduces a second convention alongside `as_expected`'s
   `std::expected<T, std::exception_ptr>`, and the docs will say so. The
   alternative — one channel for both — forces either an `exception_ptr` error type
   across every primitive in the phase, or swallowing user exceptions into error
   codes, and the second is worse than a documented seam.

## Dependency on PR #8

Stage A puts a `std::stop_callback` inside an awaiter, which is the same shape as
the hazard PR #8 removed from `cancellable_task`'s promise. This branch rebases
onto #8 once it merges rather than building beside a `stop_forwarder` that still
holds a frame pointer, so that there is one canonical pattern in the tree while six
new users of it are being written.

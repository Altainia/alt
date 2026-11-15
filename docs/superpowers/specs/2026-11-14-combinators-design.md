# Design: `alt` coroutine combinators (phase 4)

**Date:** 2026-11-14
**Status:** Draft
**Related:** `2026-09-10-coroutines-roadmap.md` (phase 4),
`2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1-2),
`2026-10-26-async-primitives-design.md` (phase 5)

> **How this document came about.** The author asked to start phase 4 and opted out of
> the design conversation, so every call here is the designer's rather than theirs. The
> calls that change what gets built are marked **[my call]** and collected at the end for
> checking.

## Intent

Phases 1-3 built the coroutine, the cancellation cascade and the schedulers; phase 5 let
a coroutine wait for something that is not another coroutine. Nothing yet lets a
coroutine wait for **several things at once**. A handler fetching three resources, a UI
action that must finish before a deadline, a shutdown that waits for every outstanding
request — none of those are expressible today without hand-rolling a counter.

| Component | What it is for |
|---|---|
| `when_all` | Await several awaitables; complete when every one has. |
| `when_any` | Await several awaitables; complete when the first one has, and cancel the rest. |

## Goals

- A coroutine can await several awaitables concurrently and get every result.
- A coroutine can race awaitables and take the first, with the losers torn down rather
  than leaked or left running.
- **Cancelling a chain that is inside a combinator tears down every child.** A dropped
  connection is the ordinary case, not an edge case.
- `when_any` makes timeouts expressible, which phase 5 excluded as a non-goal on exactly
  that ground: "race a completion source against a timer once `when_any` exists".

## Non-goals

- **Timers.** Still no clock source in this library. `when_any` makes a timeout
  *expressible* by racing against a caller-supplied awaitable; it does not supply one.
- **`when_all` over an infinite or streaming range.** Phase 6's `async_generator<T>` is
  where that belongs.
- **Structured concurrency enforcement.** `task_group` is the owner's teardown point; a
  combinator is an expression, not a scope.
- **Alignment with C++26 `std::execution`.** Excluded by the roadmap.

## Anti-goals

- **A combinator that can leave a child running after the awaiting chain is gone.**
- **A combinator that lets one child's teardown free a sibling's frame.** This is the
  specific hazard the design exists to avoid; see below.
- **A heap allocation per combinator.** The library erases handlers "without allocating"
  everywhere (`work_item`, `exception_handler`, `completion_handler`); a combinator that
  allocates shared state would be the first exception and would need to earn it.

## The problem that shapes the whole design

`cancellation_context` is **linear-chain shaped**: one root, one pointer relayed down one
chain. `task_promise_base::set_cancellation_context` takes a single pointer, and
`task.hpp:270-275` relays it to the one child being awaited. Phase 5's spec named this
limitation when it put the completion accounting in `detail/`: `task_group` and
`when_all` are both "several live children of one owner, which the single-`root`
cancellation context does not express."

Two things follow, and the second was written down a month before this document.

### The sibling use-after-free

Suppose `when_all` relays the awaiting chain's context to all N children. A child that
reaches a suspension point with a stop pending calls `abandon_chain(ctx)`, which destroys
`ctx.root` — the root frame of the whole chain. That cascades down: the root's locals are
destroyed, including the await temporary holding the combinator, which owns every child
task. So a sibling's frame is freed while that sibling may be:

- inside its own `await_suspend`, which is on the stack; or
- sitting in a detached `waiter_list` chain, already claimed by a satisfier that is about
  to resume it.

The second is the worse one, because the satisfier holds a raw `waiter_node*` into a frame
that no longer exists, and nothing on the list can tell it so.

### It would invalidate an argument four shipped components rest on

`include/alt/async_latch.hpp:58-77` records this in advance:

> That first rule is why `count_down()` can resume its waiters from one multi-node chain
> (`detach_all`) instead of one `detach_one` at a time: nothing in the library lets one
> waiter own a sibling's frame, because there is no `when_all` or race combinator and a
> task is lazy and owns only the child it awaits. Revisit the day such a combinator
> lands: every multi-node resumption in the library would need the same revisit, which is
> `async_event::set`, this `count_down`, `async_semaphore::release` ... and all four
> destructors, including the `~waiter_list` backstop each of them runs ahead of.

The note also names the remedy it would force: "Resuming one node per detach, and not
holding the next node across a resumption." That is a change to four shipped,
heavily-tested components and to the mechanism underneath them.

## The decision: every child is its own root **[my call]**

A combinator does **not** relay the awaiting chain's context to its children. Each child
gets a context whose `root` is that child's own frame, and its own `std::stop_source`
linked to the awaiting chain's token.

What this buys:

- A child observing a stop tears down **only itself**, through the ordinary cascade.
- **No child can ever destroy a sibling.** The premise the four primitives rest on
  continues to hold, and the revisit the latch note anticipated is **not** required by
  this phase. That note should be updated to say why, not deleted.
- The combinator learns a child is gone through the **completion hook** added in phase 5,
  and counts with `detail::completion_counter` — which is exactly what that internal was
  put in `detail/` for.

What it costs:

- A child's cancellation no longer propagates upward on its own. The combinator must
  propagate: on a stop, it requests stop on each child and completes once the count
  reaches zero.
- Each child needs its own stop state, so a combinator over N children holds N
  `std::stop_source`s plus one `std::stop_callback` on the parent's token.

So `when_all` is structurally **`task_group` with results**: N self-owning children, a
count, and a join. That is a deliberate echo, not a coincidence, and the shared pieces
are already shipped and tested.

## The hard part this design does not fully solve

The children write their results into slots, and those slots have to live somewhere.

If the slots live in the combinator's own frame, a child that finishes **after** that
frame is destroyed writes into freed memory. The combinator's frame is destroyed from
above, by `abandon_chain`, which cannot wait for anything: it is called from inside a
leaf's `await_suspend` and must not block.

Phase 5 hit the same wall and resolved it by making the owner responsible — `task_group`
is documented as "stop, then join, then destroy", and its destructor asserts rather than
blocking, because "the group does not own the frames, and blocking would deadlock
whenever a task needs the calling thread." A combinator cannot push that onto the caller:
`co_await when_all(a, b)` is an expression, and there is nowhere for the caller to join.

**[my call]** The resolution is a precondition plus a structural guarantee, in that order:

1. **Structural:** a child parked on anything this library owns is released
   *synchronously* by its own stop callback, on the thread that requests the stop. This
   is the mechanism phase 5 established and measured: `request_stop` is synchronous on the
   caller's thread, `waiter_stop_release` resumes inline, and a released child runs to its
   next suspension point and self-destructs there. So for children built from `alt::task`,
   `alt::task_completion_source` and the four primitives, the combinator's teardown
   requests N stops and every child is gone before the teardown returns.
2. **Precondition, documented:** a child that is *running* on another thread at the moment
   of teardown — mid-computation in a `thread_pool` work item, say — cannot be waited for.
   Such a child must not be given a slot in the combinator's frame. The combinator
   therefore requires that each child's next suspension point is reachable without another
   thread's cooperation, which is the same class of precondition as "destroying a primitive
   while another thread is satisfying it is undefined."

I am not satisfied that (2) is tight enough, and it is the first thing a reader should
attack. The honest alternatives, both rejected for now:

- **Heap-allocated refcounted shared state**, so children can outlive the combinator.
  Costs the allocation this document lists as an anti-goal, and turns a bounded teardown
  into an unbounded one.
- **Block in the teardown** until every child reports. Deadlocks exactly when a child
  needs the thread doing the blocking, which is the `run_loop` case the library is built
  for.

## Components

**`when_all`** — completes when every child has. Results are returned positionally:
a `std::tuple` for a fixed argument list, a `std::vector` for a range. A child whose
awaitable yields `void` occupies its position with a `std::monostate`, so positions stay
stable. **[my call]** Exceptions do not propagate out of the combinator: each position
carries `std::expected<T, std::error_code>`, with a cancelled child reporting
`std::errc::operation_canceled`, following the whole phase-5 family. A child that throws
has its exception stored and rethrown when that position is read, which is
`result_store`'s existing behavior.

**`when_any`** — completes when the first child does, then cancels the rest and waits for
them to be gone before returning. **[my call]** It yields the index of the winner plus
that winner's result, rather than a variant: the index is what a caller switches on, and
a variant over N heterogeneous results is awkward to consume and worse to document.
Losers' results are discarded.

## Staging **[my call]**

Two pull requests, as phase 5 did, because the second is strictly harder than the first.

**Stage A** — `when_all`. The fan-out, the per-child root, the counting, the result
assembly, and the update to `async_latch`'s note explaining why the revisit it predicted
is not needed.

**Stage B** — `when_any`. Adds cancel-the-losers and the wait for their teardown, which is
where the precondition above is load-bearing.

## What is left to the builder

- The per-child wrapper's exact shape: whether it is a distinct promise type or a
  parameterization of `cancellable_task`'s.
- Whether `completion_counter` is used as-is or needs a "resume this one handle at zero"
  path that avoids a `waiter_list` for the single-joiner case.
- Result storage layout, and how a `void` position is represented in the `vector` form.
- Whether `when_all` is itself a `task<T>` or a bespoke awaitable. A `task<T>` reuses
  everything but relays its own context to anything it awaits, which the children must
  not receive.
- Test strategy, including a direct test that a cancelled combinator leaves no child
  running, and a mutation check per test asserting a race or an ordering.

## Calls I made, for the author to check

1. **Every child is its own cancellation root**, rather than relaying the parent's
   context. This is the call the rest of the design hangs on. Reverse it only with a
   plan for the four-component revisit the latch note describes.
2. **Results are `std::expected` per position, and exceptions do not escape the
   combinator.** Consistent with phase 5; the alternative is letting the first exception
   propagate and discarding the others, which loses results the caller asked for.
3. **`when_any` yields an index plus the winner's result**, not a variant.
4. **Two stages, `when_all` first.**
5. **The off-thread-child precondition** in "the hard part", which I flag as the weakest
   part of this design.

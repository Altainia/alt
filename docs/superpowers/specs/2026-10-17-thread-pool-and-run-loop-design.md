# Design: `alt` coroutines phase 3 — `thread_pool` and `run_loop`

**Date:** 2026-10-17
**Status:** Ready for review
**Related:** `2026-09-10-coroutines-roadmap.md` (phase 3), `2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1 and 2)

**Components:**
- `include/alt/run_loop.hpp` + `src/run_loop.cpp` (compiled, public) — new
- `include/alt/thread_pool.hpp` + `src/thread_pool.cpp` (compiled, public) — new
- `include/alt/detail/work_queue.hpp` (header-only, helper) — new
- `include/alt/scheduler.hpp` (existing) — `manual_scheduler` rebuilt on the shared queue

## Intent

Phases 1 and 2 built coroutines that hop between schedulers but shipped no
scheduler a program would actually use: `trampoline_scheduler` runs work inline
and `manual_scheduler` is for tests. A consumer today must write their own.
Phase 3 supplies the two that the motivating scenario needs: somewhere to do
work off the main thread, and somewhere to come back to.

The first consumer is a web framework written in C++ that runs on a server,
with the browser as a thin client sending events over a socket. A button press
there arrives on a connection, is handled, and produces a response. The
framework owns its own loop. Later consumers are expected to target Win32 and
Wayland, where the platform owns the loop instead, and that difference is the
main thing shaping `run_loop`.

Target usage:

```cpp
#include <alt/run_loop.hpp>
#include <alt/thread_pool.hpp>

alt::cancellable_task on_button(session* s, alt::thread_pool& pool, alt::run_loop& ui)
{
    const auto request = s->read_fields();
    co_await alt::resume_on(pool);      // off the loop
    const auto answer = expensive(request);
    co_await alt::resume_on(ui);        // back onto it
    s->respond(answer);
}

int main()
{
    auto made = alt::thread_pool::create();   // hardware_concurrency threads
    if(!made)
    {
        std::println(stderr, "no thread pool: {}", made.error().message());
        return 1;
    }

    alt::thread_pool& pool = *made;
    alt::run_loop     ui;

    serve(ui, pool);   // posts work into the loop as events arrive
    ui.run();          // blocks here until ui.stop()

    // Quiesce before either is destroyed: each drain can post onto the other.
    while(pool.drain() + ui.drain() > 0)
    {
    }
}
```

And the same `run_loop` as a guest of a platform loop:

```cpp
// called from a Win32 message handler, or a Wayland dispatch callback
void on_host_wakeup(alt::run_loop& loop)
{
    loop.run_pending();   // drains what was queued, returns promptly
}

alt::run_loop loop{alt::run_loop_options{.on_wake = post_host_message}};
```

## Goals

- A serial executor usable both as the owner of its thread and as a guest of a
  platform loop, without the consumer choosing a different type for each.
- A thread pool that is obviously correct before it is clever.
- Both usable as `alt::scheduler`, so `resume_on`, the cancellation teardown's
  home scheduler, and everything else from phases 1 and 2 work unchanged.
- `run_loop` additionally satisfies `alt::drivable_scheduler`, so
  `sync_wait(awaitable, loop)` drives it.

## Non-goals

- Work stealing. Deferred until someone measures queue contention.
- Priorities. See the decision below.
- Timers or delayed work. Not in the roadmap for this phase, and a timer wheel
  is its own design.
- Thread affinity, pinning, or dynamic pool resizing.

## Anti-goals

Things that would make this a failure even if it worked:

- A `run_loop` that cannot be driven by a platform event loop. That forecloses
  every consumer after the first.
- A pool whose shutdown silently discards queued work. Queued items include
  coroutine resumptions and cancelled-chain teardowns, so discarding them leaks
  frames invisibly. See the shutdown decision.
- Duplicating the queue-and-wake protocol. The existing one already shipped a
  lost-wakeup defect; two copies of it will drift.

## Constraints

- C++23. GCC 13 or later, Clang 19 or later.
- Public symbols in `namespace alt`, internals in `alt::detail`.
- `snake_case`, `PascalCase` template parameters, `m_` members, `const` by
  default, Doxygen on public API, American English except the `cancel-` family.
- A `work_item` callback may not throw, enforced by its type. So neither
  component needs an exception policy for the work it runs.
- `tests/.clang-tidy` is appended to, never rewritten.

## Decisions

### One `run_loop` type, two modes

`run_loop` is a scheduler, so `post` works from any thread. On top of that:

| Member | On | Behavior |
|---|---|---|
| `post` | both | queues an item; callable from any thread |
| `drain()` | both | runs everything queued, returns how many items ran |
| `abandon()` | both | stops accepting, discards the queue, waits for in-flight items |
| `run()` | `run_loop` | drains, blocking when idle, until `stop()` |
| `stop()` | `run_loop` | makes `run()` return after the current item |
| `run_pending()` | `run_loop` | drains a bounded batch without blocking, returns the count |
| `run_one()`, `run_one_or_wait(stop)`, `notify_waiters()` | `run_loop` | satisfies `drivable_scheduler` |

Two types would differ only in who blocks, which is not a difference worth a
second name.

**`stop()` gates `run()` and nothing else.** It is not a gate on execution. A
stopped loop still runs items through `drain()`, `run_pending()`, `run_one()` and
`run_one_or_wait()`, and its destructor still drains. This is the only reading in
which three things the rest of this document wants can all hold: that destruction
drains, that `sync_wait(awaitable, loop)` works, and that `stop()` is a safe thing
to call during shutdown. Had `stop()` gated execution, then the normal shutdown of
stopping first and destroying second would silently discard every queued
resumption and teardown, which is this phase's stated anti-goal; and a driving
`sync_wait` on a stopped loop would either spin at full CPU forever or deadlock,
since only a work item can set the flag it waits on.

`thread_pool` has no `run()` and therefore no `stop()`: its lifecycle is
construct, post, destroy, with `drain()` and `abandon()` as the explicit controls.
`run_loop` has no public `wake()`; `stop()` covers ending a loop and
`notify_waiters()` covers nudging a blocked waiter.

### `run_pending()` drains a bounded batch

It takes the queue's length once on entry and runs at most that many items.
Anything a work item posts during the drain waits for the next call.

Draining until empty would livelock a host loop: one item that posts another
means control never returns, and the platform's UI freezes. Easy to get right
now, miserable to retrofit once a consumer depends on greedy behavior.

### The host wake hook

`run_loop_options::on_wake` is an optional `work_item`-shaped callback that a
host supplies to nudge its own loop: a posted Win32 message, a write to a
Wayland eventfd. The loop knows nothing about platforms. With no hook, `run()`
waits on its own condition variable.

**It fires on the empty-to-non-empty transition, not on every post.** Per-post
delivery would mean a `PostMessage` or an eventfd write for every single
coroutine resumption, and it would make a hook that posts to its own loop
recurse without end. Edge-triggering fixes both: a hook's own post finds the
queue already non-empty and fires nothing. It also coalesces a burst into one
nudge, which is what a host wants.

It is invoked on the posting thread, outside the queue's lock, and it may not
throw, for the same reason work items may not. Supplying a hook and also calling
`run()` is permitted, not a mode error; the hook simply fires as well.

### No priorities

A pool costs nothing to create, so a consumer who must keep long jobs away from
latency-sensitive work makes two pools. Priorities inside one pool are an
interface commitment that every future scheduler would have to honor, for a
problem that composition already solves. This is a decision taken here, not a
user request, and the counter-case is a consumer who wants one pool sized to the
machine with mixed work in it; they are asked to make two smaller ones instead.

### Destruction drains; abandoning is explicit

`~thread_pool` and `~run_loop` run everything already queued, then stop.

The alternative was considered and rejected. Queued items are not necessarily
coroutine resumptions, since a `work_item` is a bare function pointer and an
opaque pointer, and consumers post whatever they like. But two of the three
kinds the library itself posts fail silently and unrecoverably if discarded: a
resumption leaves a coroutine suspended forever with its frame never freed, and
a cancelled chain's teardown posted to a home scheduler leaks the entire chain.
That second one contradicts a hazard the cancellable task page already
documents.

The cost of draining is that shutdown takes as long as the queued work, a work
item that posts more work extends it, and a resumption that blocks forever hangs
it. That is the right trade because the failure is loud, where the leak is
invisible.

For shutdown under duress there is an explicit escape: `abandon()` stops
accepting work, discards what is queued, and waits only for items currently
executing. Never the default, and documented as leaking any coroutine frames
whose resumptions were discarded.

### Two cross-posting schedulers cannot be shut down by destructors alone

Each destructor drains only its own queue, and that is not enough when two
schedulers post to each other, which is exactly the target usage. A chain parked
on the pool and cancelled with the loop as its home posts a teardown onto the
loop; a chain parked on the loop posts its continuation onto the pool. So a
drain on one can enqueue onto the other after that other has already been
destroyed, which is undefined behavior and strictly worse than the invisible leak
the drain was introduced to avoid.

Destructor draining therefore remains the behavior for a scheduler used alone,
and `drain()` is public so a consumer can quiesce a set of them: stop admitting
work at the application level, then call `drain()` on each in turn until every one
reports an empty queue, then destroy them. Both component pages must state this,
and the `main()` sketch above must show it rather than relying on declaration
order, which in that sketch destroys the loop first and would hit exactly this
hazard.

### Both components are compiled, not header-only

Neither has templates in its interface, so both get a `.hpp` declaration and a
`.cpp` definition, which is the convention `CLAUDE.md` documents and the library
has so far used only for `version.cpp`.

Two reasons beyond convention. It keeps `<thread>`, `<mutex>`,
`<condition_variable>` and the pool's internals out of every translation unit
that merely wants to name the type. And cppcheck only analyzes `src/*.cpp`, so
this is the first coroutine-adjacent code it will cover at all.

### The queue protocol is extracted and shared

`include/alt/detail/work_queue.hpp` holds the queue, its mutex, its condition
variable, the bounded drain, the non-arming notify, and **two distinct wake
signals, which must not be conflated**:

- A consumable one-shot latch. This is what `manual_scheduler::wake()` already
  is, documented as arming a flag that stays set until a waiter consumes it, and
  an existing test pins that behavior.
- A non-consumable closing state that releases every waiter and stays released.

The pool's shutdown needs the second. A one-shot latch consumed by one of N
waiting worker threads leaves the rest parked, and a spurious `false` out of
`run_one_or_wait` is read as completion by a driving `sync_wait` — precisely the
defect class this extraction exists to retire. Building the pool on the latch
alone would reintroduce it in a component whose failure mode is a hung
shutdown.
`manual_scheduler` is rebuilt on it, and `run_loop` and `thread_pool` are built
on it.

This modifies a shipped, tested component, which is a real risk and the reason
it is called out here rather than done quietly. It is worth it: that protocol
already shipped one lost-wakeup defect, found only because a later component
read the stale flag as completion. A second hand-written copy would be a second
chance to make the same mistake, in a component whose failure mode is a hung UI
thread. `manual_scheduler`'s public surface does not change, and its existing
tests are the regression check for the extraction.

### Construction returns `std::expected` from a factory

Creating a thread can fail, a constructor cannot report that without throwing,
and this project's guidelines prefer `std::expected` for recoverable errors. So
the constructor is private and construction goes through:

```cpp
static std::expected<thread_pool, std::error_code> create(std::size_t threads = 0);
```

with `0` meaning `hardware_concurrency()`.

**The pool stays non-movable, and that is compatible with `std::expected`.** This
was checked rather than assumed, because the obvious reading says a non-movable
type cannot live in an `expected` and forces a `unique_ptr` and a heap
allocation. It can: the factory constructs in place and returns a prvalue, the
caller binds a reference, and the address is stable for the `expected`'s
lifetime. Measured on GCC 15: the factory compiles, `*made` and
`made.value()` both yield references, and `&*made` is stable.

The non-movability then propagates, which is a feature rather than a cost. An
`expected` holding a non-movable value is itself non-movable, so a pool cannot be
moved out of the variable it was created in, and the dangling references that a
move would leave in `scheduler_ref` and in awaiters inside suspended frames are
prevented by the type system instead of by documentation. The price is that a
pool cannot be returned from a function or stored in a container, which is
appropriate for something that owns threads and has references taken to it.

`std::error_code` rather than a component-specific enum, which is a departure
from the `base64_error` precedent. That precedent is right for domain errors like
an invalid character, where an enum names the whole space. A thread that will not
start fails for a reason the OS already has a code for, usually a resource limit,
and discarding that reason would make the one failure mode here harder to
diagnose than it needs to be.

**Partial failure fails the whole creation.** If the fifth of eight threads
cannot start, the four already created are joined and the error is returned. A
pool silently running with fewer threads than asked for is a performance mystery
later, and the factory exists precisely so this can be reported instead of
guessed at.

`run_loop` keeps a public constructor. It spawns nothing and so has no failure to
report. The asymmetry is honest: it reflects a real difference between the two
components rather than a style choice.

### Thread pool shape

Fixed thread count, defaulting to `std::thread::hardware_concurrency()` and
settable through the factory, with a minimum of one. One shared queue. Threads are
`std::jthread`. Posting from a pool thread is supported and required, since a
coroutine resumed on the pool may post its own continuation there.

## Semantics and guarantees

1. **`run_loop` is serial across threads, and reentrant on one.** Two threads
   must not be inside its running members concurrently; that is a precondition
   failure, asserted in debug builds. One thread nesting them is permitted and
   supported, because the pattern is legitimate: a work item that calls
   `sync_wait(awaitable, loop)` drives the loop from inside a `run()` already on
   the stack below it, which is how a modal flow in a UI framework is written.
   The hazards are that each nested level consumes stack, and that a nested
   `run_pending()` computes its own batch bound independently of the outer one.
   Posting is unrestricted from any thread.
2. **`stop()` is sticky.** A `run_loop` stopped before `run()` is called returns
   immediately rather than blocking; this is what makes a shutdown race between
   the two safe. `stop()` is also idempotent.
3. **`post` after `stop()` is queued and runs if anything drains it**, including
   the destructor. `post` after `abandon()` is accepted and never runs, because
   `abandon()` stops accepting; that is not an error, since a coroutine
   completing concurrently with shutdown cannot reasonably be asked to check
   first, and it is the leak the `abandon()` documentation has to name.
4. **Neither component is copyable or movable.** Both own threads or a thread's
   loop, and references to them are held by `scheduler_ref` and by awaiters
   inside suspended coroutine frames.
5. **Both must outlive every suspension that targets them**, which is the
   existing `scheduler_ref` contract. A pool destroyed while a coroutine is
   parked awaiting a hop onto it is undefined; the drain-on-destruction behavior
   covers queued items, not references held elsewhere.

## Edge cases

| Case | Behavior |
|---|---|
| `run()` on an already-stopped loop | returns immediately |
| `drain()` or `run_one()` on a stopped loop | still runs items; `stop()` gates only `run()` |
| `sync_wait(awaitable, loop)` on a stopped loop | works; it drives `run_one_or_wait`, which is not gated |
| a work item calling `sync_wait(awaitable, loop)` on its own loop | permitted; nesting on one thread is supported |
| two rapid posts onto an empty loop with a wake hook | the hook fires once, on the first |
| a wake hook that posts to its own loop | fires once; the hook's own post finds a non-empty queue |
| `stop()` from inside a work item | the current item finishes, then `run()` returns |
| `stop()` from another thread while `run()` blocks | the loop wakes and returns |
| `run_pending()` with an empty queue | returns zero, does not block |
| a work item posting to its own loop during `run_pending()` | the new item waits for the next call |
| a work item posting during `run()` | runs in the same `run()` |
| `~thread_pool` with queued work | drains it, then joins |
| `~thread_pool` with a work item that posts more work | drains that too; shutdown is as long as the work |
| `abandon()` with queued coroutine resumptions | discarded; those frames leak, and this is documented |
| `thread_pool::create(0)` | `hardware_concurrency()` threads, clamped to at least one |
| `hardware_concurrency()` returning zero | clamped to one thread |
| a thread failing to start partway through creation | threads already created are joined, the error is returned, no pool exists |
| trying to move a created pool | ill-formed; the `expected` is non-movable because the pool is |
| posting from a pool thread | supported |
| two threads calling `run()` on one loop | precondition failure, asserted in debug |
| `on_wake` posting to the loop it belongs to | permitted; it is just another post |

## Testing strategy

Tests go in `tests/test_run_loop.cpp` and `tests/test_thread_pool.cpp`, each
registered in `tests/CMakeLists.txt`. `manual_scheduler`'s existing tests in
`tests/test_scheduler.cpp` are the regression check for the queue extraction and
must not be weakened to accommodate it.

Both components are concurrent, so the `debug-tsan` preset is a gate for this
phase, not a nicety.

- **`run_loop`, deterministic:** `stop()` before `run()`; `stop()` from inside an
  item; `run_pending()` bounded so a self-posting item does not livelock, which
  is the test that would fail against a greedy drain; `run_pending()` on an
  empty queue; `drivable_scheduler` satisfied, asserted with a `static_assert`.
- **`run_loop`, threaded:** `stop()` from another thread while `run()` blocks; the
  wake hook invoked on the posting thread, asserted by recording the thread id;
  the hook firing once rather than twice for two rapid posts, which is the test
  that distinguishes edge-triggered from per-post and would pass against either
  if written only as "the hook fired"; `sync_wait(awaitable, loop)` driving a
  stopped loop; one thread nesting `sync_wait` inside a `run()` work item.
- **Cross-posting shutdown:** a cancelled chain parked on the pool with the loop
  as its home, quiesced by alternating `drain()` until both are empty, asserting
  the teardown ran and no frame leaked. This is the case destructors alone cannot
  handle.
- **`thread_pool`:** every posted item runs; posting from a pool thread; drain on
  destruction, asserted by counting completions after the destructor returns;
  `abandon()` discarding queued work while finishing in-flight work; thread count
  clamped at one for both zero and a zero `hardware_concurrency`; the factory's
  error path, asserted with a `static_assert` that the pool is neither copyable
  nor movable and that the returned `expected` is not movable either.
- **Integration:** a `cancellable_task` that hops to the pool and back to the
  loop, cancelled while parked on each, asserting the resumption check from the
  issue #4 fix still holds through real schedulers rather than the test ones.

## Files changed

| File | Change |
|------|--------|
| `include/alt/detail/work_queue.hpp` | New |
| `include/alt/run_loop.hpp` | New |
| `src/run_loop.cpp` | New |
| `include/alt/thread_pool.hpp` | New |
| `src/thread_pool.cpp` | New |
| `include/alt/scheduler.hpp` | `manual_scheduler` rebuilt on the shared queue |
| `tests/test_run_loop.cpp` | New |
| `tests/test_thread_pool.cpp` | New |
| `tests/CMakeLists.txt` | Register the two test files |
| `docs/run_loop.md` | New component page |
| `docs/thread_pool.md` | New component page |
| `docs/scheduler.md` | Point at the two real schedulers |
| `README.md` | Add both components |
| `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md` | Mark phase 3 shipped |

`src/` is globbed, so the two new translation units are picked up after a
configure. The version bump is a separate commit and its part is the author's to
choose.

## Left to the builder

- Whether the shared queue exposes the bounded drain as a count or an iterator
  over claimed items.
- Whether `thread_pool` holds a `std::vector<std::jthread>` or a fixed array
  sized at construction.
- The exact assertion mechanism for the serial precondition on `run_loop`.
- Naming inside `detail/work_queue.hpp`, which is internal.

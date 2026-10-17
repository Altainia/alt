# run_loop

**Header:** `<alt/run_loop.hpp>`

A serial executor for one thread, usable in either of two modes: it can own the
thread it runs on, or it can be a guest inside a platform loop that something
else owns. It is an [`alt::scheduler`](scheduler.md), so `alt::resume_on` hops
onto it and a cancelled chain can name it as its home.

```cpp
#include <alt/run_loop.hpp>
```

## Interface

```cpp
struct run_loop_options
{
    work_item on_wake{};
};

class run_loop
{
public:
    run_loop() = default;
    explicit run_loop(run_loop_options options) noexcept;

    run_loop(const run_loop&)            = delete;
    run_loop& operator=(const run_loop&) = delete;
    run_loop(run_loop&&)                 = delete;
    run_loop& operator=(run_loop&&)      = delete;

    ~run_loop();

    void post(work_item w);

    std::size_t drain();
    void        run();
    void        stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept;

    std::size_t run_pending();
    bool        run_one();
    bool        run_one_or_wait(const std::atomic<bool>* stop = nullptr);
    void        notify_waiters();

    [[nodiscard]] bool empty() const;
};
```

- `post(w)` queues an item and, if a wake hook was supplied, nudges the host.
  Safe from any thread.
- `drain()` runs queued items until the queue is empty and returns how many ran.
  Items queued by the items it runs are run too.
- `run()` drains, and blocks when idle, until `stop()`.
- `stop()` makes `run()` return after the current item. Sticky and idempotent.
  `stopped()` reports whether it has been called.
- `run_pending()` runs a bounded batch without blocking and returns how many
  ran.
- `run_one()` runs the oldest queued item and returns `true`, or returns
  `false` if the queue was empty.
- `run_one_or_wait(stop)` blocks until an item can be run, then runs it, and
  returns `false` if the wait ended for another reason. It behaves as
  [`alt::manual_scheduler`](scheduler.md#manual_scheduler)'s does.
- `notify_waiters()` prompts a blocked waiter to re-test its condition and arms
  nothing.
- `empty()` is `true` when nothing is queued.

`run_one`, `run_one_or_wait` and `notify_waiters` make `run_loop` a
`drivable_scheduler`, so the driving overload of [`alt::sync_wait`](sync_wait.md)
accepts it. `alt::thread_pool` is deliberately not drivable; see
[thread_pool](thread_pool.md#not-a-drivable_scheduler) for what that means when
choosing between them.

`run_loop` is neither copyable nor movable. It owns a thread's loop, and
references to it are held by `alt::scheduler_ref` and by awaiters inside
suspended coroutine frames.

## Mode one: the loop owns its thread

`run()` is the thread's main function. It runs items as they arrive and sleeps
on an internal condition variable when there are none, until another thread
calls `stop()`.

```cpp
void say(void*) noexcept
{
    std::puts("ran on the loop thread");
}

void stop_loop(void* state) noexcept
{
    static_cast<alt::run_loop*>(state)->stop();
}

alt::run_loop loop;
std::thread   worker{[&loop] { loop.run(); }};

loop.post({.invoke = &say, .state = nullptr});
loop.post({.invoke = &stop_loop, .state = &loop});   // queued behind say, so say has run

worker.join();   // run() returned after stop_loop ran
```

The stop is posted as an item rather than called from the main thread, on
purpose. `run()` checks for a stop before it takes the next item, so a
`loop.stop()` from the main thread right after the first `post` could land
before the loop thread had run anything, and the item would then run in the
loop's destructor, on the main thread. Queued in order, the stop item cannot run
until `say` has.

Declare the loop before the thread, as above, so that the loop outlives it.

## Mode two: the loop is a guest

A program whose thread belongs to a Win32 message pump or a Wayland dispatch
loop cannot hand that thread to `run()`. Instead it supplies a wake hook that
nudges its own loop, and calls `run_pending()` from the handler for that nudge.

```cpp
alt::run_loop loop{alt::run_loop_options{
    .on_wake = {.invoke = [](void*) noexcept { /* post a message to the host loop */ },
                .state  = nullptr}}};

// Later, in the host loop's handler for that message:
const std::size_t ran = loop.run_pending();

// Then re-arm the host, because the wake hook will not. run_pending() is
// bounded, and on_wake only fires on an empty-to-non-empty transition, so a
// post that lands while the queue is still non-empty fires nothing. Without
// this check that item sits queued with no nudge pending, and the loop stalls
// until some unrelated host event happens to arrive.
if(!loop.empty())
{
    // post the same message to the host loop that on_wake would have posted
}
```

The hook knows nothing about the loop and the loop knows nothing about the
platform. The hook is invoked on the posting thread, outside the queue's lock,
and may not throw, for the same reason a `work_item` may not.

## Hazards

### The wake hook is edge-triggered

`on_wake` fires when a post makes the queue go from empty to non-empty, not on
every post. A host that expects one nudge per post will under-wake, and one
nudge must therefore be enough to drain everything queued behind it. A
handler that calls `run_pending()` must also check `empty()` afterwards and
re-arm the host itself when work remains, because that call is bounded (next
section). Skipping that check is not under-waking, it is a **stall**: a
cross-thread post that lands after `run_pending()` took its length snapshot,
while the queue is still non-empty, fires no hook, so the item waits with no
nudge pending until some unrelated host event arrives. Per-post delivery would cost a platform
wakeup for every coroutine resumption, and would make a hook that posts to its
own loop recurse without end.

### `run_pending()` is bounded

The queue's length is taken once on entry, and `run_pending()` runs at most that
many items. An item that posts more work therefore cannot keep control: the new
work waits for the next call. A caller who expects `run_pending()` to empty the
queue and writes `while(...)` around it on that assumption will write a loop that
does not terminate in a guest loop, and one that starves the host's own events.
Use `drain()` to run to empty when the host can afford it, and check `empty()`
after `run_pending()` when it cannot.

### `stop()` gates `run()` and nothing else

A stopped loop is not inert. `drain()`, `run_pending()`, `run_one()` and
`run_one_or_wait()` still run items after `stop()`, and the destructor still
drains. This is deliberate: were `stop()` a gate on execution, the ordinary
shutdown of stopping and then destroying would silently discard every queued
coroutine resumption and every cancelled chain's teardown, leaking the frames
with no diagnostic.

### Serial across threads, reentrant on one

Two threads must not be inside the loop's running members (`run`, `drain`,
`run_pending`, `run_one`, `run_one_or_wait`) at once. That is a precondition,
and only part of it is checked: debug builds assert it in `run`, `run_pending`
and `run_one_or_wait`. `drain()` and `run_one()` carry the same precondition
with nothing to enforce it, so calling either from a second thread while
another thread is inside the loop is silently undefined behavior in every build.
`post()` is the exception: it is safe from any thread, and is how work reaches
the loop.

One thread nesting those members is supported, because that is how a modal flow
is written: a work item calls `sync_wait(awaitable, loop)`, which drives the
loop from inside a `run()` that is already below it on the stack. Each nested
level consumes stack, and a nested `run_pending()` computes its own batch
bound.

### Driving the loop from inside a work item means catching

A `work_item` callback may not throw, and the type enforces it. `sync_wait`
can throw: it rethrows whatever exception the awaited work ended with. So a
work item that calls `sync_wait` must catch, or an exception from the awaited
work escapes a `noexcept` callback and terminates the process. This is not
hypothetical: clang-tidy's `bugprone-exception-escape` found it in this
library's own nesting test.

```cpp
alt::task<int> compute(alt::run_loop& loop)
{
    co_await alt::resume_on(loop);
    co_return 42;
}

struct modal_state
{
    alt::run_loop* loop;
};

void run_modal(void* state) noexcept
{
    auto* const s = static_cast<modal_state*>(state);
    try
    {
        const int value = alt::sync_wait(compute(*s->loop), *s->loop);
        std::printf("result %d\n", value);
    }
    catch(const std::exception& e)
    {
        std::printf("failed: %s\n", e.what());
    }
}

alt::run_loop loop;
modal_state   state{&loop};
loop.post({.invoke = &run_modal, .state = &state});
loop.drain();
```

### The loop must outlive every suspension targeting it

A `resume_on(loop)` stores a pointer to the loop, and a `scheduler_ref` holds
one too. Nothing extends the loop's lifetime for them. This is where a caller
is most tempted to treat a loop as a local: a loop that goes out of scope while
a coroutine is suspended on it leaves that coroutine permanently unresumed.

### Declare what work items point at before the loop

Anything a posted work item's `state` points at must outlive the loop, which
means it must be declared before it. The destructor drains, so items run while
the loop is being destroyed, and by then anything declared after the loop in the
same scope is already gone.

```cpp
{
    alt::run_loop loop;
    counter       c;               // WRONG: destroyed before the loop's destructor drains
    loop.post({.invoke = &bump, .state = &c});
}

{
    counter       c;               // right: outlives the loop
    alt::run_loop loop;
    loop.post({.invoke = &bump, .state = &c});
}
```

This is a use-after-free that neither AddressSanitizer nor ThreadSanitizer
reports when both objects share a scope: the stack slot is still mapped, so
stack-use-after-scope does not fire. The only defense is the declaration order.

## Shutting down

For a `run_loop` on its own, shutdown is simple: call `stop()` if `run()` is
active, join the thread, and let the destructor drain. Because the loop is
serial, once `drain()` returns on the owning thread and nothing else is posting,
`empty()` is a reliable statement that nothing remains.

That stops being true as soon as another scheduler posts to the loop. A
`thread_pool` worker that is mid-item is invisible to the loop's `empty()` and
may post to it after any drain. Two schedulers that post to each other cannot be
shut down by their destructors alone, and no sequence of `drain()` calls
establishes quiescence between them. A cancelled chain whose home scheduler is
one of them is exactly how the situation arises. The remedy is to destroy the
pool before the loop, which means declaring the loop first, and to stop any
chains that hop in both directions. The pool's destruction joins its workers, so
nothing can post to the loop afterwards, and the loop's destructor then drains
whatever the pool left behind. The full reasoning, and the caveat that the
library does not report when a cancelled chain's teardown has finished, are in
[thread_pool](thread_pool.md#shutting-down-two-schedulers-that-post-to-each-other).

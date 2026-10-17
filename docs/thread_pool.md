# thread_pool

**Header:** `<alt/thread_pool.hpp>`

A fixed set of threads running work items from one queue. It is an
[`alt::scheduler`](scheduler.md), so `alt::resume_on` hops onto it and a
cancelled chain can name it as its home.

```cpp
#include <alt/thread_pool.hpp>
```

Deliberately plain: one shared queue, no work stealing, and no priorities.
Two pools cost nothing to create, so a consumer who must keep long jobs away
from latency-sensitive work makes two pools rather than asking one to arbitrate.

## Interface

```cpp
class thread_pool
{
public:
    [[nodiscard]] static std::expected<thread_pool, std::error_code>
    create(std::size_t threads = 0);

    thread_pool()                              = delete;
    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&)                 = delete;
    thread_pool& operator=(thread_pool&&)      = delete;

    ~thread_pool();

    void post(work_item w);

    std::size_t drain();

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] bool        empty() const;
};
```

(The class also has a public constructor taking a private passkey type. Only
`create()` can call it, so it is not part of the API.)

- `create(threads)` starts a pool. Zero means
  `std::thread::hardware_concurrency()`, and the result is clamped to at least
  one, including when that query returns zero.
- `post(w)` queues an item. Safe from any thread, including a pool thread.
- `drain()` **runs queued items on the calling thread** and returns how many
  ran. It is not a way to wait for the workers. See the
  [shutdown section](#shutting-down-two-schedulers-that-post-to-each-other).
- `thread_count()` is how many threads the pool started. Always at least one.
- `empty()` is `true` when nothing is queued. An item a worker is currently
  running is not counted.

## Construction can fail

Starting a thread can fail, and a constructor cannot report that without
throwing, so construction goes through `create()`, which returns
`std::expected<thread_pool, std::error_code>`. The error carries the reason the
OS gave. A thread that fails to start fails the whole call: threads already
started are released and joined, and the error is returned. A pool silently
running with fewer threads than requested is a performance mystery later, so it
is reported instead.

```cpp
auto made = alt::thread_pool::create(4);
if(!made)
{
    std::fprintf(stderr, "no pool: %s\n", made.error().message().c_str());
    return;
}
alt::thread_pool& pool = *made;

pool.post({.invoke = &do_work, .state = &job});
```

A `std::bad_alloc` raised while creating threads is reported as
`std::errc::resource_unavailable_try_again`, the same code the OS gives when it
has run out of threads. The error code therefore does not distinguish
out-of-memory from thread-limit exhaustion.

### The pool cannot be moved, and neither can its `expected`

`thread_pool` is neither copyable nor movable, and has no default constructor.
The factory constructs the pool in place inside the `std::expected`, and the
caller binds a reference to it, as above. The address of that reference is
stable for as long as the `expected` lives.

The non-movability propagates to the `expected` itself, which is the point:
references to the pool are held by `alt::scheduler_ref` and by awaiters inside
suspended coroutine frames, and a pool moved out from under them would leave
those references dangling. The price is that a pool cannot be returned from a
function or stored in a container. A function that needs to give a pool to its
caller should take the pool by reference, or let the caller create it.

## Not a `drivable_scheduler`

`thread_pool` has no `run_one`, `run_one_or_wait` or `notify_waiters`, so it
does not satisfy `drivable_scheduler`, and the driving overload of
[`alt::sync_wait`](sync_wait.md) will not select it. Only the blocking overload,
`sync_wait(awaitable)`, applies.

That is the right behavior for a pool. The pool's own threads are already
running its work, so the calling thread has nothing to pump and should simply
park. [`alt::run_loop`](run_loop.md) *is* drivable, and is the right choice when
the thread that waits is also the thread the work must run on. With
`sync_wait(task, loop)` that thread pumps the loop itself while it waits.

```cpp
alt::task<int> on_pool(alt::thread_pool& pool)
{
    co_await alt::resume_on(pool);
    co_return 7;
}

const int value = alt::sync_wait(on_pool(pool));   // blocking overload; parks
```

## Hazards

### Destruction drains, so shutdown takes as long as the queued work

Queued items may be coroutine resumptions, or a cancelled chain's teardown
posted here as its home scheduler, and discarding either leaks frames with no
diagnostic. So the destructor closes the queue with a *draining* close: items
already queued still run, on the workers, and only then are the workers joined.
The cost is that shutdown takes as long as the queued work. A loud wait is
better than a silent leak.

### No priorities; make two pools

There is one queue and it is first-in, first-out. Long jobs queued ahead of
short ones delay them. Create a second pool for the latency-sensitive work.

### The pool must outlive every suspension targeting it

This is `alt::scheduler_ref`'s existing contract. Draining at destruction covers
queued items, not references held elsewhere: a coroutine suspended on something
other than the pool's queue, which later tries to `resume_on` the destroyed
pool, is a use-after-free.

### Declare what work items point at before the pool

Anything a posted work item's `state` points at must outlive the pool, so
declare it *before* the pool. The destructor runs queued items and joins the
workers, and that must happen while the state they touch is still alive.

```cpp
{
    auto made = alt::thread_pool::create(2);   // WRONG: declared before what its items touch
    std::latch done{1};
    pool_fence fence{&done};
    (*made).post({.invoke = &signal, .state = &fence});
}   // fence and latch are destroyed first; the destructor then runs items that use them

{
    std::latch done{1};                        // right: outlive the pool
    pool_fence fence{&done};
    auto made = alt::thread_pool::create(2);
    (*made).post({.invoke = &signal, .state = &fence});
}
```

Neither ThreadSanitizer nor AddressSanitizer's stack-use-after-scope flags the
first form when both objects share a scope, so it can pass every sanitizer run
and still be a use-after-free. This bit this library's own test suite while the
pool was being written. The only defense is the declaration order.

### A work item that calls `sync_wait` must catch

A `work_item` callback may not throw, but `sync_wait` can, rethrowing whatever
the awaited work ended with. A work item that blocks on `sync_wait` from a pool
thread must catch, or the exception escapes a `noexcept` callback and
terminates the process. See
[run_loop](run_loop.md#driving-the-loop-from-inside-a-work-item-means-catching)
for the pattern. (Blocking a pool thread on work that needs a pool thread can
also deadlock a small pool.)

## Shutting down two schedulers that post to each other

For a `run_loop` on its own, draining until empty is a sound way to quiesce,
because the loop is serial. **It is not sound for a pool.** `drain()` plus
`empty()` cannot establish that a pool is quiescent: a worker that is mid-item
is invisible to `empty()`, and may still post afterwards, including back onto a
`run_loop` the caller has already drained. An alternating loop that calls
`drain()` on each scheduler until all report empty therefore has a window in
which both look empty and one is about to receive work.

Only destruction can establish quiescence for a pool. The destructor closes the
queue with a draining close, so queued items still run, and then joins the
workers, so none can be mid-item afterwards. When two schedulers post to each
other, destructors alone cannot shut them down either: each drains only its own
queue, and draining one can enqueue onto the other after it is gone. A cancelled
chain with one of them as its home is exactly how this arises.

The remedy rests on destruction order, and it is sound because of two facts:

- `~thread_pool` closes the queue with a draining close and joins the workers.
  Once it returns, no worker is running, so nothing can post to the loop any
  more. That is why the pool is the quiescence point.
- `~run_loop` drains rather than discards, because queued items may be coroutine
  resumptions or a cancelled chain's teardown posted there as its home. So
  whatever the pool posted on its way down still runs when the loop is destroyed
  afterwards.

So destroy the pool first, then the loop. That is a declaration-order
requirement: **declare the loop before the pool**, so that reverse-order
destruction gives pool-then-loop. It is the same rule as
[declaring what work items point at before the
pool](#declare-what-work-items-point-at-before-the-pool), applied to two
schedulers. Getting it backwards destroys the loop first, and the
pool's drain-down then posts into a destroyed loop, which is a use-after-free
and not merely stale work.

```cpp
alt::run_loop loop;                        // declared first, destroyed last
auto          made = alt::thread_pool::create(2);
```

This holds provided the items left on the loop do not post back to the pool,
which will already be gone by then. If chains hop in both directions, stop them
from hopping first: call `request_stop()` on their `cancellation_handle`s, which
makes each chain tear itself down at its next suspension point instead of
continuing. Be aware that the library does not report when that teardown has
finished. `stop_requested()` says only that the request was made. A caller who
needs to observe completion must supply the mechanism, for example a counter
the coroutine decrements from a destructor of one of its locals as the frame
dies. `tests/test_scheduler_integration.cpp` does exactly this, with a
`frame_tracker` local whose destructor decrements a live-frame count and a
`teardown_recorder` local that notes the destroying thread. Do not rely on
`drain()` in a loop for this.

`drain()` is useful for what it is: running queued items on the calling thread,
for instance to help finish work before a pool is destroyed.

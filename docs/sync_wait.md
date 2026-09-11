# sync_wait

**Header:** `<alt/sync_wait.hpp>`

The only blocking entry point in the coroutine components: runs an awaitable
to completion and returns its result on the calling thread. `alt::task`
deliberately has no `get()` — `sync_wait` is how a coroutine chain is run from
non-coroutine code.

```cpp
#include <alt/sync_wait.hpp>
```

## Interface

```cpp
template<awaitable A>
/* result */ sync_wait(A&& a);

template<awaitable A, drivable_scheduler S>
/* result */ sync_wait(A&& a, S& scheduler);
```

Both overloads return the awaitable's result. An rvalue reference result has its
reference removed and is returned by value, since the temporary that produced it
is gone by the time `sync_wait` returns; that is reference removal only, not full
decay, so cv-qualification and array or function types are left alone. An lvalue
reference result is preserved. Anything that escaped the awaitable is rethrown.

## The blocking overload

```cpp
alt::task<int> answer()
{
    co_return 42;
}

const int value = alt::sync_wait(answer());
```

The calling thread parks on a semaphore until the awaitable completes. That
is fine as long as nothing the awaitable does needs *this* thread to make
progress.

## The driving overload

```cpp
alt::manual_scheduler worker;

alt::task<int> compute()
{
    co_await alt::resume_on(worker);
    co_return 42;
}

const int result = alt::sync_wait(compute(), worker);
```

The calling thread pumps `scheduler` instead of parking, until the awaitable
signals completion. It passes its own completion flag to
`run_one_or_wait(stop)`, so the wait ends on that flag directly and nothing is
left armed in the scheduler afterwards.

## Choosing between them

**The blocking overload deadlocks on any awaitable that needs the calling
thread to pump a scheduler for it to make progress.** `alt::manual_scheduler`
runs nothing until something calls `run_one()`, `run_one_or_wait()`, or
`run_all()` on it — if the calling thread is the only thread that will ever
do that, and it has instead called the blocking `sync_wait(a)` and parked,
the work sits in the queue forever and the semaphore never releases.

Any single-threaded test built around `alt::manual_scheduler` — which is
every such test in this library — must use the driving overload,
`sync_wait(a, scheduler)`, passing the same scheduler the coroutine under
test posts to. The blocking overload is for awaitables whose progress does
not depend on the thread calling `sync_wait`, typically because the work runs
on a separate thread pool or I/O reactor that pumps itself.

### The scheduler must outlive the call, with a little margin

Completion publishes the flag the pumping thread is waiting on and then
notifies the scheduler. Those are two steps, and only the first has to happen
before `sync_wait` returns. So when the work completes on some other thread,
that thread can still be inside the scheduler after the call has returned.

Destroying the scheduler the moment `sync_wait` returns races that
notification. Keep it alive until the threads that might complete the work
have been joined. In practice a scheduler outlives the work it schedules
anyway, and `alt::scheduler_ref` already requires exactly this, but the
driving overload is the place where a caller is most tempted to treat the
scheduler as a local whose job ends when the wait does.

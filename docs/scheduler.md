# scheduler

**Header:** `<alt/scheduler.hpp>`

The scheduling primitive the coroutine components are built on, plus two
concrete schedulers and the awaitable that hops a coroutine onto one.

```cpp
#include <alt/scheduler.hpp>
```

## `work_item` and the `scheduler` concept

```cpp
struct work_item
{
    void (*invoke)(void* state) noexcept = nullptr;
    void* state                          = nullptr;
};

template<typename S>
concept scheduler = requires(S& s, work_item w) { s.post(w); };
```

A scheduler's entire contract is one operation: `post(work_item)`. A
`work_item` is a function pointer plus an opaque `void*`, so posting one
allocates nothing. Both resuming a coroutine (`alt::resume_on`) and
destroying a cancelled frame elsewhere are expressed as work items, so a
scheduler implementation never needs to know coroutines exist.

**The callback may not throw**, and the type enforces it: a potentially
throwing function will not convert. There is no error channel in a `work_item`,
so an escaping exception would surface in whichever thread happened to be
draining the queue, with no relationship to whoever posted the item, and would
abandon anything that item had queued behind it. A callback that can fail has
to handle that itself.

`alt::resume_on` is also a cancellation observation point. When the awaiting
coroutine belongs to an `alt::cancellable_task` chain and a stop has been
requested by the time the scheduler gets round to the resumption, the chain is
destroyed instead of resumed. There is no cost to this when no stop was
requested, and none at all outside a cancellable chain, where there is no
context to consult.

One consequence is worth stating for `alt::resume_on`. Resuming any of this
library's coroutines cannot throw, because every promise here stores an escaped
exception rather than rethrowing it. A foreign coroutine type whose promise
rethrows would terminate when resumed through a scheduler, which is the honest
outcome: the only other destination for that exception is an arbitrary pumping
thread.

```cpp
template<typename S>
concept drivable_scheduler = scheduler<S> && requires(S& s, const std::atomic<bool>* stop) {
    { s.run_one() } -> std::same_as<bool>;
    { s.run_one_or_wait(stop) } -> std::same_as<bool>;
    s.notify_waiters();
};
```

`drivable_scheduler` additionally lets a thread pump the queue itself. It is
what the driving overload of [`alt::sync_wait`](sync_wait.md) requires, so
that a blocked thread never has to just park while its own work sits unrun in
the queue.

## `scheduler_ref`

```cpp
class scheduler_ref
{
public:
    scheduler_ref() noexcept;

    template<scheduler S>
    explicit scheduler_ref(S& s) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

    void post(work_item w) const;
};
```

A non-owning, type-erased reference to any `scheduler`: a pointer plus a
`post` function pointer. Allocates nothing and is trivially copyable. A
default-constructed `scheduler_ref` is empty and converts to `false`; calling
`post` on one is a precondition violation. This is the type
[`alt::cancellable_task`](cancellable_task.md)'s `start_options::home` holds,
so that it can accept any scheduler without becoming a template itself.

```cpp
alt::manual_scheduler ui;
const alt::scheduler_ref ref{ui};
```

## `trampoline_scheduler`

```cpp
class trampoline_scheduler
{
public:
    void post(work_item w) const;
};
```

Runs a posted item immediately, unless this thread is already inside a
`trampoline_scheduler::post` call, in which case the item is queued and the
outermost call drains the queue (including anything queued by items it runs)
before returning. This is what keeps a chain of synchronous resumptions from
growing the call stack: each resume posts its continuation rather than
calling it directly, so the stack unwinds one level before the next
continuation runs.

The queue is **per-thread, not per-instance** — two `trampoline_scheduler`
objects used on the same thread share one drain:

```cpp
alt::trampoline_scheduler a;
alt::trampoline_scheduler b;

void inner(void*) { std::puts("inner (queued)"); }
void outer(void*)
{
    std::puts("outer starts");
    b.post({.invoke = &inner, .state = nullptr});  // queued: a is already draining
    std::puts("outer ends — inner has not run yet");
}

a.post({.invoke = &outer, .state = nullptr});
// prints: outer starts / outer ends — inner has not run yet / inner (queued)
```

This is harmless — a work item is self-contained — but it means a nested post
through a *different* `trampoline_scheduler` instance on the same thread is
still deferred, not run inline. There is no plain always-run-inline
scheduler in this library, since the only difference from
`trampoline_scheduler` would be that it can overflow the stack.

## `manual_scheduler`

```cpp
class manual_scheduler
{
public:
    void post(work_item w);

    bool run_one();
    bool run_one_or_wait(const std::atomic<bool>* stop = nullptr);
    std::size_t run_all();

    [[nodiscard]] bool empty() const;

    void notify_waiters();
    void wake();
};
```

`run_one_or_wait`'s optional `stop` flag joins its wait condition. That lets a
caller waiting on something this scheduler knows nothing about be released
without arming any state here, which is how the driving `alt::sync_wait` waits
on its own completion flag.

`notify_waiters` and `wake` are not the same thing, and the difference matters.
`wake` is a latch: it arms a flag that stays set until a waiter consumes it, so
it is the right tool for a caller ending a pump loop of their own. Because it
persists, a `wake` nobody consumes is observable by the next call.
`notify_waiters` arms nothing and only prompts a blocked waiter to re-test its
condition, so it cannot be observed later. Use it when the condition being
waited on lives outside this scheduler.

Runs nothing until told to. This is the deterministic scheduler for tests:
post work from any thread, then decide on the pumping thread exactly when
each item runs. Thread-safe.

- `run_one()` runs the oldest queued item and returns `true`, or returns
  `false` immediately if nothing is queued.
- `run_one_or_wait(stop)` blocks until an item can be run, then runs it. It
  returns `false` instead when the wait ended for another reason: the optional
  `stop` flag became true, or `wake()` released it with nothing queued. The
  driving `alt::sync_wait` overload passes its own completion flag as `stop`,
  which is how it learns the awaited work finished without needing any state
  here.
- `run_all()` runs `run_one()` in a loop until the queue is empty and returns
  how many items ran.
- `notify_waiters()` prompts a blocked waiter to re-test its condition and arms
  nothing.
- `wake()` releases a thread blocked in `run_one_or_wait()` without queueing
  anything, and stays armed until a waiter consumes it.

```cpp
alt::manual_scheduler worker;

alt::task<int> compute()
{
    co_await alt::resume_on(worker);
    co_return 42;
}

const int result = alt::sync_wait(compute(), worker);  // pumps worker itself
```

## `resume_on`

```cpp
template<scheduler S>
[[nodiscard]] auto resume_on(S& s) noexcept;
```

The awaitable for hopping a coroutine onto a scheduler:
`co_await alt::resume_on(pool)` suspends the current coroutine and posts its
resumption to `pool`.

```cpp
alt::task<void> handler()
{
    co_await alt::resume_on(worker_pool);  // now running on a worker
    do_work();
    co_await alt::resume_on(ui_scheduler); // back on the UI thread
    update_ui();
}
```

## Lifetime

**The scheduler, and anything a work item's state pointer points at, must
outlive the suspension it was posted for.** `alt::resume_on(s)` stores a
pointer to `s`, not a copy, and the posted work item's `state` is typically
the address of the coroutine frame itself — nothing about the scheduler
interface extends anyone's lifetime. A `manual_scheduler` that goes out of
scope, or is destroyed, while work is still queued for it leaves that queued
work permanently unrun; a coroutine suspended waiting on it never resumes.

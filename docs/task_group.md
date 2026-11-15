# task_group

**Header:** `<alt/task_group.hpp>`

The owner's teardown point for [`alt::cancellable_task`](cancellable_task.md)s. A
group owns the one stop signal every task spawned through it shares, and counts
the tasks still alive, so that "everything I started is gone" has an answer a
caller can wait for.

```cpp
#include <alt/task_group.hpp>
```

## Interface

```cpp
class task_group
{
public:
    using join_awaiter = detail::completion_counter::join_awaiter;

    task_group();
    ~task_group();

    task_group(const task_group&)            = delete;
    task_group& operator=(const task_group&) = delete;
    task_group(task_group&&)                 = delete;
    task_group& operator=(task_group&&)      = delete;

    cancellation_handle spawn(cancellable_task t, const start_options& options = {});

    bool                             request_stop() noexcept;
    [[nodiscard]] bool               stop_requested() const noexcept;
    [[nodiscard]] std::size_t        outstanding() const noexcept;
    [[nodiscard]] join_awaiter       join() noexcept;   // co_await yields alt::wait_result
};
```

- `spawn` starts the task immediately, up to its first suspension point, exactly
  as `alt::start` does, and returns that task's own handle. Cancelling the handle
  cancels only that task; `request_stop()` cancels all of them. It is not
  `[[nodiscard]]`, unlike `alt::start`, because the group is the handle an owner
  keeps, so dropping the per-task one is the common case.
- `request_stop()` asks every task to stop, and returns `true` when this call was
  the one that requested it. A task spawned afterwards stops at its first
  suspension point.
- `outstanding()` is how many spawned tasks are still alive: a snapshot, as stale
  on return as any other.
- `co_await group.join()` completes when none is outstanding, and does not suspend
  at all if none is. It yields [`alt::wait_result`](async_event.md#the-result-of-a-wait):
  a value when the count reached zero, and `std::errc::operation_canceled` when
  the awaiting chain was cancelled. Any number of coroutines may join at once and
  all are released together.
- A task spawned into the group must not `co_await group.join()`. Its own
  completion is part of the outstanding count and is retired only when its frame
  is destroyed, so such a join can never reach zero and can only end in
  `std::errc::operation_canceled`, through the group's own stop. A supervisor
  that waits for the group belongs outside it.

`spawn` racing a task's completion on another thread is tested, under
ThreadSanitizer. The other members' thread safety is not separately tested: it
follows from `std::stop_source` and from the counter's own contract, and only the
destructor is excluded from it. The group is neither copyable nor movable, since
the completion hook inside every task holds its address.

## What the group supplies, and what stays yours

Of the four fields of `alt::start_options`, the group owns two. `token` carries
the group's stop signal and `on_completion` carries the hook that counts a task
as gone. Passing either of your own is a mistake, and debug builds assert it.

`home` and `on_exception` are yours and pass through untouched, per task.

- To cancel the group from a signal you already have, register a
  `std::stop_callback` on that signal whose callback calls `group.request_stop()`.
- To be told when one task finishes, use an `alt::scope_exit` local in the task's
  body. It runs on both the normal and the cancelled path.

## Teardown is three steps

```cpp
group.request_stop();
co_await group.join();
// now destroy the primitives the tasks used, and the group itself
```

It is finite, and that is the point of the order. A task parked on a primitive
that nobody will ever set would make a join wait forever, and destroying the
primitive first to release it would be destroying something a live task still
holds. The stop breaks that circle: a parked waiter is released by its **own stop
callback**, not by the primitive's destruction, so after `request_stop()` every
task is on its way out without anything having to be destroyed first. The join
then waits only for the tail of that, which is frames being torn down, possibly on
a `home` scheduler that is yet to be drained.

### Declare the primitives before the group

```cpp
alt::async_event loaded;    // first
alt::task_group  group;     // after, so it is destroyed first
```

Locals and members are destroyed in reverse order of declaration, so a group
declared after the primitives its tasks use is destroyed before them, while those
primitives still exist. The reason to want that order is what the reverse does to
the one diagnostic there is. With the order reversed and no `home`
scheduler, the primitive is destroyed first, which releases the parked waiter,
whose teardown retires the count. The group is then destroyed with
`outstanding() == 0`, and the destructor's assert does not fire at all. A scratch
program in a debug build confirmed it: one task parked on an `alt::async_event`,
the group never stopped or joined. Declared event first, it aborted on the
destructor's assert; declared group first, it exited cleanly. So the reversed
order can silence the only signal that the group was destroyed unjoined, and the
prescribed order cannot.

Nothing enforces the order at compile time, and the sanitizers were measured not
to tell the two orders apart. In a scratch program with a `home` scheduler that is
drained after the scope ends, so a task is still live when the group is destroyed,
both orders behaved identically: a debug build aborted on the destructor's assert
(under ASan+UBSan and under TSan); with `NDEBUG` AddressSanitizer reported
`stack-use-after-scope` in `completion_counter::complete()` for both orders, and
ThreadSanitizer reported nothing for either. So in that setup the assert fires
for either order and is no signal of the *order*. What those catch is the group
destroyed unjoined, which is the mistake the order merely decides whether the
assert can see. The order has to be read for.

## From code that is not a coroutine

The owner of a group is often a window or a service, not a coroutine. Use
`alt::sync_wait` with the loop that the tasks' `home` or resumption targets, so
that the loop keeps being pumped while the owner waits. That is reentrant on a
loop, which is the route a modal flow takes, and it **must catch**: `sync_wait`
rethrows, and a `sync_wait` inside a work item that throws terminates the
process. See [run_loop](run_loop.md#driving-the-loop-from-inside-a-work-item-means-catching).

```cpp
// struct window { alt::async_event loaded; alt::task_group group; ... };
//                 ^ the primitives its tasks use come first; the group is declared after

void close_window(void* state) noexcept
{
    auto* const w = static_cast<window*>(state);

    try
    {
        w->group.request_stop();                       // 1. stop every task
        alt::sync_wait(w->group.join(), *w->loop);     // 2. wait, pumping the loop
    }
    catch(const std::exception&)
    {
        // log it; a throw here must not leave the work item
    }

    // 3. destroy the primitives and the group, which happens when w goes
}
```

## Joining is required before destruction

The destructor asserts, in debug builds, that nothing is outstanding. It does
nothing else: it neither stops nor waits. The group does not own the frames,
which own themselves, so there is nothing for it to destroy, and blocking would
deadlock whenever a task needs the destroying thread to make progress. Destroying
a group with live tasks leaves their completion hooks pointing at freed memory.

A cancelled joiner is the one way to reach that assert without having made a
mistake. If the coroutine that awaits `join()` is itself cancelled, the join
reports `operation_canceled` and the group is left unjoined with tasks still
live. The owner's answer is to join without being cancellable, or to stop and join
from a scope the cancellation does not reach.

## A spawn racing a join may land after it

`join()` completes when the count reaches zero. A `spawn` racing that moment may be
counted just after it, and the joiner has then been released with a task still
alive. An owner that is tearing down stops spawning first, and then the question
has one answer.

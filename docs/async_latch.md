# async_latch

**Header:** `<alt/async_latch.hpp>`

A single-use countdown that coroutines await without blocking a thread.
Constructed with a count; `count_down()` lowers it, and the call that brings it
to zero releases every parked waiter at once. Awaiting a latch that is already
at zero does not suspend.

The latch does not reset. Once at zero it stays there, which is what makes it
safe to await from a coroutine that arrives late. A signal that can be raised
and lowered repeatedly is [`alt::async_event`](async_event.md).

```cpp
#include <alt/async_latch.hpp>
```

## Interface

```cpp
class async_latch
{
public:
    explicit async_latch(std::ptrdiff_t count);

    ~async_latch();

    async_latch(const async_latch&)            = delete;
    async_latch& operator=(const async_latch&) = delete;
    async_latch(async_latch&&)                 = delete;
    async_latch& operator=(async_latch&&)      = delete;

    void               count_down(std::ptrdiff_t n = 1);
    [[nodiscard]] bool try_wait() const noexcept;

    class latch_awaiter;   // what operator co_await returns

    [[nodiscard]] auto operator co_await() noexcept;   // -> latch_awaiter
};
```

- The constructor takes the number of counts to wait for, counted by
  `count_down`'s arguments. Zero is a latch that is already released. **The count
  must not be negative**; a debug build asserts it, and a release build does not
  check.
- `co_await latch` yields `std::expected<void, std::error_code>`: a value when
  the count reached zero, and `std::errc::operation_canceled` when the wait
  ended without that. It costs no suspension at all if the count is already
  zero.
- `count_down(n)` lowers the count by `n` and, if that reaches zero, releases
  every parked waiter, satisfied, in the order they parked. Counting down past
  zero is harmless rather than an error: the count stops at zero, and only the
  call that actually brought it there releases anyone, so a latch is never
  released twice. `n` must not be negative — asserted in a debug build only —
  and zero changes nothing. **When this is the call that reaches zero, waiter
  bodies run on the calling thread before it returns.**
- `try_wait()` is `true` once the count has reached zero, so an await would not
  suspend. A snapshot like any other observation of shared state: a concurrent
  `count_down` may have reached zero just after the caller read `false`.
- `latch_awaiter` is the type `operator co_await` returns. It is public because
  an awaiter has to be a named type, not because a caller writes it down.

Every member but the destructor is safe to call from any thread, and
concurrently: of several threads counting down at once, exactly one is the call
that reaches zero, and it alone releases the waiters.

`async_latch` is neither copyable nor movable. The awaiters inside suspended
coroutine frames hold pointers to it, and no copy or move could fix those up.

## Awaiting a latch

The shape it is for: an owner that needs to know a fixed number of jobs are
finished.

```cpp
#include <alt/async_latch.hpp>
#include <alt/task.hpp>

alt::task<void> after_all_three(alt::async_latch& done)
{
    if(!co_await done)
    {
        co_return;         // released without the count reaching zero
    }

    collect_results();
}

// Each job, on whatever thread it finishes on:
void job_finished(alt::async_latch& done)
{
    done.count_down();
}
```

With `alt::async_latch done{3}`, the third `count_down()` is the one that
releases `after_all_three` — and runs its body on the thread that made that
third call.

`count_down(n)` lowers the count by more than one at a time, for a producer that
retires several jobs at once:

```cpp
alt::async_latch done{10};

done.count_down(4);
done.count_down(6);        // this call reaches zero and releases the waiters
```

## Single use

There is no `reset()` and no way to raise the count. A latch is one countdown,
matching `std::latch`, and a second round of waiting needs a second latch.

The consequence worth planning for is that a latch cannot be the join point of
anything that may spawn more work after the count is fixed: the count is decided
at construction. A latch answers "are these *n* things done?" and nothing else.

## Hazards

### Waiters resume on the thread that reached zero

An `async_latch` captures no scheduler. Every waiter it releases is resumed
inside the `count_down()` call that reached zero, on the thread that made it,
and runs its body there up to its next suspension point. So the last job to
finish is the thread that runs the join code, and `count_down()` must not be
called with a lock held that a waiter's body might take.

A waiter whose continuation is thread-sensitive hops explicitly:

```cpp
alt::task<void> handler(alt::async_latch& done, alt::run_loop& ui)
{
    if(!co_await done)
    {
        co_return;
    }

    co_await alt::resume_on(ui);   // now on the UI thread, not count_down()'s thread
    collect_results();
}
```

This is [`alt::resume_on`](scheduler.md#resume_on)'s existing philosophy — hops
are explicit — applied to a primitive. The destructor releases its waiters too,
so the thread that destroys a latch also runs waiter bodies.

### A released wait did not see the count reach zero

A wait can end without the count ever reaching zero, and then it reports
`std::errc::operation_canceled`:

- the awaiting chain was cancelled while the waiter was parked, or
- the latch was destroyed with the waiter still parked.

Cancellation **releases** the parked waiter; it does not count anything down and
satisfies nothing. The released body resumes, with the error in hand, and runs
to its next suspension point, where [the ordinary
cascade](cancellable_task.md) tears the chain down. Code between the `co_await`
and that suspension point really does run, which is why the result has to be
checked rather than assumed.

Cancellation reaches a waiter only inside a chain rooted at an
[`alt::cancellable_task`](cancellable_task.md), through the stop callback the
awaiter registers on that chain's token. Awaited from a plain `alt::task` run by
[`alt::sync_wait`](sync_wait.md) there is no stop state to observe, so the wait
has exactly two ways to end: the count, or the latch's destruction.

### Ignoring the result is diagnosed on Clang only

Every one of these waits is `[[nodiscard]]`, and
`detail::cancellation_checked` — which wraps every await inside an `alt::task`
or `alt::cancellable_task`, and is documented at
`include/alt/detail/cancellation.hpp` — carries the attribute too. The honest
strength of that is narrower than it looks, and it was measured rather than
assumed:

- **Clang and clang-tidy warn** on a dropped `co_await` on any of these four
  primitives.
- **GCC does not.** On all four the attribute is unenforced under GCC: a dropped
  result is diagnosed by nothing.
- A discarded `co_await` on an `alt::task<T>` *is* diagnosed by GCC, as well as
  by Clang. **What divides those two cases on GCC was not determined, so do not
  read the line above as a statement about `co_await` in general.** It is
  demonstrably not the result type: a discarded `co_await` on a
  `task<std::expected<void, std::error_code>>` warns on GCC, while one on an
  `async_event` — whose result is that same type — does not.

Measured with `-Wall -Wextra` on GCC 15.2.0 and Clang 21.1.8.

So on a GCC build `co_await latch;` written as a statement is diagnosed by
nothing, and the coroutine goes on to read results that may not exist.

Of the four primitives in this family only [`alt::async_mutex`](async_mutex.md)
has a guarantee that does not depend on the compiler: its wait yields a guard
that cannot be constructed without the lock, so there is no "proceeded without
it" state to reach. For `async_event`, `async_latch` and `async_semaphore` the
diagnostic is all there is. Check the result.

### Destruction releases the waiters, and runs their bodies

`~async_latch` releases every waiter still parked, unsatisfied. That is a
backstop rather than the expected path — a waiter nobody resumes stays suspended
forever and its frame is never freed, so releasing them beats leaking them — but
it means **coroutine bodies run during the destructor**, on the destroying
thread. Anything those bodies touch must outlive the latch, which means it must
be declared before it:

```cpp
// collector is an alt::cancellable_task that awaits the latch and writes to sink.

{
    alt::async_latch done{3};
    result_sink      sink;    // WRONG: destroyed before the latch releases its waiters
    const auto       running = alt::start(collector(done, sink));
}

{
    result_sink      sink;    // right: outlives the latch
    alt::async_latch done{3};
    const auto       running = alt::start(collector(done, sink));
}
```

This is the same declaration-ordering rule the schedulers carry — see
[run_loop](run_loop.md#declare-what-work-items-point-at-before-the-loop) — and
it is equally invisible to the sanitizers. AddressSanitizer poisons a stack slot
only after every destructor in the scope has run, so when both objects share a
scope the first form passes every sanitizer run and is still a use-after-free.
The only defense is the declaration order.

### Destroying a latch while another thread is satisfying it is undefined

Destruction is the one operation that is not thread-safe. Destroying an
`async_latch` while another thread is inside the `count_down()` that reaches
zero is undefined behavior, and so is destroying one while a cancellation is in
flight against one of its waiters. This is the same class of precondition
`std::mutex` carries: the owner has to know that nobody is still using the
object. Destroying the latch from a waiter's body on *this* thread is the
supported case described below, and is not this hazard.

The supported shutdown is to stop the chains first, so their waiters are
released by their own stop callbacks, and destroy the latch once nothing can
reach it.

### A parked waiter's frame is not yours to destroy

The node a parked waiter occupies lives inside that waiter's coroutine frame,
and nothing withdraws it from the latch's queue. So a coroutine parked here must
not have its frame destroyed from outside. Destroying the latch instead releases
every waiter, which is the supported teardown.

Nor may a waiter's body destroy *another* waiter of the same latch: the
releasing `count_down()` takes the whole queue before it resumes anyone, so a
frame destroyed from inside one resumption may be one that call is already
committed to resuming.

Destroying the *latch* from a waiter's body is a different thing and is allowed:
the releasing `count_down()` publishes the zero and takes the queue first, and
the resumption is the last thing it does, so there is nothing left for it to
read afterwards. One qualification comes with that, and no waiter can check it
for itself. That one call is committed to resuming every waiter it took, so the
siblings that follow the one which destroyed the latch are still resumed —
correctly, each having been won before anything ran — into a scope where the
latch no longer exists. Those siblings must not touch the latch after their wait
returns. Since a waiter cannot tell whether it is the last of a chain, in
practice a waiter that destroys the latch must be one whose siblings do not go
back to it, which is what a shutdown looks like: everybody is leaving.
[`alt::async_semaphore`](async_semaphore.md#a-waiter-that-destroys-the-semaphore-cannot-know-it-is-the-last)
documents the same qualification for the same reason.

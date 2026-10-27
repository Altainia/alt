# async_event

**Header:** `<alt/async_event.hpp>`

A manual-reset signal that coroutines await without blocking a thread. Awaiting
an unset event suspends the coroutine; `set()` releases every parked waiter at
once and leaves the event set, so a coroutine that arrives afterwards does not
suspend at all. `reset()` clears it again.

That stickiness is the point: a result that is already available must not be
missed by whoever asks for it second.

```cpp
#include <alt/async_event.hpp>
```

## Interface

```cpp
class async_event
{
public:
    explicit async_event(bool set = false) noexcept;

    ~async_event();

    async_event(const async_event&)            = delete;
    async_event& operator=(const async_event&) = delete;
    async_event(async_event&&)                 = delete;
    async_event& operator=(async_event&&)      = delete;

    void               set();
    void               reset() noexcept;
    [[nodiscard]] bool is_set() const noexcept;

    class event_awaiter;   // what operator co_await returns

    [[nodiscard]] auto operator co_await() noexcept;   // -> event_awaiter
};
```

- `co_await event` yields `std::expected<void, std::error_code>`: a value when
  the event released the wait, and `std::errc::operation_canceled` when the wait
  ended without that. It costs no suspension at all if the event is already set.
- `set()` sets the event and releases every parked waiter, satisfied, in the
  order they parked. Idempotent: setting an event that is already set releases
  whatever is parked, which is nothing, because nothing can park on a set event.
  **Waiter bodies run on the calling thread, before `set()` returns.**
- `reset()` clears the flag and does nothing else. It releases nobody and parks
  nobody; it only decides whether the *next* await suspends.
- `is_set()` is `true` while the event is set. It is a snapshot like any other
  observation of shared state: a concurrent `set()` or `reset()` may have moved
  it by the time the caller acts on the answer.
- `event_awaiter` is the type `operator co_await` returns. It is public because
  an awaiter has to be a named type, not because a caller writes it down.

Every member but the destructor is safe to call from any thread, and
concurrently: two threads setting the event resume each parked waiter once
between them, not once each.

`async_event` is neither copyable nor movable. The awaiters inside suspended
coroutine frames hold pointers to it, and no copy or move could fix those up.

## Awaiting an event

```cpp
#include <alt/async_event.hpp>
#include <alt/task.hpp>

alt::task<void> show_when_loaded(alt::async_event& loaded, model& m)
{
    if(!co_await loaded)
    {
        co_return;          // released without the event firing; see the hazards
    }

    render(m);
}
```

Whichever thread finished the load calls `loaded.set()`, which releases every
coroutine parked on the event — and runs their bodies on that thread.

## Manual reset, and what it is for

`set()` leaves the event set. A waiter arriving a minute later still completes
without suspending, which is what makes an event safe to await from a coroutine
that started late:

```cpp
alt::task<void> late(alt::async_event& e)
{
    if(co_await e)        // does not suspend when e is already set
    {
        proceed();
    }
}

alt::async_event ready;
ready.set();

alt::sync_wait(late(ready));   // completes without ever suspending
```

`reset()` is the only way back to the unset state. An event that is reset while
a coroutine is between `is_set()` returning `true` and its own `co_await` simply
parks, which is the ordinary behavior of an unset event rather than a race:
nothing is lost, because nothing had been handed out.

A single-use count, rather than a sticky flag, is
[`alt::async_latch`](async_latch.md).

## Hazards

### Waiters resume on the thread that satisfied them

An `async_event` captures no scheduler. Every waiter it releases is resumed
inside `set()`, on the thread that called it, and runs its body there up to its
next suspension point. So `set()` from a worker thread runs UI code on that
worker unless the waiter says otherwise, and `set()` must not be called with a
lock held that a waiter's body might take.

A waiter whose continuation is thread-sensitive hops explicitly:

```cpp
alt::task<void> handler(alt::async_event& loaded, alt::run_loop& ui)
{
    if(!co_await loaded)
    {
        co_return;
    }

    co_await alt::resume_on(ui);   // now on the UI thread, not set()'s thread
    render();
}
```

This is [`alt::resume_on`](scheduler.md#resume_on)'s existing philosophy — hops
are explicit — applied to a primitive. The destructor releases its waiters too,
so the thread that destroys an event also runs waiter bodies.

### A released wait did not see the event fire

A wait can end without the event ever being set, and then it reports
`std::errc::operation_canceled`:

- the awaiting chain was cancelled while the waiter was parked, or
- the event was destroyed with the waiter still parked.

Cancellation **releases** the parked waiter; it does not set the event and
satisfies nothing. The released body resumes, with the error in hand, and runs
to its next suspension point, where [the ordinary
cascade](cancellable_task.md) tears the chain down. Code between the `co_await`
and that suspension point really does run, which is why the result has to be
checked rather than assumed.

Cancellation reaches a waiter only inside a chain rooted at an
[`alt::cancellable_task`](cancellable_task.md), through the stop callback the
awaiter registers on that chain's token. Awaited from a plain `alt::task` run by
[`alt::sync_wait`](sync_wait.md) there is no stop state to observe, so the wait
has exactly two ways to end: the event, or the event's destruction.

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

So on a GCC build `co_await event;` written as a statement is diagnosed by
nothing, and the coroutine carries on as though the event had fired.

Of the four primitives in this family only [`alt::async_mutex`](async_mutex.md)
has a guarantee that does not depend on the compiler: its wait yields a guard
that cannot be constructed without the lock, so there is no "proceeded without
it" state to reach. For `async_event`, `async_latch` and `async_semaphore` the
diagnostic is all there is. Check the result.

### Destruction releases the waiters, and runs their bodies

`~async_event` releases every waiter still parked, unsatisfied. That is a
backstop rather than the expected path — a waiter nobody resumes stays suspended
forever and its frame is never freed, so releasing them beats leaking them — but
it means **coroutine bodies run during the destructor**, on the destroying
thread. Anything those bodies touch must outlive the event, which means it must
be declared before it:

```cpp
// waiter is an alt::cancellable_task that awaits the event and writes to sink.

{
    alt::async_event event;
    result_sink      sink;    // WRONG: destroyed before the event releases its waiters
    const auto       running = alt::start(waiter(event, sink));
}

{
    result_sink      sink;    // right: outlives the event
    alt::async_event event;
    const auto       running = alt::start(waiter(event, sink));
}
```

This is the same declaration-ordering rule the schedulers carry — see
[run_loop](run_loop.md#declare-what-work-items-point-at-before-the-loop) — and
it is equally invisible to the sanitizers. AddressSanitizer poisons a stack slot
only after every destructor in the scope has run, so when both objects share a
scope the first form passes every sanitizer run and is still a use-after-free.
The only defense is the declaration order.

### Destroying an event while another thread is satisfying it is undefined

Destruction is the one operation that is not thread-safe. Destroying an
`async_event` while another thread is inside `set()` is undefined behavior, and
so is destroying one while a cancellation is in flight against one of its
waiters. This is the same class of precondition `std::mutex` carries: the owner
has to know that nobody is still using the object. Destroying the event from a
waiter's body on *this* thread is the supported case described below, and is not
this hazard.

The supported shutdown is to stop the chains first, so their waiters are
released by their own stop callbacks, and destroy the event once nothing can
reach it.

### A parked waiter's frame is not yours to destroy

The node a parked waiter occupies lives inside that waiter's coroutine frame,
and nothing withdraws it from the event's queue. So a coroutine parked here must
not have its frame destroyed from outside. Destroying the event instead releases
every waiter, which is the supported teardown.

Nor may a waiter's body destroy *another* waiter of the same event: `set()`
takes the whole queue before it resumes anyone, so a frame destroyed from inside
one resumption may be one that same `set()` is already committed to resuming.

Destroying the *event* from a waiter's body is a different thing and is allowed:
`set()` takes the queue, marks the event set and only then resumes, and the
resumption is the last thing it does, so there is nothing left for it to read
afterwards. One qualification comes with that, and no waiter can check it for
itself. A single `set()` may have won several waiters and is committed to
resuming all of them, so the siblings that follow the one which destroyed the
event are still resumed — correctly, each having been won before anything ran —
into a scope where the event no longer exists. Those siblings must not touch the
event after their wait returns. Since a waiter cannot tell whether it is the
last of a chain, in practice a waiter that destroys the event must be one whose
siblings do not go back to it, which is what a shutdown looks like: everybody is
leaving. [`alt::async_semaphore`](async_semaphore.md#a-waiter-that-destroys-the-semaphore-cannot-know-it-is-the-last)
documents the same qualification for the same reason.

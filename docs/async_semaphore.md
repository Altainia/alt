# async_semaphore

**Header:** `<alt/async_semaphore.hpp>`

A counting semaphore that coroutines await without blocking a thread. It holds a
count of permits; awaiting it takes one, suspending the coroutine if there is
none to take. `release()` puts permits back, handing them to parked waiters
before the count. `try_acquire()` is the same acquisition without the
suspension, for a caller that would rather do something else.

```cpp
#include <alt/async_semaphore.hpp>
```

## Interface

```cpp
class async_semaphore
{
public:
    explicit async_semaphore(std::ptrdiff_t initial);

    ~async_semaphore();

    async_semaphore(const async_semaphore&)            = delete;
    async_semaphore& operator=(const async_semaphore&) = delete;
    async_semaphore(async_semaphore&&)                 = delete;
    async_semaphore& operator=(async_semaphore&&)      = delete;

    void                         release(std::ptrdiff_t n = 1);
    [[nodiscard]] bool           try_acquire() noexcept;
    [[nodiscard]] std::ptrdiff_t available() const noexcept;

    class semaphore_awaiter;   // what operator co_await returns

    [[nodiscard]] auto operator co_await() noexcept;   // -> semaphore_awaiter
};
```

- The constructor takes the starting permit count, so the first `initial`
  acquisitions do not suspend. **The count must not be negative**; a debug build
  asserts it, and a release build does not check.
- `co_await semaphore` yields `std::expected<void, std::error_code>`: a value
  when the wait came away with a permit, and `std::errc::operation_canceled`
  when the wait ended without one. A wait that ends that way has consumed
  nothing, so the count is exactly what it would have been had the wait never
  happened.
- `release(n)` hands out `n` permits: each goes to the oldest waiter that can
  still be won, and once there is no such waiter the rest go into the count,
  where the next acquisition finds them. `n` must not be negative — asserted in
  a debug build only — and zero changes nothing. **Waiter bodies run on the
  calling thread before `release()` returns.**
- `try_acquire()` takes one permit if there is one and returns `true`; the
  caller then owns that permit and must release it. `false` takes nothing and
  leaves the count where it was, which never goes negative.
- `available()` is how many permits are in the count. A permit handed straight
  to a parked waiter is not counted here and never was: `release()` gives it away
  without it passing through the count. Never negative, and a snapshot like any
  other observation of shared state — a concurrent acquisition may have spent the
  permit by the time the caller reads it.
- `semaphore_awaiter` is the type `operator co_await` returns. It is public
  because an awaiter has to be a named type, not because a caller writes it down.

Every member but the destructor is safe to call from any thread, and
concurrently: of several threads releasing at once, each permit reaches exactly
one waiter or the count, and of several callers reaching for one permit exactly
one takes it.

`async_semaphore` is neither copyable nor movable. The awaiters inside suspended
coroutine frames hold pointers to it, and no copy or move could fix those up.

## Bounding concurrency

The shape it is for: at most four fetches in flight, however many coroutines
want one.

```cpp
#include <alt/async_semaphore.hpp>
#include <alt/scope.hpp>
#include <alt/task.hpp>

alt::task<void> fetch_one(alt::async_semaphore& slots, const request& r)
{
    if(!co_await slots)
    {
        co_return;      // released without a permit; there is nothing to give back
    }

    // Paired by hand, and a scope guard is the way to pair it on every exit.
    const alt::scope_exit permit{[&slots] { slots.release(); }};

    co_await perform(r);
}
```

With `alt::async_semaphore slots{4}`, the fifth concurrent `fetch_one` parks
until one of the four ahead of it releases.

The guard is worth the line. `release()` has to happen on the path where
`perform` throws as well as the path where it returns, and on the path where the
chain is torn down — a cancelled chain destroys the frame, which destroys the
guard, which releases the permit. Note what that implies about where the next
waiter runs: a `release()` from a scope guard hands the permit to the next
waiter and runs that waiter's body right there, inside the destruction of this
frame.

## There is no RAII permit

Acquiring is deliberately not an RAII operation. Nothing here hands back a
guard, and `release()` is an independent call that any thread may make, because
a semaphore's acquire and release are frequently asymmetric: one party admits
work, another retires it. An acquisition that handed back an owning guard would
fit the symmetric case and get in the way of that one.

So the pairing is the caller's to get right, and `available()` will not tell
them it went wrong — a permit nobody releases is simply a permit the semaphore
no longer has.

It is worth being precise about what an RAII permit would and would not have
bought, because the obvious complaint is that
[`alt::async_mutex`](async_mutex.md) has a guard and this does not. A guard
fixes a forgotten **release**. The failure this family actually guards against
is proceeding **without having acquired** — a waiter that was released rather
than satisfied, carrying on into the bounded region as though it holds a
permit. A guard handed back by a successful acquisition does nothing about that,
because the caller who ignores the result never reaches it. `async_mutex`'s
guard helps because of the other half of its design: a mutex wait yields the
guard *or* an error and nothing else, so there is no way to name the state where
a caller believes it holds the lock and does not. A semaphore wait yields
nothing on success, so there is nothing for a guard to be attached to.

Which leaves the diagnostic, and
[the diagnostic is weaker than it looks](#ignoring-the-result-is-diagnosed-on-clang-only).

## Fairness, as far as it goes

Permits are handed to parked waiters in the order they parked, and a release
never adds to the count while a parked waiter could take the permit instead. So
a waiter that has parked cannot be overtaken by a later arrival.

What is not ordered is the race to park. A coroutine that reads an empty count
and a `try_acquire()` a moment later are two unsynchronized attempts at the same
count, and the second may win. Fairness begins once a waiter is in the queue,
not before.

## Hazards

### Waiters resume on the thread that released the permit

An `async_semaphore` captures no scheduler. Every waiter it hands a permit to is
resumed inside `release()`, on the thread that called it, and runs its body
there up to its next suspension point. So the thread that retires one piece of
work is the thread that starts the next, and `release()` must not be called with
a lock held that a waiter's body might take.

A waiter whose continuation is thread-sensitive hops explicitly:

```cpp
alt::task<void> handler(alt::async_semaphore& slots, alt::run_loop& ui)
{
    if(!co_await slots)
    {
        co_return;
    }

    const alt::scope_exit permit{[&slots] { slots.release(); }};

    co_await alt::resume_on(ui);   // now on the UI thread, not release()'s thread
    touch_ui_state();
}
```

This is [`alt::resume_on`](scheduler.md#resume_on)'s existing philosophy — hops
are explicit — applied to a primitive. The destructor releases its waiters too,
so the thread that destroys a semaphore also runs waiter bodies.

### A released wait holds no permit

A wait can end without a permit, and then it reports
`std::errc::operation_canceled`:

- the awaiting chain was cancelled while the waiter was parked, or
- the semaphore was destroyed with the waiter still parked.

Cancellation **releases** the parked waiter; it does not acquire anything. The
waiter holds no permit, and it must not release one — the count would gain a
permit the semaphore never issued. The released body resumes, with the error in
hand, and runs to its next suspension point, where [the ordinary
cascade](cancellable_task.md) tears the chain down. Code between the `co_await`
and that suspension point really does run, which is why the result has to be
checked rather than assumed.

Cancellation reaches a waiter only inside a chain rooted at an
[`alt::cancellable_task`](cancellable_task.md), through the stop callback the
awaiter registers on that chain's token. Awaited from a plain `alt::task` run by
[`alt::sync_wait`](sync_wait.md) there is no stop state to observe, so the wait
has exactly two ways to end: a permit, or the semaphore's destruction.

The converse is guaranteed too: a wait is never told it was cancelled after it
has taken a permit. Acquiring is the awaiter's own readiness check, so a permit
may be taken on either of the two occasions that check runs — and whichever one
found it, the wait reports satisfied. No permit is consumed and then disclaimed.
That is enforced in `detail::wait_awaiter::await_suspend`'s rule 4 rather than
inherited silently.

### Ignoring the result is diagnosed on Clang only

Every one of these waits is `[[nodiscard]]`, and
`detail::cancellation_checked` — which wraps every await inside an `alt::task`
or `alt::cancellable_task`, and is documented at
`include/alt/detail/cancellation.hpp` — carries the attribute too. The honest
strength of that is narrower than it looks, and it was measured rather than
assumed:

- **Clang and clang-tidy warn** on a dropped `co_await` result.
- **GCC does not warn here.** Measured on GCC 15.2: `-Wunused-result` does fire
  for a discarded `co_await` whose result is a scalar, and does not fire for one
  whose result is a class type. Every wait in this family yields a
  `std::expected`, which puts all four of them on the silent side.

So on a GCC build `co_await semaphore;` written as a statement is diagnosed by
nothing, and the coroutine enters a bounded region it holds no permit for — and
then releases a permit it never had on the way out.

Of the four primitives in this family only [`alt::async_mutex`](async_mutex.md)
has a guarantee that does not depend on the compiler: its wait yields a guard
that cannot be constructed without the lock, so there is no "proceeded without
it" state to reach. For `async_event`, `async_latch` and `async_semaphore` the
diagnostic is all there is. Check the result.

### A re-await is not handed the next permit

`release(n)` places every one of its `n` permits before it resumes anybody. Each
goes to the oldest winnable waiter, the leftovers go into the count, and only
then are the won waiters resumed. There is one visible consequence, which
`release()`'s own documentation states: a waiter resumed by that call which
immediately awaits the semaphore again finds the leftover permits **in the
count** rather than being handed one directly, so an unrelated `try_acquire()`
on another thread can take one first.

That is the price of the next hazard — being able to destroy the semaphore from
a waiter the call resumed, which is the more dangerous thing for a caller to get
wrong. It is a real cost rather than a bookkeeping detail: `available()` reads
the leftover and `try_acquire()` can act on it.

### A waiter that destroys the semaphore cannot know it is the last

Destroying the semaphore from a resumed waiter's body is allowed, including from
a waiter `release()` resumed: every permit is placed before anything is resumed,
and the resumption is the last thing `release()` does, so there is nothing left
for it to read afterwards.

One qualification comes with that, and a waiter cannot check it for itself. A
single `release()` may have won several waiters and is committed to resuming all
of them, so the siblings that follow the one which destroyed the semaphore are
still handed their permits — correctly, each having been won before anything was
resumed — into a scope where the semaphore no longer exists. **Those siblings
must not touch the semaphore after their wait returns**, which in particular
means they must not release the permit they were just given.

Since no waiter can tell whether it is the last of a chain, in practice a waiter
that destroys the semaphore must be one whose siblings do not go back to it,
which is what a shutdown drain looks like: every waiter is leaving.

### `release(n)` has no overflow guard

The total number of permits a semaphore holds must stay within
`std::ptrdiff_t`. Nothing checks it — the only assertion `release()` carries,
even in a debug build, is that `n` is not negative. Overflowing the count is a
precondition violation, not a reported error.

### Destruction releases the waiters, and runs their bodies

`~async_semaphore` releases every waiter still parked, without a permit. That is
a backstop rather than the expected path — a waiter nobody resumes stays
suspended forever and its frame is never freed, so releasing them beats leaking
them — but it means **coroutine bodies run during the destructor**, on the
destroying thread. Anything those bodies touch must outlive the semaphore, which
means it must be declared before it:

```cpp
// worker is an alt::cancellable_task that awaits a permit and writes to log.

{
    alt::async_semaphore slots{4};
    request_log          log;    // WRONG: destroyed before the semaphore releases its waiters
    const auto           running = alt::start(worker(slots, log));
}

{
    request_log          log;    // right: outlives the semaphore
    alt::async_semaphore slots{4};
    const auto           running = alt::start(worker(slots, log));
}
```

This is the same declaration-ordering rule the schedulers carry — see
[run_loop](run_loop.md#declare-what-work-items-point-at-before-the-loop) — and
it is equally invisible to the sanitizers. AddressSanitizer poisons a stack slot
only after every destructor in the scope has run, so when both objects share a
scope the first form passes every sanitizer run and is still a use-after-free.
The only defense is the declaration order.

### Destroying a semaphore while another thread is satisfying it is undefined

Destruction is the one operation that is not thread-safe. Destroying an
`async_semaphore` while another thread is inside `release()` is undefined
behavior, and so is destroying one while a cancellation is in flight against one
of its waiters. This is the same class of precondition `std::mutex` carries: the
owner has to know that nobody is still using the object. Destroying it from
inside a `release()` on *this* thread is the supported case above, and is not
this hazard.

The supported shutdown is to stop the chains first, so their waiters are
released by their own stop callbacks, and destroy the semaphore once nothing can
reach it.

### A parked waiter's frame is not yours to destroy

The node a parked waiter occupies lives inside that waiter's coroutine frame,
and nothing withdraws it from the semaphore's queue. So a coroutine parked here
must not have its frame destroyed from outside. Destroying the semaphore instead
releases every waiter, which is the supported teardown.

Nor may a waiter's body destroy *another* waiter of the same semaphore, for the
same reason: that waiter's node is in the frame being freed, and the chain the
resumption is walking still points at it.

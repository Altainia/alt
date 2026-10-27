# async_mutex

**Header:** `<alt/async_mutex.hpp>`

Mutual exclusion that blocks no thread. Awaiting the mutex acquires it,
suspending the coroutine if another party holds it, and yields the guard that
owns the lock. The guard releases it on destruction, so a critical section is a
scope.

```cpp
#include <alt/async_mutex.hpp>
```

Of the four primitives in this family it is the only one with a type-level
guarantee rather than a documented one: `async_mutex_lock` has no constructor
that does not already hold the lock, so the fails-open state — a caller that
believes it holds the lock and does not — cannot be named, let alone reached.

## Interface

```cpp
class async_mutex_lock
{
public:
    async_mutex_lock(async_mutex_lock&& other) noexcept;
    async_mutex_lock& operator=(async_mutex_lock&& other) noexcept;

    ~async_mutex_lock();

    async_mutex_lock(const async_mutex_lock&)            = delete;
    async_mutex_lock& operator=(const async_mutex_lock&) = delete;

    void               unlock() noexcept;
    [[nodiscard]] bool owns_lock() const noexcept;

    // No public constructor: only alt::async_mutex can make one, on the two
    // paths that have just acquired the lock.
};

class async_mutex
{
public:
    async_mutex() noexcept;

    ~async_mutex();

    async_mutex(const async_mutex&)            = delete;
    async_mutex& operator=(const async_mutex&) = delete;
    async_mutex(async_mutex&&)                 = delete;
    async_mutex& operator=(async_mutex&&)      = delete;

    [[nodiscard]] std::expected<async_mutex_lock, std::error_code> try_lock();

    class mutex_awaiter;   // what operator co_await returns

    [[nodiscard]] auto operator co_await() noexcept;   // -> mutex_awaiter
};
```

- A default-constructed `async_mutex` is unlocked with no waiters. There is no
  other constructor.
- `co_await mutex` yields `std::expected<async_mutex_lock, std::error_code>`:
  the guard that owns the lock, or `std::errc::operation_canceled` when the wait
  ended without it. It costs no suspension at all if the lock was free. This is
  the one primitive in the family that does **not** yield
  [`alt::wait_result`](async_event.md#the-result-of-a-wait), because a satisfied
  acquisition has a guard to hand back; the error half is the same error, named
  the same way — compare the `std::error_code` against the `std::errc` rather
  than build a second code to match it.
- `try_lock()` is the same acquisition without the suspension. It yields the
  guard when the lock was taken, and `std::errc::device_or_resource_busy` when
  another party holds it, which takes nothing and leaves the lock exactly where
  it was.
- `mutex_awaiter` is the type `operator co_await` returns. It is public because
  an awaiter has to be a named type, not because a caller writes it down.

The guard:

- `~async_mutex_lock` releases the lock if the guard still owns it.
- `unlock()` releases it now rather than at the end of the scope, and is
  idempotent: a guard that owns nothing does nothing here. That matters more
  than it looks, because releasing a lock hands it to the next waiter, and a
  second release would let a second coroutine into a critical section the first
  has not left.
- Move-only, like `std::unique_lock`. A moved-from guard owns nothing, reports
  `owns_lock()` false, and does not unlock when it dies, so the lock is released
  exactly once however the guard is passed around. Move-assignment releases the
  target's own lock first, and self-move-assignment leaves the lock where it is.
- `owns_lock()` is `true` while the guard holds the lock, and false after a move
  or an `unlock()`.
- Nothing about a guard is thread-safe against concurrent use of the same guard
  object, as with any other value. A guard moved to another thread and released
  there is fine.
- **The guard must not outlive its mutex.** Releasing a lock means handing it to
  the mutex's next waiter, which a guard whose mutex has already gone cannot do.

Every member of the mutex but the destructor is safe to call from any thread,
and concurrently: of several callers reaching for the lock exactly one takes it.

`async_mutex` is neither copyable nor movable. The awaiters inside suspended
coroutine frames and the live guards both hold pointers to it, and no copy or
move could fix those up.

## A critical section is a scope

```cpp
#include <alt/async_mutex.hpp>
#include <alt/task.hpp>

alt::task<void> append(alt::async_mutex& mutex, log_file& file, const char* line)
{
    if(const auto held = co_await mutex)
    {
        file.write(line);        // the critical section
    }                            // held dies here, releasing the lock
}
```

The `if` is not decoration. A wait that did not acquire the lock yields
`std::unexpected`, so the body of the `if` is the critical section and nothing
else can be. There is no way to reach it holding nothing.

`try_lock()` is the non-suspending form, for a caller with something else to do:

```cpp
if(auto held = mutex.try_lock())
{
    file.write(line);
}
else if(held.error() == std::errc::device_or_resource_busy)
{
    queue_for_later(line);
}
```

To end the critical section early, call `unlock()` on the guard rather than
waiting for the scope:

```cpp
alt::task<void> swap_out(alt::async_mutex& mutex, buffer& shared)
{
    if(auto held = co_await mutex)
    {
        const buffer mine = std::move(shared);
        held->unlock();                 // released here, not at the closing brace

        process(mine);                  // outside the critical section
    }
}
```

## Fairness, and the hand-off

A release hands the lock directly to the oldest parked waiter rather than
dropping it and letting everyone re-contend, so a waiter that has parked cannot
be overtaken by a later arrival.

What is not ordered is the race to park: a coroutine that finds the mutex held
and a `try_lock()` a moment later are two unsynchronized attempts at the same
lock, and the second may win. Fairness begins once a waiter is in the queue, not
before.

Handing the lock over directly has one visible cost. A coroutine that releases
the lock and immediately awaits it again does not get it straight back: it goes
to the back of the queue behind the waiters that were already there, and is
handed the lock one trip through the queue later.

However long the queue, releasing the lock uses a constant amount of stack. The
hand-off drains iteratively, so ten thousand waiters behind one lock are ten
thousand resumptions at one stack depth rather than ten thousand nested frames.
This is the same device [`alt::trampoline_scheduler`](scheduler.md#trampoline_scheduler)
uses, and for the same reason.

## Hazards

### Waiters resume on the thread that released the lock

An `async_mutex` captures no scheduler. The next waiter is resumed inside
`async_mutex_lock::unlock()` or the guard's destructor, on the thread that
released the lock, and runs its critical section there up to its next suspension
point. So **the closing brace of one critical section runs the next one**, and a
guard must not be destroyed while holding an unrelated lock that a waiter's body
might take.

A waiter whose critical section is thread-sensitive hops explicitly — but note
where the hop goes:

```cpp
alt::task<void> update(alt::async_mutex& mutex, alt::run_loop& ui)
{
    co_await alt::resume_on(ui);        // hop first, then take the lock

    if(const auto held = co_await mutex)
    {
        touch_ui_state();
    }
}
```

Hopping *after* `co_await mutex` would hold the lock across a suspension point,
which is its own hazard below. Hop first where that is possible.

This is [`alt::resume_on`](scheduler.md#resume_on)'s existing philosophy — hops
are explicit — applied to a primitive. The destructor releases its waiters too,
so the thread that destroys a mutex also runs waiter bodies.

### A released wait holds no lock

A wait can end without the lock, and then it reports
`std::errc::operation_canceled`:

- the awaiting chain was cancelled while the waiter was parked, or
- the mutex was destroyed with the waiter still parked.

Cancellation **releases** the parked waiter; it does not acquire anything. No
guard is built on that path, so the waiter has nothing to unlock and nothing to
hand on. The released body resumes, with the error in hand, and runs to its next
suspension point, where [the ordinary cascade](cancellable_task.md) tears the
chain down.

That body runs **on whichever thread observed the stop** — the thread that
called `request_stop()`, synchronously inside that call — and a `home` scheduler
on the chain does not change that: `start_options::home` governs only where the
frames are destroyed, not where a released waiter's body runs. So a released
continuation that touches thread-affine state must `co_await alt::resume_on(...)`
before touching it, exactly as it must after an ordinary hand-off. For a window
closing on the UI thread this is the hazard that bites: the body resumes inside
the close, on the closing thread, with the window already going away.
[`cancellable_task`](cancellable_task.md) states the same narrowing from the
cancellation side.

Cancellation reaches a waiter only inside a chain rooted at an
[`alt::cancellable_task`](cancellable_task.md), through the stop callback the
awaiter registers on that chain's token. Awaited from a plain `alt::task` run by
[`alt::sync_wait`](sync_wait.md) there is no stop state to observe, so the wait
has exactly two ways to end: the lock, or the mutex's destruction.

A wait is never told it was cancelled after it has taken the lock. That is a
property this class depends on absolutely — a lock taken by a wait that then
reported itself cancelled would be held by nobody, with nothing left to return
it, and every later locker would wait forever — and it is enforced in
`detail::wait_awaiter::await_suspend`'s rule 4 rather than inherited silently.

### Ignoring the result is diagnosed on Clang only, and here that is survivable

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

So on a GCC build `co_await mutex;` written as a statement is diagnosed by
nothing.

`async_mutex` is the one primitive in this family where that is not a
correctness problem, and that is the whole reason the guard exists. There is no
constructor for a guard that does not hold the lock, so a dropped result cannot
leave a caller holding a lock it does not have, and cannot leave a lock held by
a caller that thinks it holds nothing. The guard inside the discarded
`std::expected` is destroyed at the end of that full-expression and unlocks: a
dropped result is a critical section of zero length, which is never what the
caller meant but is safe. `try_lock()` carries `[[nodiscard]]` for exactly that
reason.

The other three — [`async_event`](async_event.md),
[`async_latch`](async_latch.md) and [`async_semaphore`](async_semaphore.md) —
have no such type to lean on, and on a GCC build the diagnostic is all they
have. Say it plainly: on GCC they have nothing.

### Holding the lock across a suspension point holds it for as long as that takes

Nothing stops a critical section from containing a `co_await`, and sometimes
that is the point. But the lock is held for the whole of that suspension,
including a hop to another scheduler and whatever queueing delay that scheduler
has, and every other coroutine reaching for the mutex parks behind it. Keep the
guard's scope to the state it protects, and prefer hopping before taking the
lock rather than after.

Cancellation does not strand the lock there. A cancelled chain is torn down by
destroying its frames, which destroys the guard living in one of them, which
releases the lock. Note what that implies about where the next waiter runs: the
next critical section starts inside that teardown, on whichever thread performed
it — or on the chain's `home` scheduler, if it was given one.

Which puts a sharper edge on the leak that
[`cancellable_task`](cancellable_task.md#frames-are-destroyed-on-the-thread-that-observed-cancellation-unless-a-home-scheduler-is-given)
describes. A `home` that is supplied but never pumped, or destroyed with the
teardown still queued, does not merely leak the chain's frames: the guard is one
of those frames' locals, so the mutex is left **locked forever**, and every
coroutine that reaches for it afterwards parks behind a lock nothing will ever
release.

### `async_mutex` is not recursive, and a second acquisition deadlocks

Awaiting the mutex from inside a critical section that already holds it — a
helper that takes the same lock being the usual way to get there — is a
**permanent deadlock** rather than a re-entry. `async_mutex` is no more
recursive than `std::mutex`, which carries the same precondition, and nothing
here detects the mistake:

```cpp
alt::task<void> append_locked(alt::async_mutex& mutex, log_file& file)
{
    if(const auto held = co_await mutex)   // WRONG when the caller already holds it
    {
        file.write("...");
    }
}

alt::task<void> write_batch(alt::async_mutex& mutex, log_file& file)
{
    if(const auto held = co_await mutex)
    {
        co_await append_locked(mutex, file);   // hangs here, and forever
    }
}
```

The second await finds the lock held and parks, and the only party that could
release it is the coroutine that has just parked: its guard is a live local in
the suspended frame, which nothing will now reach. That is worse than one hung
coroutine. The lock stays marked held for the rest of the program, so **every
later locker hangs too** — and if the parked coroutine had itself been resumed
by a release, that release's hand-off drain stops there as well, having seen the
lock never come back. There is no diagnostic and no timeout; the symptom is a
silent hang.

So a helper that runs inside a critical section takes the guard, or a reference
to the state the lock protects, as a parameter instead of taking the lock
itself. Where the helper really must acquire, the caller `unlock()`s before the
nested call and re-acquires afterwards — accepting that it goes to the back of
the queue, as any other re-acquisition does.

### A waiter resumed by a release may not destroy the mutex

This one is forced, not chosen, and it is where `async_mutex` parts company with
[`async_semaphore`](async_semaphore.md#a-waiter-that-destroys-the-semaphore-cannot-know-it-is-the-last),
which does permit a waiter resumed by `release()` to destroy the semaphore.

The reason is the hand-off. `release(n)` knows `n` up front and can win every
node before it resumes anybody, so it never reads the semaphore again
afterwards. `unlock()` has one lock to give and **cannot know there is a second
hand-off to make until the first waiter gives the lock back** — which happens
inside that waiter's resumption. So the drain loop reads `*this` again after
resuming, and a waiter that destroyed the mutex would leave it reading freed
storage.

The restriction costs nothing real: a waiter resumed here is inside the critical
section and holds a guard it has to release first, and releasing it is itself a
call into this mutex.

### A waiter released by the destructor may not await the mutex again

This one is chosen, and the distinction matters because nothing forces it. The
rule is the same on all four primitives in this family — none of them promises
anything about a re-await from inside a waiter its destructor released — and
each of the four destructors releases its waiters itself, before its
`waiter_list` member's own destructor runs. That explicit release is a defensive
measure, and it is why the ordinary case does not leak a re-awaiting waiter: the
re-await lands in a list that is still whole and the member's destructor
releases that round too. It is not a guarantee anyone may lean on, and no depth
of re-await is promised anywhere.

For this mutex there is a second reason on top of the family's, and it is why
the family settled where it did. The destructor is one detach and one walk of
the released waiters; it never calls `unlock()`, so no waiter it releases builds
a guard and none of them re-enters the hand-off loop. What a re-await would
require promising is something this type has nothing to promise it with. A mutex
destroyed *unlocked* with waiters still parked is not reachable through the
public interface today and is not ruled out by anything in the type either;
there, a re-await would be handed the lock inside the destructor body, and
nothing stops that guard being moved out to outlive the body — a guard with no
mutex left to hand the lock back to. So the rule stands: a waiter released by
`~async_mutex` must not come back.

### Destruction releases the waiters, and runs their bodies

`~async_mutex` releases every waiter still parked, without the lock. That is a
backstop rather than the expected path — a waiter nobody resumes stays suspended
forever and its frame is never freed, so releasing them beats leaking them — but
it means **coroutine bodies run during the destructor**, on the destroying
thread. Anything those bodies touch must outlive the mutex, which means it must
be declared before it:

```cpp
// writer is an alt::cancellable_task that awaits the mutex and writes to file.

{
    alt::async_mutex mutex;
    log_file         file;    // WRONG: destroyed before the mutex releases its waiters
    const auto       running = alt::start(writer(mutex, file));
}

{
    log_file         file;    // right: outlives the mutex
    alt::async_mutex mutex;
    const auto       running = alt::start(writer(mutex, file));
}
```

This is the same declaration-ordering rule the schedulers carry — see
[run_loop](run_loop.md#declare-what-work-items-point-at-before-the-loop) — and
it is equally invisible to the sanitizers. AddressSanitizer poisons a stack slot
only after every destructor in the scope has run, so when both objects share a
scope the first form passes every sanitizer run and is still a use-after-free.
The only defense is the declaration order.

The guard is subject to the same rule from the other side: a live guard must be
destroyed before the mutex it holds, because its destructor hands the lock to
the mutex's next waiter.

### Destroying a mutex while another thread is satisfying it is undefined

Destruction is the one operation that is not thread-safe. Destroying an
`async_mutex` while another thread is releasing the lock is undefined behavior,
and so is destroying one while a cancellation is in flight against one of its
waiters, or while a guard that could outlive it still holds the lock. This is
the same class of precondition `std::mutex` carries: the owner has to know that
nobody is still using the object.

The supported shutdown is to stop the chains first, so their waiters are
released by their own stop callbacks, and destroy the mutex once nothing can
reach it and every guard is gone.

### A parked waiter's frame is not yours to destroy

The node a parked waiter occupies lives inside that waiter's coroutine frame,
and nothing withdraws it from the mutex's queue. So a coroutine parked here must
not have its frame destroyed from outside, and a waiter's body must not destroy
another waiter of the same mutex. Destroying the mutex instead releases every
waiter, which is the supported teardown — subject to the two rules above.

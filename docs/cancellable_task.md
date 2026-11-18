# cancellable_task

**Header:** `<alt/cancellable_task.hpp>`

`alt::cancellable_task` is a self-owning root coroutine that can be told to
stop and destroys its own await chain — every child `alt::task` it is
currently awaiting, down to the leaf — the next time any frame in that chain
reaches a suspension point. This component exists for one scenario:

1. A button press starts a coroutine owned by a window.
2. The coroutine reads data from UI elements, on the UI thread.
3. It awaits a thread pool and performs a long calculation on a worker thread.
4. Meanwhile the window closes and requests cancellation.
5. The calculation finishes and the coroutine resumes on the worker thread.
6. At its next `co_await`, the coroutine machinery destroys the whole chain
   instead of resuming. From that point on nothing in the body runs, so
   nothing touches the dead window.

Point 6 is the guarantee: it is provided by the coroutine machinery itself
and does not depend on whatever is being awaited cooperating with
cancellation.

**One case narrows point 6, and it is the case this library's own async
primitives put a coroutine in.** A chain parked on an
[`alt::async_event`](async_event.md), [`alt::async_latch`](async_latch.md),
[`alt::async_semaphore`](async_semaphore.md),
[`alt::async_mutex`](async_mutex.md),
[`alt::task_completion_source`](task_completion_source.md) or
[`alt::task_group::join()`](task_group.md) is not left parked until something
satisfies it. The awaiter registers a stop callback, and `request_stop()` runs
that callback, which **releases the parked waiter and resumes its body**, with a
result reporting that the wait was released rather than satisfied. For such a
chain point 6 therefore reads "nothing after the *next* suspension point runs":
the code between that `co_await` and the following suspension point really does
execute, which is why those waits hand back a result that has to be checked.

And it executes **on the thread that called `request_stop()`, even when
`start_options::home` was supplied.** `home` governs only where the frames are
*destroyed*; it has no say over where a released waiter's body runs. In the
scenario above that means body code running on the thread that closed the
window, synchronously inside `request_stop()`, while the window is on its way
out — which is the very thing point 6 otherwise rules out. A coroutine that
awaits one of these six and then touches thread-affine state must check the
wait's result and `co_await alt::resume_on(...)` before touching it.

Step 5 is the interesting one. Cancellation is checked on entry to every
suspension point, and also at four resumptions. Three of them this library
owns: a hop through `alt::resume_on`, a child `alt::task` completing and
handing control back to its parent, and the stop callback a wait on one of
those six waits registers. The fourth belongs to a caller: an awaiter written
by hand can join them by opting in with
[`alt::cancellation_ref`](cancellation_ref.md). The first two observe the stop
and tear the chain down instead of resuming; the third resumes the waiter
deliberately, as just described; the fourth does whichever of those its author
chose. So the resume in step 5 does not happen at all if a stop was requested
while the task sat there, and step 6's teardown takes its place.

A foreign awaitable that resumes the frame itself and did not opt in is the
exception, and it is checked on entry only. See [Foreign awaitables are checked
on entry only unless they opt
in](#foreign-awaitables-are-checked-on-entry-only-unless-they-opt-in).

```cpp
#include <alt/cancellable_task.hpp>
#include <alt/task.hpp>

// An ordinary child task. Nothing about it is cancellation-aware.
alt::task<result> crunch(input data)
{
    co_await alt::resume_on(worker_pool);
    co_return expensive(data);
}

// The window's coroutine. Plain signature, no cancellation parameters.
alt::cancellable_task on_button_pressed(window* w)
{
    const input data = w->read_fields();          // UI thread
    const result r = co_await crunch(data);       // hops to the pool and back
    co_await alt::resume_on(w->ui_scheduler());    // <-- cancellation observed here
    w->show(r);                                    // never runs if cancelled
}

// At the button press.
m_running = alt::start(on_button_pressed(this),
                        {.home = alt::scheduler_ref{m_ui_scheduler}});

// When the window closes. Returns immediately; the frames die later.
m_running.request_stop();
```

A runnable, self-contained version of this scenario, using
`alt::manual_scheduler` in place of a real UI event loop and thread pool:

```cpp
#include <alt/cancellable_task.hpp>
#include <alt/scheduler.hpp>
#include <alt/task.hpp>

alt::manual_scheduler worker_pool;

struct window
{
    alt::manual_scheduler m_ui_scheduler;

    int read_fields() { /* ... */ return 21; }
    alt::manual_scheduler& ui_scheduler() { return m_ui_scheduler; }
    void show(int result) { /* never called below */ }
};

alt::task<int> crunch(int data)
{
    co_await alt::resume_on(worker_pool);
    co_return data * 2;
}

alt::cancellable_task on_button_pressed(window* w)
{
    const int data = w->read_fields();
    const int r    = co_await crunch(data);
    co_await alt::resume_on(w->ui_scheduler());
    w->show(r);
}

int main()
{
    window w;
    alt::cancellation_handle running =
      alt::start(on_button_pressed(&w), {.home = alt::scheduler_ref{w.ui_scheduler()}});

    running.request_stop();     // the window "closes"

    worker_pool.drain();        // crunch() finishes; on_button_pressed hits its
                                 // next suspension point and the chain is torn
                                 // down instead of resuming
    w.ui_scheduler().drain();   // runs the posted teardown; show() never fires
}
```

## Interface

```cpp
struct exception_handler
{
    void (*invoke)(std::exception_ptr error, void* state) = nullptr;
    void* state                                           = nullptr;
};

struct completion_handler
{
    void (*invoke)(void* state) noexcept = nullptr;
    void* state                          = nullptr;
};

class cancellation_handle
{
public:
    cancellation_handle() noexcept;
    explicit cancellation_handle(std::stop_source source) noexcept;

    bool request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] std::stop_token get_token() const noexcept;
    [[nodiscard]] std::stop_source get_source() const noexcept;
};

struct start_options
{
    std::stop_token    token{};
    scheduler_ref      home{};
    exception_handler  on_exception{};
    completion_handler on_completion{};
};

class [[nodiscard]] cancellable_task
{
public:
    using promise_type = /* unspecified */;

    cancellable_task(cancellable_task&& other) noexcept;
    cancellable_task& operator=(cancellable_task&&)      = delete;
    cancellable_task(const cancellable_task&)            = delete;
    cancellable_task& operator=(const cancellable_task&) = delete;
    ~cancellable_task();
};

[[nodiscard]] cancellation_handle start(cancellable_task t, const start_options& options = {});

struct cancellation_point
{
    [[nodiscard]] bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> h) const noexcept;
    void await_resume() const noexcept;
};
```

`start` begins the coroutine and returns a `cancellation_handle` — the only
thing the caller needs to keep, valid for as long as the caller likes, even
after the task finishes or destroys itself, because the underlying stop state
is reference-counted by the standard library rather than living in the
coroutine frame. `cancellation_handle` is copyable, so several owners can
hold independent handles to the same task.

`start_options` is every optional piece of configuration:

- `token`: an external `std::stop_token` to link in, so one outside signal
  (say, an application-wide shutdown token) can cancel many tasks. The
  returned handle's own `request_stop()` still works regardless.
- `home`: where to destroy the chain's frames once cancellation is observed —
  see below.
- `on_exception`: where an exception that escapes the coroutine body goes,
  since a root has no caller to propagate it to. Left empty, an escaping
  exception calls `std::terminate`.

`cancellation_point` is a no-cost suspension point for code that has none of
its own:

```cpp
alt::cancellable_task long_computation()
{
    for (int chunk = 0; chunk < total_chunks; ++chunk)
    {
        co_await alt::cancellation_point{};
        process(chunk);
    }
}
```

Awaited inside a `cancellable_task` or `task`, it costs one atomic load and
suspends only if a stop has been requested, in which case the chain is torn
down there. Awaited anywhere else — a plain coroutine with no cancellation
machinery — it is an inert no-op, so a helper function that awaits one is
safe to call from any coroutine.

## Every frame in the chain must be owned by the root

The teardown works by destroying the root frame and letting ownership do the
rest: destroying the root runs the destructor of the await temporary in scope
there, which owns the child `task`, which destroys the child frame, and so on
down to the leaf. The cascade therefore reaches exactly those frames that are
**transitively owned by the root frame**, which is what awaiting a `task`
directly — `co_await child()`, or `co_await std::move(child)` where `child` is
a local of the awaiting coroutine — gives you.

A `task` held anywhere else does not qualify. If a member variable or an outer
local owns the `task`, and a cancellable root awaits it from there, cancelling
the root destroys the root but not that task's frame. The frame survives,
suspended, with a continuation pointing at storage that no longer exists.
Resuming it, awaiting it again, or letting its owner's destructor destroy it
later and run anything that touches the continuation is a use-after-free.

Keep every `task` in a cancellable chain owned by the await expression that
awaits it. A `task` that must outlive one await is not a participant in the
cascade and must not be awaited from a cancellable root.

## Cancellation is not acknowledged

`request_stop()` returns immediately and tells you nothing about *when* the
frames actually die — only that a stop has been requested. This is
deliberate, not a shortcut: a `request_stop()` that blocked until the frames
were gone would deadlock in exactly the motivating scenario, whenever the
task needs the calling thread to make progress (posting its next resumption
to a scheduler the calling thread itself pumps, for instance the UI thread in
the scenario above). If code after `request_stop()` needs to know teardown
has actually happened, it needs its own signal for that — `cancellable_task`
does not provide one.

Note what this does and does not say about a chain parked on one of the six
waits listed at the top of this page, which is the case the narrowing of point 6 above describes.
There, `request_stop()` really does run body code before it returns, and with no
`home` it destroys the frames inside the call as well — so for that chain it is
the *acknowledgement* that is missing rather than the work: nothing tells the
caller which of the two happened, and the caller still must not assume either.
The no-deadlock rationale is unaffected, since a chain whose next resumption
goes through a scheduler the calling thread pumps is exactly the case where the
teardown cannot happen inside the call.

## Frames are destroyed on the thread that observed cancellation, unless a home scheduler is given

With no `home` in `start_options`, the chain is destroyed in place, on
whatever thread happens to reach the next suspension point after
cancellation was requested — which, in the motivating scenario, is the
worker thread the calculation ran on. Destroying the frame runs the
destructors of the coroutine's parameters and any live locals, so by default
**those destructors run off the UI thread**, which is frequently illegal for
UI-owned state. Supplying `home` (as in the examples above) instead posts the
destruction to that scheduler, so teardown happens wherever `home` is pumped
— typically back on the UI thread.

A home scheduler that is supplied but never pumped, or one destroyed while
this teardown work is still queued, leaks the entire chain silently: nothing
about the deferred path ever raises an error for it, it simply never
destroys the frames.

## An owner can be told when the task is gone

Set `on_completion` in `start_options` and the task calls it once, as its frame
is destroyed:

```cpp
alt::start(job(), {.on_completion = {.invoke = &task_gone, .state = &owner}});
```

The frame's destruction is the only point that normal completion and cancelled
teardown both pass through, so the hook fires on either path, on whichever thread
destroyed the frame. On the toolchains this library
supports (GCC and Clang) it is observed to run after the body's locals have been
destroyed, with only the frame storage left; the standard does not order a
suspended frame's live locals against its promise on the `destroy()` path, so
that is pinned by `TheCompletionHookFiresOnCancelledTeardown` rather than
guaranteed. It never fires for a task that was never started.

The hook must not throw. The function pointer is `noexcept` in its type for that
reason, since it runs from a destructor. It must also not touch the task: the
frame is already being torn down. A request to stop through a copy of the
`cancellation_handle` is safe, because the stop state is reference counted and
outlives the frame.

A caller who wants a per-task notification *inside* the body, while its locals
are still alive, uses an `alt::scope_exit` local instead. That also runs on both
paths.

For a set of tasks, [`alt::task_group`](task_group.md) is the teardown point: it
owns this hook and the stop token, and gives a point at which every task it
spawned is known gone.

## Foreign awaitables are checked on entry only unless they opt in

Cancellation is checked in two places. On entry to every suspension point,
which covers anything you can `co_await`. And at resumptions, of which there
are four. Three the library owns: `alt::resume_on` returning from a scheduler, a
child `alt::task` completing and transferring back to its parent, and the stop
callback that a wait on `alt::async_event`, `alt::async_latch`,
`alt::async_semaphore`, `alt::async_mutex`, `alt::task_completion_source` or
`alt::task_group::join()` registers (`detail::waiter_stop_release`). The fourth
is a caller's: an awaiter written by hand that opted in with
[`alt::cancellation_ref`](cancellation_ref.md), which is the subject of the end
of this section.

The first two observe the stop and tear the chain down instead of resuming. The
third does the opposite: it unlinks the parked waiter and **resumes** it, with a
result reporting the wait as released rather than satisfied, and lets the body
run on to its next suspension point — where the ordinary teardown then takes
over. It runs on the thread that called `request_stop()` whatever
`start_options::home` says, because `home` governs only where the frames are
destroyed. See the narrowing of point 6 at the top of this page, which is what
that costs the motivating scenario.

What is not covered automatically is a foreign awaitable that resumes the frame
directly, because intercepting a resumption means knowing how that resumption
happens — which the library cannot know for an awaiter it did not write:

```cpp
// A hand-written awaiter that hands the frame to a thread of its own.
struct my_awaiter
{
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) { start_thread_that_resumes(h); }
    void await_resume() const noexcept {}
};

alt::cancellable_task on_press(window* w)
{
    co_await my_awaiter{};   // a stop arriving here is not seen on the way back
    w->show(result());       // so this runs
    co_await alt::cancellation_point{};
}
```

Swap `my_awaiter{}` for `alt::resume_on(pool)` and the stop is observed, the
chain is destroyed, and `w->show(...)` never runs.

For an awaitable you do not control, put a `co_await alt::cancellation_point{}`
immediately after the resume. It observes the stop and costs one atomic load
when there is nothing to observe. The statements between the resume and that
`co_await` still run, which is the whole of what this mitigation costs.

**An awaiter you do control can take the fourth resumption instead.** Give its
`await_suspend` a template parameter for the promise type, ask
[`alt::get_cancellation_ref`](cancellation_ref.md) for a view of the chain, and
resume through that view rather than through the handle:

```cpp
struct my_opted_in_awaiter
{
    bool await_ready() const noexcept { return false; }

    // The typed handle, not std::coroutine_handle<>: the erased one has no promise.
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h)
    {
        m_chain = alt::get_cancellation_ref(h);
        start_thread_that_completes(this, h);
    }

    void await_resume() const noexcept {}

    // What that thread calls, in place of h.resume().
    void on_complete(std::coroutine_handle<> h)
    {
        m_chain.resume_or_abandon(h);   // last statement: either branch may free the frame
    }

    alt::cancellation_ref m_chain;
};
```

Awaiting that in place of `my_awaiter{}` above gets `alt::resume_on`'s outcome:
a stop arriving while the operation is in flight is observed on the way back,
the chain is destroyed, and `w->show(...)` never runs. See
[`alt::cancellation_ref`](cancellation_ref.md) for the delicate parts — the
lifetime of the view, the one-call-per-suspension rule, why `abandon()` requires
a stop to have been requested, and what to do when the operation stops for a
reason this chain's token never saw.

Note that the entry-only gap applies per frame. In a chain where a child is
parked on a foreign awaitable that did not opt in, that child's own body resumes
and runs; the transfer back to its parent is then intercepted, so the parent's
body does not.

## A body with no further suspension point runs to completion

Cancellation is only checked on entry to a suspension point. A coroutine that
reaches no more `co_await` expressions after a stop is requested — such as
`crunch()` above, once it has resumed on the worker and has nothing left but
`co_return` — completes normally, cancellation or not. This is exactly why
`cancellation_point` exists: insert one into a long computation to give it
somewhere to actually stop.

## A task parked on something that never resumes never self-destructs

Destruction happens *at* a suspension point, driven by that suspension point
being reached — not on a timer, and not the instant `request_stop()` is
called. A task awaiting something that will never complete or never resume
it (an operation nobody drives, a scheduler nobody pumps) never reaches
another suspension point, and so never tears itself down, no matter how long
ago cancellation was requested. This is the same failure mode as the leaked
home scheduler above, viewed from the awaited side rather than the teardown
side.

Those six waits are the exception, and they are the exception
deliberately. A coroutine parked on an [`alt::async_event`](async_event.md)
nobody ever sets — or on a latch nobody counts down, a semaphore nobody
releases, a mutex nobody unlocks, a task_completion_source nobody sets, or a
`group.join()` whose tasks never finish — is *not* stuck here: the stop callback its
awaiter registered releases the waiter, its body resumes with the released
result, and the chain tears itself down at the following suspension point. That
is the whole purpose of those stop callbacks. Everything else an awaitable can
be — a foreign awaitable that resumes the frame itself, a scheduler nobody pumps
— is covered by the rule above.

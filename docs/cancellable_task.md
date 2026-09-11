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
   instead of resuming. No user coroutine body code runs, so nothing touches
   the dead window.

Point 6 is the guarantee: it is provided by the coroutine machinery itself
and does not depend on whatever is being awaited cooperating with
cancellation.

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

    worker_pool.run_all();      // crunch() finishes; on_button_pressed hits its
                                 // next suspension point and the chain is torn
                                 // down instead of resuming
    w.ui_scheduler().run_all(); // runs the posted teardown; show() never fires
}
```

## Interface

```cpp
struct exception_handler
{
    void (*invoke)(std::exception_ptr error, void* state) = nullptr;
    void* state                                           = nullptr;
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
    std::stop_token   token{};
    scheduler_ref     home{};
    exception_handler on_exception{};
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

# Design: `alt` coroutines — `task<T>` and `cancellable_task`

**Date:** 2026-09-10
**Status:** Approved
**Related:** `2026-09-10-coroutines-roadmap.md` (the full component family and its ordering)

**Components:**
- `include/alt/awaitable.hpp` (header-only, public) — new
- `include/alt/task.hpp` (header-only, public) — new
- `include/alt/scheduler.hpp` (header-only, public) — new
- `include/alt/sync_wait.hpp` (header-only, public) — new
- `include/alt/cancellable_task.hpp` (header-only, public) — new
- `include/alt/detail/coroutine.hpp` (header-only, helper) — new
- `include/alt/detail/cancellation.hpp` (header-only, helper) — new

## Goal

A coroutine task that can be told to stop and then destroys itself at its next
suspension point, together with the minimum machinery needed to build and test
it. The motivating scenario:

1. A button press starts a coroutine owned by a window.
2. The coroutine reads data from UI elements on the UI thread.
3. It awaits a thread pool and performs a long calculation on a worker thread.
4. Meanwhile the window closes and requests cancellation.
5. The calculation finishes and the coroutine resumes on the worker thread.
6. At its next `co_await`, the coroutine machinery destroys the whole chain
   instead of resuming. No user coroutine body code runs, so nothing touches
   the dead window.

Point 6 is the requirement that shapes everything: the guarantee is provided by
the coroutine machinery itself and does not depend on the cooperation of
whatever is being awaited.

Target usage:

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
    co_await alt::resume_on(w->ui_scheduler());   // <-- cancellation observed here
    w->show(r);                                   // never runs if cancelled
}

// At the button press.
m_running = alt::start(on_button_pressed(this),
                       {.home = alt::scheduler_ref{m_ui_scheduler}});

// When the window closes. Returns immediately; the frames die later.
m_running.request_stop();
```

## Validated assumption

The design rests on one non-obvious claim: a leaf coroutine may, from inside its
own `await_suspend`, destroy the root of its await chain, cascading destruction
through the owning task objects down to and including its own frame, provided it
then touches nothing. A throwaway probe confirmed this before this document was
written.

| Scenario | Result |
|----------|--------|
| Leaf observes cancellation at a `void`-returning suspend point | Whole chain destroyed, innermost frame first |
| Root observes cancellation before starting a child | Root destroyed, child coroutine never started |
| No cancellation | Chain completes normally, no behavior change |
| Cancellation requested on one thread, observed on a worker | All frames destroyed on the worker thread |

Configurations: GCC 15.2 and Clang 21.1, at `-O0` and `-O2`, all under
AddressSanitizer and UndefinedBehaviorSanitizer. All four combinations clean.
The project baseline of GCC 13 and Clang 19 was not exercised locally and is
covered by CI on the branch.

The probe produced two findings that constrain the implementation:

- The checking wrapper's `await_suspend` must mirror the return type of the
  awaiter it wraps. For a `void` inner it returns `void`, for `bool` it returns
  `true`, and for the symmetric-transfer case it returns
  `std::noop_coroutine()`. Returning `false` would resume a frame that no longer
  exists.
- That return type must be deduced against the actual awaiting promise type, not
  against an erased `std::coroutine_handle<>`, because an awaiter is permitted to
  declare `await_suspend` over a typed handle.

## Architecture

Five layers, each usable without the ones above it.

```
cancellable_task      root ownership, cancellation state, self-destruction
        |
      task<T>         lazy child coroutines, symmetric transfer, context relay
        |
    schedulers        one primitive: post(work_item)
        |
  awaitable layer     concepts, traits, get_awaiter, as_expected
        |
     sync_wait        the only blocking entry point
```

### The awaitable layer

`awaiter` names a type with the three await member functions. `awaitable` names
a type that either is an awaiter or produces one through a member or free
`operator co_await`. `get_awaiter` performs that resolution once so the rest of
the library does not repeat it, and `await_result_t` is the result of awaiting.

These concepts describe **context-free** awaitables only. A promise type's
`await_transform` can make an expression awaitable in one coroutine and not in
another, and no concept can observe that. Phase 2 uses `await_transform` on
`task<T>`, so this limitation is load-bearing rather than academic and is stated
in the Doxygen comment for each concept.

`as_expected` adapts any awaitable into one yielding
`std::expected<await_result_t<A>, std::exception_ptr>`, which serves the
`CLAUDE.md` preference for `std::expected` without making every await site in
the library pay for it. It lives here, not with `task<T>`, because it has no
dependency on the task type.

### `task<T>`

Lazy, move-only, single-consumer, and `[[nodiscard]]`. It suspends at
`initial_suspend`, so awaiting it is what starts it, and the awaiting
coroutine's handle becomes its continuation. Its final awaiter returns that
continuation as a handle rather than resuming it, which is symmetric transfer, so
a chain of arbitrary depth unwinds in constant stack space. The task object owns
the frame and destroys it in its destructor, which is precisely what makes the
cancellation cascade work.

`task<void>` and `task<T&>` are supported through one internal result store, so
the task template itself is not specialized. Arrays are not supported. Awaiting
an lvalue yields a reference to the result; awaiting an rvalue moves it out.

Discarding a task is nearly always a bug, because a lazy task nobody awaits
simply never runs, hence `[[nodiscard]]`.

Its promise holds one extra pointer, `detail::cancellation_context*`, which is
null unless a `cancellable_task` root installed one. A `task<T>` awaited outside
a cancellable root therefore pays one null pointer test per suspension point and
nothing else.

### Schedulers

A scheduler has exactly one required operation:

```cpp
struct work_item
{
    void (*invoke)(void* state);
    void* state;
};

template<typename S>
concept scheduler = requires(S& s, work_item w) { s.post(w); };
```

A `work_item` is a function pointer plus a `void*`, so posting allocates
nothing. Everything else is built on top of it. `resume_on(s)` posts a resume
trampoline; phase 2 posts a destroy trampoline against the same primitive. A
`schedule()` member requirement would have forced a second concept and would
have left phase 2 unable to express "destroy this frame over there". This single
decision is what makes the opt-in home scheduler cheap.

`scheduler_ref` is a non-owning erased reference, a `void*` plus a post function
pointer, constructible from any `scheduler`. It allocates nothing and is what
`cancellable_task` stores.

`trampoline_scheduler` invokes a work item immediately when it is not already
running one on this thread, and queues it otherwise, with the outermost
invocation draining the queue before it returns. Stack depth is therefore
bounded by a constant regardless of how long a resume chain is, while a
non-reentrant post still runs synchronously, which is the property tests want
from an inline scheduler. The queue is thread-local, so two instances on one
thread share a drain, which is harmless because work items are self-contained.
There is no plain `inline_scheduler`: the only difference would be that it
overflows the stack.

`manual_scheduler` is a thread-safe FIFO that a test pumps by hand. Beyond
`post`, it offers `run_one`, `run_all`, `empty`, a blocking pump with an
external `wake`, and the debug assertion described below.

### `sync_wait`

The only blocking entry point in the library. `task<T>` deliberately has no
`get`. Two overloads:

- `sync_wait(awaitable)` starts the awaitable on the calling thread and parks on
  a `std::binary_semaphore` that completion signals, then returns the result or
  rethrows.
- `sync_wait(awaitable, drivable_scheduler&)` pumps the scheduler until the work
  completes, rather than parking. This is how the C++26 execution model solves
  the same problem, where `sync_wait` drives a run loop of its own.

Without the second overload, calling `sync_wait` on work that needs the calling
thread to pump a `manual_scheduler` deadlocks. An earlier draft of this design
proposed detecting that automatically, by marking the parked thread in a
thread-local and asserting in `manual_scheduler::post`. That check is unsound:
posting from a parked thread is perfectly legitimate when a *different* thread
pumps the scheduler, so the assertion would fire on correct programs. No cheap
sound check exists, because whether a deadlock occurs depends on which threads
will pump later.

The mitigation is therefore documentation plus a default that steers away from
the trap. The driving overload is the documented choice whenever a
`manual_scheduler` is involved, which covers every single-threaded test, and a
mistake surfaces as a ctest timeout rather than silent corruption.

### Cancellation

Three objects:

| Object | Lives in | Holds |
|--------|----------|-------|
| `detail::cancellation_context` | the root coroutine frame | root handle, `std::stop_token`, optional `scheduler_ref` |
| `std::stop_source` | wherever the owner keeps it | the shared, reference-counted stop state |
| `cancellation_handle` | the owner | a `std::stop_source` copy |

The context lives in the root's frame, so nothing in this design needs bespoke
reference counting. Each awaited `task<T>` receives a pointer to it through
`await_transform` and relays the same pointer further down. The leaf copies the
root handle to its own stack before destroying anything, so the pointer
dangling during the cascade is harmless. The `std::stop_source` is separately
reference counted by the standard library, which is why an owner can safely keep
one, query it, and request stop on it after the frame is gone.

**Where the check happens.** `await_transform` wraps every inner awaitable in
`detail::cancellation_checked<Inner>`, whose `await_ready` is

```cpp
bool await_ready() { return !stop_requested() && inner.await_ready(); }
```

When cancellation has been requested this returns `false` unconditionally, which
forces a suspension so that `await_suspend` can run and destroy. When it has
not, it delegates, so an already-ready awaitable still costs nothing and
`cancellation_point` is free on the uncancelled path. `await_suspend` then
re-checks and, if cancelled, destroys before delegating to the inner awaiter.

Checking on entry to a suspension point rather than on resumption is what makes
destruction safe: the inner awaitable has not yet registered a callback, started
a thread, or queued anything, so nothing is in flight at the moment the frames
die.

**Who destroys what.** Only the root is destroyed, and only a leaf triggers it.
Destroying the root frame runs the destructor of the await temporary in scope
there, which owns the child `task<T>`, which destroys the child frame, and so on
down to the leaf whose `await_suspend` is on the stack. That function must
therefore touch no member and no frame-resident local after the destroy call.
It is the single delicate function in the design, it is written once, it carries
a comment saying so, and it is tested under AddressSanitizer.

**Where destruction happens.** By default the observing thread destroys the
chain in place, which is immediate. When a home scheduler was supplied, the leaf
instead posts a destroy trampoline to it and returns, leaving the frames
suspended until that post runs. Frames holding UI-affine state then always die
on the UI thread. Both paths are sound; the probe exercised the first and the
mechanism for the second is the same `post(work_item)` used by `resume_on`.

## Public API

```cpp
// ---------------------------------------------------------------- awaitable.hpp
namespace alt
{
	template<typename T> concept awaiter = /* ... */;
	template<typename T> concept awaitable = /* ... */;
	template<typename T, typename R> concept awaitable_of = /* ... */;

	template<awaitable A> using await_result_t = /* ... */;

	template<awaitable A> decltype(auto) get_awaiter(A&& a);

	template<awaitable A> auto as_expected(A&& a);   // -> expected<R, exception_ptr>
}

// -------------------------------------------------------------------- task.hpp
namespace alt
{
	template<typename T = void>
	class [[nodiscard]] task
	{
	public:
		task(task&& other) noexcept;
		task& operator=(task&& other) noexcept;
		task(const task&) = delete;
		task& operator=(const task&) = delete;
		~task();

		// True once the coroutine has run to completion, so its result is available
		// without suspending. False for an unstarted, running, or moved-from task.
		[[nodiscard]] bool is_ready() const noexcept;

		auto operator co_await() const& noexcept;   // yields T&  (or void)
		auto operator co_await() && noexcept;       // yields T&& (or void)
	};
}

// --------------------------------------------------------------- scheduler.hpp
namespace alt
{
	struct work_item
	{
		void (*invoke)(void* state);
		void* state;
	};

	template<typename S> concept scheduler = requires(S& s, work_item w) { s.post(w); };

	// A scheduler whose queue a blocked thread can drive itself.
	template<typename S>
	concept drivable_scheduler = scheduler<S> && requires(S& s) {
		{ s.run_one() } -> std::same_as<bool>;        // false when the queue was empty
		{ s.run_one_or_wait() } -> std::same_as<bool>;// blocks; false when woken instead
		s.wake();                                     // releases a blocked run_one_or_wait
	};

	class scheduler_ref
	{
	public:
		scheduler_ref() noexcept;                        // empty
		template<scheduler S> explicit scheduler_ref(S& s) noexcept;
		explicit operator bool() const noexcept;
		void post(work_item w) const;
	};

	class trampoline_scheduler { public: void post(work_item w); };

	class manual_scheduler
	{
	public:
		void post(work_item w);
		bool run_one();             // false when the queue was empty
		bool run_one_or_wait();     // blocks until an item ran or wake() was called
		std::size_t run_all();
		bool empty() const;
		void wake();                // releases a blocked run_one_or_wait
	};

	template<scheduler S> auto resume_on(S& s) noexcept;
}

// --------------------------------------------------------------- sync_wait.hpp
namespace alt
{
	template<awaitable A> await_result_t<A> sync_wait(A&& a);
	template<awaitable A, drivable_scheduler S> await_result_t<A> sync_wait(A&& a, S& s);
}

// -------------------------------------------------------- cancellable_task.hpp
namespace alt
{
	class [[nodiscard]] cancellable_task;      // coroutine return type; lazy

	class cancellation_handle                  // copyable, outlives the frame
	{
	public:
		bool request_stop() noexcept;          // true when this call requested it
		[[nodiscard]] bool stop_requested() const noexcept;
		[[nodiscard]] std::stop_token get_token() const noexcept;
		[[nodiscard]] std::stop_source get_source() const noexcept;
	};

	struct exception_handler          // erased and stateful, like work_item
	{
		void (*invoke)(std::exception_ptr, void* state) = nullptr;
		void* state = nullptr;
	};

	struct start_options
	{
		std::stop_token token{};      // linked in; the task always owns its own source
		scheduler_ref home{};         // empty: destroy in place
		exception_handler on_exception{};   // empty: std::terminate
	};

	[[nodiscard]] cancellation_handle start(cancellable_task t, start_options options = {});

	// co_await this where a long computation has no natural suspension point.
	// Awaited inside alt's coroutine types, await_transform wraps it in the
	// cancellation check. Awaited anywhere else it is a no-op that never suspends,
	// so it is safe to place in a helper that is not always run under a root.
	struct cancellation_point
	{
		bool await_ready() const noexcept { return true; }
		void await_suspend(std::coroutine_handle<>) const noexcept {}
		void await_resume() const noexcept {}
	};
}
```

### Why `start` rather than an eager constructor

A `cancellable_task` needs its stop token, its home scheduler, and its exception
handler installed **before** its body runs, and a coroutine's caller does not
receive the return object until after an eager body has reached its first
suspension point. The three candidate ways to configure it were:

| Approach | Verdict |
|----------|---------|
| Type-sniff the coroutine's parameter list from the promise constructor | Works, and is a standard mechanism, but it is implicit magic: a `std::stop_token` parameter would silently change behavior. |
| Lazy type plus a `start()` member | Explicit, but forgetting the call is a silent no-op. |
| Lazy type plus a free `alt::start` returning the handle | Chosen. The handle can only be obtained by starting, so the call cannot be forgotten, and the coroutine's own signature stays clean. |

`cancellable_task` is therefore the lazy, not-yet-running coroutine return type,
exactly parallel to `task<T>`, and `alt::start` is where ownership transfers to
the coroutine itself. This differs from the earlier sketch, in which the type was
eager and exposed a `stop_source` member; that shape could not be configured.

**An external token is linked, not adopted.** The promise always owns a
`std::stop_source`, and a token supplied through `start_options` is forwarded
into it by a `std::stop_callback` held in the frame. Adopting the caller's token
instead would leave the promise with no source, so
`cancellation_handle::request_stop` would silently do nothing whenever a token
was supplied. Linking also gives the teardown the synchronization it needs for
free, because `std::stop_callback`'s destructor blocks until a concurrently
running callback has finished. A token that has already been stopped fires its
callback the moment it is registered, so cancellation is then observed at the
task's first suspension point.

`cancellable_task` returns void and is not awaitable. It is a root, so it has
nobody to return a value to and nobody to propagate an exception to. On an
unhandled exception it calls `start_options::on_exception` if one was supplied
and otherwise calls `std::terminate`, matching a detached thread rather than
silently swallowing a failure.

## Semantics and guarantees

1. **No user code after observation.** Once cancellation is observed, no
   statement of any coroutine body in the chain executes. Only destructors of
   live locals and of coroutine parameters run.
2. **Observation is at entry to a suspension point.** A coroutine that never
   reaches another `co_await` runs to completion and is unaffected. Long compute
   loops should `co_await alt::cancellation_point{}` to create one.
3. **Cancellation is not acknowledged.** `request_stop` returns immediately. The
   owner learns nothing about when the frames actually died, by design, because a
   waiting `request_stop` would deadlock whenever the task needs the calling
   thread to make progress.
4. **The handle outlives the task.** Every `cancellation_handle` operation is
   safe after the coroutine has finished or self-destructed, because the state is
   the standard library's reference-counted stop state.
5. **Cancellation propagates only through `alt::task<T>`.** A third-party
   awaitable in the middle of a chain is still checked before it starts, which is
   where the check matters, but a coroutine type from another library will not
   relay the context to its own children.
6. **Destruction order is innermost frame first.** Root frame destruction reaches
   the leaf through the owning task destructors, so leaf locals are destroyed
   before their parents'.

## Edge cases

| Case | Behavior |
|------|----------|
| `task<T>` destroyed before it is ever awaited | Frame destroyed, body never runs, no leak |
| `task<T>` awaited twice | Precondition violation, debug assertion |
| Moved-from `task<T>` | Null handle, destructor is a no-op, awaiting it asserts |
| Throw before the first suspension point | Captured in the promise, rethrown at `await_resume` |
| Throw from the move constructor of a returned value | Captured, rethrown at `await_resume` |
| `task<T>` with move-only, reference, or `void` result | All supported |
| Chain deep enough to overflow the stack | Constant stack via symmetric transfer |
| `request_stop` concurrent with normal completion | Safe. Only the coroutine ever destroys; `request_stop` only touches the stop state, which is independently reference counted |
| `request_stop` on an already-finished task | No-op |
| `request_stop` called twice | Second call is a no-op; the chain is already gone after the first observation |
| Cancellation observed with a home scheduler that never runs | Frames stay suspended and leak. Documented; the same is true of any posted work |
| `sync_wait` on work that never completes and never posts | Hangs, as any wait on something that never happens must |
| Blocking `sync_wait` on work needing the calling thread to pump | Deadlocks. Documented; use the driving overload |
| Empty `scheduler_ref` posted to | Precondition violation, debug assertion |
| `cancellation_point` awaited outside a cancellable root | Never suspends, costs one null pointer test |
| Cancellation requested before `start` is called | Observed at the first suspension point; the body runs up to it |

## Testing strategy

Tests go in `tests/test_awaitable.cpp`, `tests/test_coroutine.cpp`,
`tests/test_scheduler.cpp`, `tests/test_cancellation.cpp`, `tests/test_task.cpp`,
`tests/test_sync_wait.cpp`, and `tests/test_cancellable_task.cpp`, each
registered in `tests/CMakeLists.txt`. The two covering `detail/` headers follow
the precedent of `tests/test_digest.cpp` and `tests/test_hash.cpp`.

Everything above the cancellation layer is tested single-threaded and
deterministically through `manual_scheduler`, so failures are reproducible.
Cancellation adds three groups:

- **Deterministic.** A tracker type counts construction and destruction of
  locals and of coroutine parameters at every level of a chain. Each test asserts
  the exact destruction order and that no body statement past the cancellation
  point ran, using a flag set by the body rather than by output.
- **Cross-thread.** `std::jthread` resumes a parked chain after
  `request_stop`, covering the motivating scenario. No thread pool is needed and
  none exists until phase 3.
- **Sanitized.** The whole suite already runs under the `debug-asan` preset,
  which is where the destroy-from-`await_suspend` cascade is verified. This is a
  hard requirement for this component rather than a general quality check.

Matrix to cover for cancellation: observed at the root, at a middle frame, and at
a leaf; observed at a `void`, `bool`, and handle-returning suspension point;
destroyed in place and via a home scheduler; requested before `start`, while
running, and after completion; and a chain with no cancellation at all, to prove
the wrapper is transparent.

## Files changed

| File | Change |
|------|--------|
| `include/alt/awaitable.hpp` | New |
| `include/alt/task.hpp` | New |
| `include/alt/scheduler.hpp` | New |
| `include/alt/sync_wait.hpp` | New |
| `include/alt/cancellable_task.hpp` | New |
| `include/alt/detail/coroutine.hpp` | New |
| `include/alt/detail/cancellation.hpp` | New |
| `tests/test_awaitable.cpp` | New |
| `tests/test_coroutine.cpp` | New, covers `detail/coroutine.hpp` |
| `tests/test_cancellation.cpp` | New, covers `detail/cancellation.hpp` |
| `tests/test_task.cpp` | New |
| `tests/test_scheduler.cpp` | New |
| `tests/test_sync_wait.cpp` | New |
| `tests/test_cancellable_task.cpp` | New |
| `tests/CMakeLists.txt` | Register the seven test files and link `Threads::Threads` |
| `docs/awaitable.md` | New component page |
| `docs/task.md` | New component page |
| `docs/scheduler.md` | New component page |
| `docs/sync_wait.md` | New component page |
| `docs/cancellable_task.md` | New component page |
| `README.md` | Mention the coroutine components |

All seven headers are header-only, so the root `CMakeLists.txt` needs no change:
`install(DIRECTORY include/alt)` is recursive and the `src/` glob finds nothing
new. Phase 2 tests use `std::jthread`, so `tests/CMakeLists.txt` links
`Threads::Threads`.

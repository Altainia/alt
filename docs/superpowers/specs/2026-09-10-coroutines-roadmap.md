# Roadmap: `alt` coroutine components

**Date:** 2026-09-10
**Status:** Approved
**Related:** `2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1 and 2)

## Purpose

The driving requirement is a coroutine type for a UI framework that can be told
to stop and then destroys itself at its next opportunity, so that a window can
close while one of its coroutines is still running work on a worker thread.
That type cannot be built or tested in isolation, so this roadmap fixes the
order of the surrounding machinery and records what is deliberately excluded.

## Ordering principle

Each phase depends only on phases before it. Every phase gets its own design
spec, implementation plan, and pull request.

| Phase | Components | Header | Depends on |
|-------|-----------|--------|-----------|
| 1 | `awaiter`, `awaitable`, `awaitable_of`, `await_result_t`, `get_awaiter`, `as_expected` | `include/alt/awaitable.hpp` | nothing |
| 1 | `task<T>` | `include/alt/task.hpp` | awaitable layer |
| 1 | `scheduler` concept, `work_item`, `scheduler_ref`, `trampoline_scheduler`, `manual_scheduler`, `resume_on` | `include/alt/scheduler.hpp` | awaitable layer |
| 1 | `sync_wait` | `include/alt/sync_wait.hpp` | awaitable layer, schedulers |
| 1 | continuation slot and result storage internals | `include/alt/detail/coroutine.hpp` | nothing |
| 2 | `cancellable_task`, `cancellation_handle`, `start`, `start_options`, `exception_handler`, `cancellation_point` | `include/alt/cancellable_task.hpp` | all of phase 1 |
| 2 | cancellation context and the checking awaiter | `include/alt/detail/cancellation.hpp` | phase 1 |
| 3 | `thread_pool`, `run_loop` | `include/alt/thread_pool.hpp`, `include/alt/run_loop.hpp` | phase 1 |
| 4 | `when_all`, `when_any` | `include/alt/when_all.hpp`, `include/alt/when_any.hpp` | phases 1 and 3 |
| 5 | `async_event`, `async_mutex`, `async_latch`, `async_semaphore`, `task_completion_source`, `task_group` | one header each | phases 1 and 3 |
| 6 | `shared_task<T>`, `async_generator<T>` | one header each | phases 1 and 3 |

Phases 1 and 2 are specified together in the companion design document. They
are inseparable because `task<T>` itself carries the cancellation hook: its
promise holds a context pointer and its `await_transform` injects the check.
Designing `task<T>` first and retrofitting cancellation would have forced either
a rewrite of the type or a second cancellation-aware child task type, and the
latter would mean a chain is only cancellable when every link opts in.

## Notes on individual phases

**Phase 1 places `sync_wait` and `manual_scheduler` early.** Neither is needed
by the headline feature at runtime, but both are needed to test it. A
deterministic single-threaded pump is the only way to pin down self-destruction
semantics precisely, and a blocking wait is the only way for a test body to
observe the result of an asynchronous chain.

**Phases 1 and 2 already introduced the threading dependency; phase 3 adds the
first compiled component that needs it.** Phase 1's `scheduler.hpp` uses
`std::mutex` and `std::condition_variable`, phase 2's `sync_wait.hpp` uses
`std::binary_semaphore`, and `cancellable_task.hpp` uses `std::stop_source` and
`std::stop_callback`, so the `alt` target links `Threads::Threads` publicly and
the exported package config carries a `find_dependency(Threads)` from phase 2
onward. What phase 3 adds is a real compiled translation unit with threads in it
— the library still has only `src/version.cpp` — which is why the thread pool is
deferred past the headline feature. Phase 2 tests use bare `std::jthread`
instead of a pool.

**Phase 5 contains the pieces a UI framework actually reaches for.** A
completion source that turns a callback API into an awaitable is what lets a
coroutine await a button click or a dialog result. A task group gives an owner a
point at which every task it owns is known to be gone. Neither is required by
the stated scenario, because a window can hold one `std::stop_source` and pass
its token to every task it owns, which makes cancelling a whole window's worth
of coroutines a single call.

## Deliberate exclusions

| Excluded | Reason |
|----------|--------|
| A synchronous generator | `std::generator` covers it. Note that libc++ does not yet ship `<generator>`, so a consumer on Clang with libc++ has no generator at all; this is not a reason for `alt` to add one. |
| Coroutine allocator support | No requirement here calls for custom frame allocation, and adding it infects every promise type in the family. |
| Sender and receiver alignment with C++26 `std::execution` | A different concurrency model. Adopting it would dominate the design without serving the UI scenario. Phase 1's awaitable concepts do not preclude a later adapter. |
| A cancellable child task type | Cancellation lives in `task<T>` and in the root, so every chain is cancellable without opt-in. |
| Blocking cancellation | A `request_stop` that waited for the frame to die would deadlock whenever the task needs the calling thread to make progress, which is exactly the UI thread case. |

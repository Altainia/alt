# task

**Header:** `<alt/task.hpp>`

`alt::task<T>` is a lazy, move-only, single-consumer coroutine result. The
coroutine's body does not run until the task is awaited; awaiting it is what
starts it.

```cpp
#include <alt/task.hpp>
#include <alt/sync_wait.hpp>

alt::task<int> square(int n)
{
    co_return n * n;
}

alt::task<int> sum_of_squares(int a, int b)
{
    const int sa = co_await square(a);
    const int sb = co_await square(b);
    co_return sa + sb;
}

const int total = alt::sync_wait(sum_of_squares(3, 4));  // 25
```

## Interface

```cpp
template<typename T = void>
class [[nodiscard]] task
{
public:
    using promise_type = /* unspecified */;
    using value_type   = T;

    task() noexcept;
    task(task&& other) noexcept;
    task& operator=(task&& other) noexcept;
    task(const task&)            = delete;
    task& operator=(const task&) = delete;
    ~task();

    [[nodiscard]] bool is_ready() const noexcept;

    [[nodiscard]] auto operator co_await() const& noexcept;
    [[nodiscard]] auto operator co_await() && noexcept;
};
```

`T` may be `void`, a value type, or a reference type. Arrays are not
supported.

`is_ready()` is true once the coroutine has run to completion; it is false for
an empty, unstarted, or still-running task, and a ready task's result can be
read without suspending.

Awaiting an lvalue task (`operator co_await() const&`) yields a reference to
the stored result. Awaiting an rvalue task (`operator co_await() &&`, as
happens with a temporary such as `square(a)` above) yields an rvalue reference
to the stored result, so the usual way to consume it — `auto x = co_await
square(a);` — moves it out.

Bind that rvalue result with `auto`, not with `auto&&` or `decltype(auto)`:
those keep a reference into a frame that the await temporary owns, and that
frame is destroyed at the end of the full-expression, so the reference dangles
on the very next line. `auto` copies or moves the value out while the frame is
still alive and is always safe. Awaiting an *lvalue* task is the case where
`auto&&` is fine, because the task object — and so the frame — outlives the
await.

The task owns the coroutine frame and destroys it — running any live locals'
destructors — in its own destructor. This is also what lets
[`alt::cancellable_task`](cancellable_task.md) tear down a whole chain by
destroying only its root: destroying a `task` destroys the frame it owns,
which destroys the `task` objects for any children it was awaiting, and so on
down to the leaf.

## Awaitable exactly once

A `task` may be awaited at most once. Awaiting it a second time, or awaiting
one that is empty or moved-from, is undefined behavior (checked with an
`assert` in a debug build). Move the task if it needs to be handed to
something else before being awaited; do not keep a copy to await later —
`task` is not copyable for exactly this reason.

## Discarding a task means it never runs

`task` is lazy and `[[nodiscard]]`: nothing runs until the task is awaited. A
task that is constructed and then discarded — never bound to a variable,
never `co_await`-ed — never starts its coroutine body at all. This is almost
never what's intended, which is why the type is marked `[[nodiscard]]`; treat
a discarded task as a bug to fix, not a warning to suppress.

## Discarding an await *result* is `[[nodiscard]]` too

A different thing from the above, and newer. Every `co_await` inside an
`alt::task` or an `alt::cancellable_task` is wrapped by the cancellation layer,
which injects the stop check ahead of the suspension point, and that wrapper's
`await_resume` carries `[[nodiscard]]`. So the rule is: **a value-returning
await inside an `alt::task` is `[[nodiscard]]`**, whatever is being awaited.
Void-returning awaits are unaffected, having no result to discard.

```cpp
alt::task<void> caller()
{
    co_await square(3);             // warns: the result is dropped
    const int n = co_await square(3);
    use(n);
}
```

This widening arrived with the phase that added the async synchronization
primitives, so a build that was clean before it may now warn — including one
with warnings-as-errors, as this project's own clang-tidy gate is. The fix is to
use the value, or to drop it deliberately with a cast to `void`.

Which compilers act on it was measured rather than assumed, with `-Wall
-Wextra` on GCC 15.2.0 and Clang 21.1.8:

- A discarded `co_await` on an `alt::task<T>` is diagnosed by **GCC as well as
  by Clang** and clang-tidy. Measured for `T = int` and for
  `T = std::expected<void, std::error_code>`.
- A discarded `co_await` on one of the async primitives —
  [`async_event`](async_event.md), [`async_latch`](async_latch.md),
  [`async_semaphore`](async_semaphore.md), [`async_mutex`](async_mutex.md) — is
  diagnosed by Clang and clang-tidy and by **nothing at all on GCC**.
- **What divides those two cases on GCC was not determined**, so neither line
  above generalizes to an awaitable it does not name. It is demonstrably not the
  result type: a `task<std::expected<void, std::error_code>>` warns on GCC,
  while an `async_event` — whose result is that same type — does not.

`include/alt/detail/cancellation.hpp` records the measurement, and why no rule
was inferred from it.

## `task<T&>` — the result outlives the frame only if something else keeps it alive

```cpp
alt::task<int&> counter_ref(int& counter)
{
    ++counter;
    co_return counter;
}
```

Awaiting a `task<T&>` yields a reference to the object the coroutine returned,
not a copy. That object must be kept alive by something other than the task:
the coroutine's own frame is destroyed once the task holding it goes out of
scope, so a reference to a local of that coroutine would dangle. Return a
reference to something the caller (or a wider scope) owns, as `counter` above
is owned by whoever called `counter_ref`.

## Constant stack space is conditional, not guaranteed

Completing a task transfers control straight to its continuation (*symmetric
transfer*), which is what makes a long chain of awaited tasks unwind without
growing the stack — **when the compiler cooperates**. The C++ standard permits
symmetric transfer to avoid growing the stack; it does not require the
compiler to actually implement the resumption as a tail call, and this
project's baseline compilers do not agree on when they do:

- **Clang** turns the resumption into a tail call at every optimization level
  measured, including `-O0`, with and without the sanitizers. A chain of any
  depth is safe.
- **GCC** turns it into a tail call from `-O2` upward only. **At `-O0` and
  `-O1` it does not**, and each layer of the chain then consumes real,
  measured stack: 64 bytes per frame at `-O0`, 48 bytes at `-O1`, growing
  **linearly** with chain depth until it overflows. This is exactly the
  configuration of this project's own `debug` and `debug-asan` presets, both
  of which build at `-O0`.
- **AddressSanitizer makes things worse, through two separate, independently
  measured mechanisms.** At `-O2`, it defeats the tail-call elision GCC would
  otherwise perform, so per-frame usage jumps from constant to roughly 700
  bytes and again grows linearly with depth. At `-O0`, GCC was already not
  eliding, and ASan additionally enlarges each frame, from 64 bytes to about
  208. Both effects are real; neither one alone is the whole story.

This was measured, not assumed — a 200,000-frame chain segfaults on plain
GCC at `-O0` and `-O1` with no sanitizer involved, and survives at `-O2` and
on Clang at every level tested. See this project's design spec for the
coroutine components for the exact configurations. What it means for a
caller: a deep chain is safe on Clang at any optimization level, and on GCC
from `-O2` upward. On GCC at `-O0` or `-O1` — which includes this project's
own debug presets — stack use grows with chain depth regardless of whether a
sanitizer is present, and a chain beyond roughly a hundred thousand frames
will overflow the default stack even with no sanitizer at all. The project's
own 50,000-frame test happens to pass at plain `-O0` only because that depth
fits comfortably under a typical 8 MB stack, not because the tail call is
being elided.

The project's own test for this (`Task.DeepChainRunsInConstantStack`) is
skipped under GCC when `__SANITIZE_ADDRESS__` is defined, which is one
measured failure mode, and also in any unoptimized GCC build (`-O0` or
without `-O`), which is the other. Both are configurations where the
property genuinely does not hold.

Those two skips together mean it never ran on GCC in any `Debug` preset, so
the property was asserted nowhere on that compiler. The `release-tests` preset
exists for it: an optimized build that still builds the test suite, where the
tail call is present and the test actually measures something. On Clang it
runs everywhere.

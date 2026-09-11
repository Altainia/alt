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
happens with a temporary such as `square(a)` above) moves the result out.

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

- **Clang** turns the resumption into a tail call even at `-O0`. A chain of
  any depth is safe.
- **GCC** only does so under optimization. At `-O0`, each layer of a chain
  consumes real stack space, so a chain's stack usage grows **linearly** with
  its depth. A chain tens of thousands of frames deep still fits under a
  plain `-O0` build's default stack limit, but the same chain overflows once
  AddressSanitizer is enabled, because ASan enlarges every stack frame.
  Any optimized GCC build (`-O1` and above) is fine.

This was measured, not assumed — see this project's design spec for the
coroutine components for the exact configurations tested — and it is a
toolchain property the library cannot work around. What it means for a
caller: build and test deeply chained tasks in an optimized configuration if
you need the depth to be unbounded. A debug build on GCC can overflow on a
chain that is merely thousands of frames deep, well short of what an
optimized build handles comfortably. The project's own test for this
(`Task.DeepChainRunsInConstantStack`) is skipped under GCC for this reason.

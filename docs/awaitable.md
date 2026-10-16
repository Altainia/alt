# awaitable

**Header:** `<alt/awaitable.hpp>`

The vocabulary the rest of the coroutine components are built on: concepts for
recognizing an awaitable type, a trait for its result, a helper that resolves
`operator co_await`, and an adaptor that turns a thrown exception into a
`std::expected`.

```cpp
#include <alt/awaitable.hpp>
```

## Concepts

| Name | Satisfied by |
|------|--------------|
| `alt::awaiter<T>` | A type with `await_ready`, `await_suspend`, and `await_resume` |
| `alt::awaitable<T>` | A type that is an `awaiter`, or that produces one via a member or free `operator co_await` |
| `alt::awaitable_of<T, R>` | An `awaitable<T>` whose result is exactly `R` |

```cpp
static_assert(alt::awaitable<alt::task<int>>);
static_assert(alt::awaitable_of<alt::task<int>, int&&>);
```

`await_result_t` gives the result type directly:

```cpp
template<awaitable A>
using await_result_t = /* the type produced by co_await-ing A */;
```

## `get_awaiter`

```cpp
template<typename A>
constexpr decltype(auto) get_awaiter(A&& a);
```

Applies a member `operator co_await`, then a free one, and otherwise returns
`a` unchanged on the assumption that it is already an awaiter. This is the
same resolution the compiler performs at a `co_await` expression, exposed so
other code — `alt::awaitable_of`, `alt::as_expected`, the coroutine
components below — does not have to repeat it. The result may be a reference
into the argument, so the argument must outlive it.

## `as_expected`

```cpp
template<awaitable A>
[[nodiscard]] auto as_expected(A&& a);
```

Wraps an awaitable so that awaiting the result yields
`std::expected<await_result_t<A>, std::exception_ptr>` (or
`std::expected<void, std::exception_ptr>` when the result is `void`) instead of
throwing. A reference result is decayed to a value, since the expected has to
store it.

```cpp
#include <alt/awaitable.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>

alt::task<int> divide(int a, int b)
{
    if (b == 0)
        throw std::runtime_error{"division by zero"};
    co_return a / b;
}

const auto result = alt::sync_wait(alt::as_expected(divide(10, 0)));
if (result)
    use(*result);
else
    report(result.error());  // a std::exception_ptr
```

Only what `await_resume` throws is captured, because that is where
`alt::task` reports the outcome of its coroutine body. An exception thrown by
`await_suspend` — the awaited operation failing to start, rather than
failing once running — still propagates out of the `co_await` expression
unchanged.

## Limitations

These concepts describe **context-free** awaitables only: whether `co_await`
compiles for a given expression can also depend on the promise type of the
coroutine doing the awaiting, through that promise's `await_transform`. A
promise can make an expression awaitable inside itself and nowhere else, and
no concept can observe that — `awaitable<T>` can only tell you whether `T` is
awaitable on its own terms, outside any particular coroutine.

This is not a theoretical gap: `alt::task`'s promise type defines
`await_transform` in order to inject its cancellation check ahead of every
suspension point. Something that satisfies `alt::awaitable` may still behave
differently, or fail to compile, when awaited inside an `alt::task` versus
inside a plain coroutine with no `await_transform` — and something that is
*not* `alt::awaitable` on its own can still be perfectly awaitable inside a
coroutine whose promise transforms it into something that is.

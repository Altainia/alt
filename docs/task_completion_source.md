# task_completion_source

**Header:** `<alt/task_completion_source.hpp>`

The bridge from a callback-based API to a `co_await`. A foreign thread calls
`set_value(...)` or `set_exception(...)`, and the coroutine awaiting the source is
resumed with that result. It is set once and awaited once, and it is not movable,
because the foreign callback holds a pointer to it.

```cpp
#include <alt/task_completion_source.hpp>
#include <alt/task.hpp>

alt::task<void> show_the_reply(client& c)
{
    alt::task_completion_source<reply> source;

    // The callback API knows nothing about coroutines; it just calls us back.
    c.send_request(request{}, [&source](reply r) { source.set_value(std::move(r)); });

    const auto result = co_await source;   // alt::completion_result<reply>

    if(!result)
    {
        co_return;      // released without being set; see the hazards
    }

    render(*result);
}
```

The source is kept alive by the coroutine here, which is only correct because the
coroutine does not return until the await finishes. If the callback may fire after
the coroutine has gone, the source belongs somewhere that outlives both.

## Interface

```cpp
namespace alt
{
    template<typename T>
    using completion_result = std::expected<typename detail::completion_value<T>::type, std::error_code>;

    template<typename T = void>
    class task_completion_source
    {
    public:
        using value_type = T;

        task_completion_source() noexcept = default;
        ~task_completion_source();

        task_completion_source(const task_completion_source&)            = delete;
        task_completion_source& operator=(const task_completion_source&) = delete;
        task_completion_source(task_completion_source&&)                 = delete;
        task_completion_source& operator=(task_completion_source&&)      = delete;

        // Shown simplified: the real overloads are constrained, so exactly one
        // is viable for a given T, and U is never named by the caller.
        template<typename U> void set_value(U&& value);   // T a value type
        void set_value();                                 // T = void
        void set_value(T& value);                         // T = U&
        void set_exception(std::exception_ptr error);

        [[nodiscard]] bool is_set() const noexcept;

        class completion_awaiter;   // what operator co_await returns

        [[nodiscard]] completion_awaiter operator co_await() noexcept;
    };
}
```

- `co_await source` yields `alt::completion_result<T>`: the value when the source
  was set, and `std::errc::operation_canceled` when the wait ended without that
  (the awaiting chain was cancelled, or the source was destroyed with the waiter
  still parked). It costs no suspension at all if the source is already set.
  For `T = void` it is exactly [`alt::wait_result`](async_event.md#the-result-of-a-wait).
- `set_value(...)` fills the source and resumes the waiter. For a value type the
  value is moved into the source and then moved out to the waiter, so a move-only
  `T` works. `T`'s move constructor must not throw: the move happens inside the
  waiter queue's critical section.
- `set_exception(...)` fills the source with an exception, which the waiter's
  `co_await` rethrows.
- `is_set()` is `true` once either was called.

## Set once, awaited once

Setting twice, and awaiting twice, are preconditions rather than supported
operations. Each is an assertion in a debug build. A second set is then a no-op in a
release build; a second await would read a value the first already moved out, so
there is nothing safe to fall back to. By inspection of the code, a release
build does one of two things with it. Made while the source is still unset, the
second waiter parks behind the first, the setter releases only one waiter, and
only the source's destructor releases the other, unsatisfied. Made after the
set, it does not park and reads the moved-out value.

Waiters are not queued: the source has one consumer, so a second parked waiter is
not reachable through the documented interface. For several consumers of one
result use [`alt::async_event`](async_event.md) and read the value from elsewhere.

## The thread the waiter resumes on

`set_value` and `set_exception` resume the waiter on the calling thread, inside the
call, and run its body there up to its next suspension point. Do not call them with
a lock held that the waiter's body might take. A waiter that must be somewhere
specific hops explicitly with [`alt::resume_on`](scheduler.md#resume_on).

The waiter may destroy the source from its body once it has read the value; the
setter does nothing with the source after the resumption.

## The exception channel is deliberately not the error channel

An exception the caller supplied through `set_exception` **rethrows from the
await**, like any exception a callee throws. The condition alt itself reports,
`std::errc::operation_canceled`, **travels in the returned `std::expected`**. So:

```cpp
try
{
    const auto result = co_await source;

    if(!result) { /* released: cancelled, or the source was destroyed */ }
}
catch(const std::exception& e)
{
    // what the callback passed to set_exception
}
```

That is a second convention alongside
[`alt::as_expected`](awaitable.md#as_expected)'s `std::expected<T, std::exception_ptr>`,
and **`as_expected` is not meant to be composed over this type.** The alternative,
one channel for both, would force an `exception_ptr` error type across every
primitive in the phase, or swallow the caller's exceptions into error codes, and the
second is worse than a documented seam.

## A reference source yields a `std::reference_wrapper`

`task_completion_source<T&>` is settable with a `T&` and yields
`std::expected<std::reference_wrapper<T>, std::error_code>`, because
`std::expected<T&, E>` is ill-formed in C++23. The wrapper converts to `T&`, and
`*result` binds to the caller's object rather than a copy:

```cpp
alt::task_completion_source<config&> source;
// ... callback: source.set_value(the_config);

const auto result = co_await source;
config& cfg = *result;          // the callback's object
```

The object must outlive the await.

## Hazards

### A source never set, never cancelled and never destroyed parks its waiter forever

Nothing times out. A waiter on a source whose callback never fires stays suspended
and its frame is never freed. There are exactly three ways out of the wait:

- the source is set (`set_value` or `set_exception`),
- the awaiting chain is cancelled, which releases the waiter with
  `operation_canceled` (only inside a chain rooted at an
  [`alt::cancellable_task`](cancellable_task.md)), or
- the source is destroyed, which releases the waiter with `operation_canceled` and
  runs its body on the destroying thread.

A callback API that can fail without calling back is the case to guard: complete the
source from the failure path too.

### A released wait did not see the source set

`operation_canceled` means the thing awaited never happened. The released body
resumes, with the error in hand, on the thread that observed the stop or destroyed
the source, so code between the `co_await` and the next suspension point really does
run: check the result rather than assuming it.

### Destroying a source while another thread is setting it is undefined

Destruction is the one operation that is not thread-safe, here as on the rest of
this family. Destroying a source while a callback thread is inside `set_value`, or
while a cancellation is in flight against its waiter, is undefined behavior. The
owner has to know nobody is still using it.

### Ignoring the result is diagnosed on Clang only

The await is `[[nodiscard]]`, with the same enforcement limits
[`alt::async_event`](async_event.md#ignoring-the-result-is-diagnosed-on-clang-only)
documents. Unlike a mutex there is no guard type to make the bad state
unrepresentable, so check the result.

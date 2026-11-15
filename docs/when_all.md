# when_all

**Header:** `<alt/when_all.hpp>`

Awaits several awaitables concurrently and yields every result. Each awaitable runs
inside its own [`alt::cancellable_task`](cancellable_task.md), so they all make
progress at once, and the await completes when the last of them has finished.

```cpp
#include <alt/when_all.hpp>
#include <alt/task.hpp>

alt::task<void> show_the_page(client& c)
{
    const auto results = co_await alt::when_all(c.fetch_header(), c.fetch_body());

    if(!results)
    {
        co_return;      // the chain was cancelled; see below
    }

    const auto& [header, body] = *results;
    render(header, body);
}
```

## Interface

```cpp
template<typename... Awaitables> requires (awaitable<Awaitables> && ...)
[[nodiscard]] auto when_all(Awaitables&&... awaitables);

template<std::ranges::sized_range R>
  requires (!awaitable<R>) && awaitable<std::ranges::range_value_t<R>>
        && std::constructible_from<std::ranges::range_value_t<R>, std::ranges::range_rvalue_reference_t<R>>
[[nodiscard]] auto when_all(R&& range);
```

- The first form takes the awaitables as arguments and yields a `std::tuple`.
- The second takes a range of awaitables and yields a `std::vector`. The range must
  be a `std::ranges::sized_range`, because the child count is needed before the
  first child starts. Infinite and streaming ranges are excluded by that. A type
  that is both an awaitable and a sized range is one child, and the first form
  takes it.
- Neither starts anything until awaited. Each is `[[nodiscard]]`.

## The result type

Awaiting either form yields a `std::expected<Results, std::error_code>`, where
`Results` is a `std::tuple` (first form) or a `std::vector` (range form) of one
value per child, **in argument or range order, not completion order**. What a child
contributes:

| A child that yields | Contributes |
|---|---|
| `T` | `std::remove_cvref_t<T>` |
| `void` | `std::monostate` |
| `T&` | `std::reference_wrapper<T>` |

The wrapper for `T&` exists because `std::expected<T&, E>` is ill-formed in
libstdc++, so a reference cannot sit in the result directly. The `void` case keeps a
position, and in the range form it makes the vector's element type uniform.

```cpp
const auto results = co_await alt::when_all(compute(), log_it(), lookup());
// std::expected<std::tuple<int, std::monostate, std::reference_wrapper<record>>,
//               std::error_code>
```

Test it the way [`alt::wait_result`](async_event.md#the-result-of-a-wait) is tested:

```cpp
if(!results && results.error() == std::errc::operation_canceled)
{
    // the awaiting chain was cancelled
}
```

An empty argument list or an empty range is not a cancellation: it yields an empty
tuple or an empty vector.

## The range form

```cpp
std::vector<alt::task<int>> jobs = make_jobs();

const auto results = co_await alt::when_all(std::move(jobs));
// std::expected<std::vector<int>, std::error_code>
```

Every element is **moved into its child**, so the child owns it, and the value is
moved out of the child's frame before the frame dies. A consequence is that this form
has no lifetime precondition on the range's elements. An rvalue range is owned by the
returned awaitable. An lvalue range is bound, not copied, and is left holding
moved-from elements, so it must not be awaited twice. A const range is accepted only
if its elements are copyable, and they are then copied. The range is iterated once.

## Cancellation

Cancellation is an observable result, not a thrown exception. When the awaiting
chain is cancelled, the await yields `std::errc::operation_canceled`, and every
child is asked to stop.

The reason it has to be in the result at all is that the awaiting body keeps
running. The check that guards a suspension point runs ahead of it, and
`cancellation_checked::await_resume` is a bare delegation, so nothing checks again on
the way out. The resumed body runs on with whatever it does next, and it would
otherwise read results that were never produced. Test the result, and expect the
body to be torn down at its next suspension point, as for any cancelled coroutine.

### Each child is its own cancellation root

The children observe one stop source that the combinator owns, not the awaiting
chain's token. Nothing cancels a single child on its own: a stop is all or nothing.
Three invariants make that safe:

- **(A)** The combinator never owns or destroys a child frame. It only ever
  requests stops, and each child is a self-owning `cancellable_task`.
- **(B)** The claim election cannot free an already-claimed node. A loser whose node
  a satisfier already won loses the compare-and-swap in `waiter_list::claim`, so its
  `waiter_stop_release` resumes and frees nothing.
- **(C)** The combinator never destroys a child. It may resume the awaiting chain
  while children are still alive, and (A) is what makes that safe for the sibling
  hazard.

The argument is recorded where the hazard lives, in the note on
[`alt::async_latch`](async_latch.md) in `include/alt/async_latch.hpp`. Nesting one
`when_all` inside a child is the case that makes the root property observable, and
`tests/test_when_all.cpp` guards it with a nested case.

### A child's exception

A child's exception is rethrown at the await, **lowest position first**, and the
other children's results are then discarded. It is the lowest position, not the first
to throw. Every child still runs to completion first: the await does not return early
on the first failure.

## Home scheduler and teardown

The children inherit the awaiting chain's `home` scheduler
([`alt::start_options`](cancellable_task.md)), so a child holding thread-affine state
is destroyed on the thread `home` promises. The children's frames can outlive the
chain, which may be gone long before `home` is drained, and the shared block is what
makes that safe. It is reference counted, held by every child and by the awaiter, and
freed by whichever party drops the last reference. It also holds the result slots
of the argument-list form.

What a `when_all` allocates, as a list of what allocates rather than a total. A
total would be a claim about the standard library's internals: whether a
`std::stop_source` allocates at all is up to the implementation, so add up the
entries for the one you build against.

- **The shared block**, one `make_unique`, on every form. The argument-list form's
  slot tuple sits inline in it and costs nothing beyond it.
- **The stop state of the block's `std::stop_source`**, the one the children
  observe. A default-constructed `std::stop_source` allocates its stop state on
  libstdc++; an implementation that stored it inline would not.
- **A vector of slots and the result vector the await yields**, range form only.
  Both are sized from the range, so an empty range allocates neither.
- **One coroutine frame per child, and that child's own stop state.** Each child is
  a `cancellable_task` root, and its promise holds a `std::stop_source` of its own:
  the block's token is forwarded into it rather than observed directly, so the child's
  stop state is a second one alongside the block's.

So no form is allocation-free: an empty argument list and an empty range both start
no children and still allocate the block and the block's stop state.

## Preconditions

- **An awaitable passed as an lvalue, and everything it refers to, must outlive the
  children.** With a `home` scheduler that means outliving its drain, since the
  children's frames stay alive until that scheduler runs their teardown. An rvalue
  awaitable is moved into its child's frame, which owns the *object*; what the
  object refers to is not owned, and still has to outlive the children. In the
  opening example `c` must outlive them, and a lambda that captures by reference
  loses its referent if the referent does not. Copying would not make the lvalue
  case safe, because `alt::task` is not copyable.
- **Passing the same awaitable object twice is a precondition violation.** Two
  children would await one object concurrently, which no awaitable in the library
  supports. Nothing checks for it. Moving one named awaitable into two positions
  is no better: it compiles, and the second child gets a moved-from object.

## Hazards

### A child that fails to start leaks the shared block

If starting child `i` throws, which for a coroutine frame allocation failing is the
realistic case, the exception propagates out of the await and nothing releases the
reference that child was given. The block, its result slots and the children's stop
source are never freed. Children `0` to `i-1` are already running, and they run to
completion unobserved with their results discarded. There is no hang, because the
chain never parks. It is the same no-rollback contract as
[`task_group::spawn`](task_group.md), so budget one leak per failed start.

### Resources held by a result that nobody reads

Results produced by children after the chain was cancelled are discarded with the
slots, never delivered. A child that yields something that owns a resource therefore
releases it by destruction and nothing else. This is the stage A form of a wider
question, which is taken up with `when_any`, where the losers' results are discarded
on every successful call and not only on a cancellation. That note is deferred to
that component.

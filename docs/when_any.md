# when_any

**Header:** `<alt/when_any.hpp>`

Races several awaitables and yields the first to complete, with its position. Each
awaitable runs inside its own [`alt::cancellable_task`](cancellable_task.md), so they
all make progress at once; the first to finish stops the rest, and the await returns
once every child has retired.

```cpp
#include <alt/when_any.hpp>
#include <alt/task.hpp>

alt::task<void> show_the_page(client& c, timer& t)
{
    // Not const: taking the winner's value out below moves out of it.
    auto result = co_await alt::when_any(c.fetch_body(), t.after(5s));

    if(!result)
    {
        co_return;      // the chain was cancelled; see below
    }

    if(result->index() == 0)
    {
        render(std::move(*result).get<0>());
    }
    else
    {
        report_timeout();
    }
}
```

That shape — racing work against a deadline — is what the component exists for. This
library supplies no timer; `when_any` is what makes a timeout *expressible* once you
have one.

## Interface

```cpp
template<typename... Awaitables>
  requires (awaitable<Awaitables> && ...) && (sizeof...(Awaitables) > 0)
[[nodiscard]] auto when_any(Awaitables&&... awaitables);

template<std::ranges::sized_range R>
  requires (!awaitable<R>) && awaitable<std::ranges::range_value_t<R>>
        && std::constructible_from<std::ranges::range_value_t<R>, std::ranges::range_rvalue_reference_t<R>>
[[nodiscard]] auto when_any(R&& range);
```

- The first form takes the awaitables as arguments and yields an `alt::when_any_result`
  over their value types.
- The second takes a range of awaitables and yields an `alt::when_any_range_result` over
  the one element value type. The range must be a `std::ranges::sized_range`, because the
  child count is needed before the first child starts. A type that is both an awaitable
  and a sized range is one child, and the first form takes it.
- Neither starts anything until awaited. Each is `[[nodiscard]]`.
- **A race needs at least one entrant**, and the two forms say so differently: the
  argument-list form rejects zero awaitables at **compile time**, through its constraint,
  so the diagnostic lands on the caller's line. An empty range cannot be caught that way
  — a range's size is not part of its type — so it is a **run-time** error and the await
  yields `std::errc::invalid_argument`. See [The empty cases](#the-empty-cases).

## The result type

Awaiting either form yields a `std::expected<Result, std::error_code>`. What a child
contributes to it is the mapping [`when_all`](when_all.md#the-result-type) uses,
unchanged:

| A child that yields | Contributes |
|---|---|
| `T` | `std::remove_cvref_t<T>` |
| `void` | `std::monostate` |
| `T&` | `std::reference_wrapper<T>` |

The argument-list form's `Result` is `alt::when_any_result<V...>`, one position per
argument:

```cpp
const auto result = co_await alt::when_any(compute(), log_it());
// std::expected<alt::when_any_result<int, std::monostate>, std::error_code>

result->index();            // 0 or 1: the argument that completed first
result->get<0>();           // where index() is 0: the winner's value, as a const int&
```

`index()` is the winning position and `get<I>()` its value. Asking for a position that
did not win is a caller error: it asserts in a debug build and throws
`std::bad_variant_access` otherwise. **Take a winner out with
`std::move(*result).get<I>()`**, which is the only form that can move a move-only value
out — the `const` lvalue `get<I>()` yields a `const T&`, so that spelling needs the
result itself to be a non-`const` `auto`. In a template it reads
`std::move(*result).template get<I>()`.

The range form's `Result` is `alt::when_any_range_result<V>`, with `index()` the winner's
position in the range and `value()` its value, by `const&` or, on an rvalue, moved out.
It is a class of its own rather than a `when_any_result` over one type, whose `index()`
would be fixed at 0 by construction.

Test the error half the way [`alt::wait_result`](async_event.md#the-result-of-a-wait) is
tested:

```cpp
if(!result && result.error() == std::errc::operation_canceled)
{
    // the awaiting chain was cancelled
}
```

## The range form

```cpp
std::vector<alt::task<int>> jobs = make_jobs();

const auto result = co_await alt::when_any(std::move(jobs));
// std::expected<alt::when_any_range_result<int>, std::error_code>

const std::size_t which = result->index();      // an index into the range
const int         value = result->value();
```

Every element is **moved into its child**, so the child owns it, and the value is moved
out of the child's frame before the frame dies. A consequence is that this form has no
lifetime precondition on the range's elements. An rvalue range is owned by the returned
awaitable. An lvalue range is bound, not copied, and is left holding moved-from
elements, so it must not be awaited twice. A const range is accepted only if its elements
are copyable, and they are then copied. The range is iterated once.

## The empty cases

A race with no entrants can never complete, and parking forever is the worst available
answer. So neither form parks:

- `alt::when_any()` **does not compile.** The constraint rejects it at the call site, and
  an argument-list race over no awaitables would have no position to assemble from
  besides.
- An **empty range** yields `std::errc::invalid_argument`, without suspending and without
  allocating anything.

Note that `when_all()` over zero children completing at once with an empty tuple is
correct and is not the same shape.

## What a loser loses

A race has one winner, and **two ways a loser's result is discarded**, with a third
kind of loss behind them. None is a bug; all are worth knowing before putting work into
a race.

- **A completed loser's value is discarded.** Every child that completes writes its own
  result slot, because a child cannot know it has won until it has elected, and only the
  elected slot is ever read. Two plain tasks that both simply finish are the common
  case of this, not an exotic one.
- **A loser's exception is discarded**, and this is an **asymmetry with
  [`when_all`](when_all.md)**, which rethrows the lowest-position child's exception. A
  `when_any` rethrows only the *winner's*. A loser that completes by throwing has its
  exception stored in a slot nothing reads, and nothing anywhere reports it.
- **A loser stopped before it completed loses whatever its work had already consumed**,
  which is the resource-loss mode `when_all` documents on its cancellation path: a
  semaphore permit taken and never returned, an
  [`alt::task_completion_source`](task_completion_source.md) value set and never read. An
  [`alt::async_mutex`](async_mutex.md) loser is harmless, since the guard's destructor
  unlocks.

A loser that is still parked on an `alt` wait primitive when the winner's stop arrives is
*released* rather than torn down — that is what those primitives do with a parked waiter —
so its body runs on and produces the value or exception that is then discarded. Two kinds
of loser produce nothing at all instead: one whose awaitable is a nested `alt::task`, which
is destroyed at that task's final suspend once a stop has been requested, and one parked on
a scheduler through [`alt::resume_on`](scheduler.md), which is abandoned when the
scheduler comes to resume it. And a loser parked on something the stop cannot reach — an
awaitable of your own that registers no stop callback — is not resumed by the stop at all:
it stays outstanding until its own mechanism resumes it, and what happens then is decided
by how it was awaited: awaited directly it produces its value or exception, as a released
loser does, because nothing re-checks the stop on the way out of a wait; awaited through a
nested `alt::task` it is destroyed at that task's final suspend and produces nothing.
Every one of them retires, whichever way it goes, and a won await waits for all of them.

## The winner's exception

A child that completes by **throwing is a completed child and wins**. Its exception is
rethrown at the await — and rethrown *after* the losers have retired, exactly as a
winning value is held back until then:

```cpp
try
{
    const auto result = co_await alt::when_any(may_throw(), other());
    // ...
}
catch(const std::runtime_error& error)
{
    // the winner threw; every child has already retired
}
```

A **cancelled** race rethrows nothing, winner's exception included: the cancellation is
the only thing the caller is told.

## Cancellation and teardown

Cancellation is an observable result, not a thrown exception. When the awaiting chain is
cancelled the await yields `std::errc::operation_canceled`, and every child is asked to
stop. The reason it has to be in the result at all is
[`when_all`](when_all.md#cancellation)'s, unchanged: the awaiting body keeps running, and
nothing checks the stop again on the way out of the await.

**A cancellation that arrives after a winner was elected still reports
`operation_canceled`, and the winner is discarded.** The window is real rather than
theoretical, because the chain stays parked while the losers unwind, so there is a
stretch of time in which the race is decided and the await has not returned. A caller
that cancelled a race is told the race was cancelled rather than handed a result it may
no longer have anywhere to put.

**A won await returns only once every child has retired.** The awaiting chain parks on
the completion counter, and the losers retire into it like the winner, so no child of
the caller's is running when a satisfied await comes back. That is what makes the lvalue
precondition below checkable **on a won await** — there, and only there, referents that
outlive the *await* have outlived the children too, which a caller can observe rather
than a teardown it cannot. It costs latency: the winner's value is held back until the
losers have finished unwinding.

An await **the cancellation releases** deliberately does not wait. It resumes at once,
with children still unwinding, exactly as a cancelled [`when_all`](when_all.md) does,
because a window closing while one of its coroutines still runs work on a worker thread
is the requirement the whole cancellation design exists for. That is why the precondition
is phrased against *the children* and not against the await.

**A satisfied race that elected no winner also reports `operation_canceled`.** Every
child retired, none of them by completing, and no cancellation was requested: there is no
winning slot to read, so the await reports the same released error a cancelled race
reports rather than reading a slot no child filled.

No route to that state is known, and none is claimed. Nothing in the test suite reaches
it and no mechanism in the library is known to produce it. The report exists because the
alternative in a release build is worse than an error: an out-of-range slot read in the
range form, and in the argument-list form a `std::bad_variant_access` thrown out of an
await whose documented error channel holds only the two errors above. Treat it as a
defensive return rather than as a behavior to design against.

### Each child is its own cancellation root

The children observe one stop source the combinator owns, not the awaiting chain's
token. Nothing cancels a single child on its own: a stop is all or nothing, and the
winner's stop and a cancellation are separate operations — only the second marks the
wait cancelled. The three invariants that make this safe are `when_all`'s, stated under
the same labels in [its documentation](when_all.md#each-child-is-its-own-cancellation-root)
and in the note on [`alt::async_latch`](async_latch.md), where the hazard they are about
lives. `when_any` fits them unchanged, and the `async_latch` note says why: it carries a
copy of `when_all`'s completion hook, parks one joiner on the same completion counter and
keeps the same park-until-zero discipline, so invariant (C)'s overlap is the same in
both — one child whose frame is being destroyed, still holding a block reference across a
satisfied resumption, and that child may be a loser the winner's stop tore down rather
than one that finished its body; and many children still unwinding on a cancellation. The
`async_latch` note carries the derivation behind that.

A `when_any` awaited in a promise with **no cancellation context** — an `alt::task`
driven by `alt::sync_wait`, with no `cancellable_task` root above it — is simply
uncancellable. There is no token to link, so nothing can stop the children, and the
await can only report the winner. That is not special to `when_any`: it is what
"cancellation is relayed from a root" means for every awaitable in the library.

## Home scheduler and allocations

The children inherit the awaiting chain's `home` scheduler
([`alt::start_options`](cancellable_task.md)), so a child holding thread-affine state is
destroyed on the thread `home` promises. The children's frames can outlive the chain,
and the reference-counted shared block is what makes that safe: it is held by every
child and by the awaiter, and freed by whichever party drops the last reference.

What a `when_any` allocates is `when_all`'s list plus one thing that costs nothing: the
block also holds the winner election, inline. See
[`when_all`](when_all.md#home-scheduler-and-teardown) for the list and for why it is a
list of what allocates rather than a total.

## Preconditions

- **An awaitable passed as an lvalue, and everything it refers to, must outlive the
  children.** With a `home` scheduler that means outliving its drain. An rvalue
  awaitable is moved into its child's frame, which owns the *object*; what the object
  refers to is not owned, and still has to outlive the children. The range form has no
  such precondition, its elements being moved into their children.
- **Passing the same awaitable object twice is a precondition violation.** Two children
  would await one object concurrently, which no awaitable in the library supports.
  Nothing checks for it.
- **Reading a result position other than `index()`** asserts in a debug build and throws
  `std::bad_variant_access` otherwise.

## Hazards

### A child that fails to start leaks the shared block

If starting child `i` throws, which for a coroutine frame allocation failing is the
realistic case, the exception propagates out of the await and nothing releases the
reference that child was given. The block, its result slots, its election and the
children's stop source are never freed. Children `0` to `i-1` are already running, and
they run to completion unobserved with their results discarded. There is no hang,
because the chain never parks. It is `when_all`'s hazard and
[`task_group::spawn`](task_group.md)'s no-rollback contract, so budget one leak per
failed start.

### One joiner, and what a second would re-open

Exactly one party parks on a race's completion counter: the awaiting chain. That is a
property of the design rather than an accident, and the liveness assert in
`include/alt/detail/waiter_list.hpp` depends on it — the assert sits *ahead* of that
list's claim election, so that even a claim which would lose aborts on a destroyed list
instead of returning quietly.

A change that parked a **second** joiner on one race's counter would make a late losing
claim on a freed list an ordinary shape during teardown, and that assert would then fire
on a benign race, in every Debug, ASan, TSan and Coverage build. Such a change re-opens
where the assert belongs, which is a decision about `waiter_list` rather than about this
component, and it should be made deliberately rather than discovered from a red build.
The enumeration behind this is in `detail::when_any_awaiter`'s note in
`include/alt/when_any.hpp`.

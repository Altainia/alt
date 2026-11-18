# cancellation_ref

**Header:** `<alt/cancellation_ref.hpp>`

A pointer-sized, non-owning view of one await chain's cancellation state. It is
the extension point through which an awaiter you wrote yourself joins the
resumptions [`alt::cancellable_task`](cancellable_task.md)'s machinery already
intercepts, instead of being checked on entry to the suspension point only.

```cpp
#include <alt/cancellation_ref.hpp>
```

Nothing else in the library includes this header, and it includes only
`<alt/detail/cancellation.hpp>` and what it needs from the standard library. An
awaiter's own header includes it; `<alt/task.hpp>` and
`<alt/cancellable_task.hpp>` do not pull it in.

## What problem this solves

Cancellation in a chain rooted at an `alt::cancellable_task` is observed on entry
to every suspension point, and at three resumptions the library owns:
`alt::resume_on` returning from a scheduler, a child `alt::task` transferring
back to its parent, and the stop callback a wait on one of the six async
primitives registers. Intercepting a resumption means knowing how that
resumption happens, so the library can only do it for resumptions it performs
itself.

An awaiter that hands the frame to a thread, a timer or a callback API of its own
resumes that frame itself, and the library sees nothing. Without an opt-in, such
an awaiter leaves exactly the window
[`alt::cancellable_task`](cancellable_task.md) otherwise closes: a stop requested
while the operation was in flight is not noticed until the body reaches its next
`co_await`, so the body runs in between. The documented mitigation for an
awaitable you do not control is to put a `co_await alt::cancellation_point{}`
straight after the resume, which still runs the statements up to it.

An awaiter whose author *does* control it can do better. `cancellation_ref` gives
it the two things it needs and nothing more: the chain's stop token, and a way to
tear the chain down in place of resuming it.

## Interface

```cpp
class cancellation_ref
{
public:
    cancellation_ref() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] std::stop_token get_stop_token() const noexcept;
    [[nodiscard]] bool            stop_requested() const noexcept;

    void abandon() const noexcept;
    bool resume_or_abandon(std::coroutine_handle<> h) const noexcept;
};

template<typename Promise>
[[nodiscard]] cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise> h) noexcept;
```

- The view is trivially copyable and the size of a pointer. An awaiter stores one
  in its own storage — which is the coroutine frame — between `await_suspend` and
  the operation completing, because that is the only place the token and the
  teardown are both to hand at completion time.
- `operator bool` is `true` when the view names a chain that carries cancellation
  state. It names no chain at all when it is empty; see below.
- `get_stop_token()` is the chain's `std::stop_token`, which an awaiter can pass
  to its operation or register a `std::stop_callback` on. An empty view yields a
  default-constructed token, for which `stop_possible()` is `false`, so a caller
  may hand it onward unconditionally — a `std::stop_callback` on such a token
  simply never fires.
- `stop_requested()` is `true` once a stop has been requested on the chain, and
  always `false` for an empty view.
- `abandon()` destroys the root frame and with it every frame below it, or posts
  that destruction to the chain's `start_options::home`. It has two
  preconditions, each carrying its own assert in
  `include/alt/cancellation_ref.hpp`: the view is not empty
  (`"abandoning an empty alt::cancellation_ref"`) and a stop has already been
  requested on the chain (`"abandoning an alt chain with no stop requested"`).
  Those asserts are compiled out by `NDEBUG`; the preconditions are not.
- `resume_or_abandon(h)` abandons the chain if a stop has been requested and
  resumes `h` otherwise, returning `true` when it resumed. An empty view resumes.
  It is what an awaiter calls in place of `h.resume()`, and `h` must not be
  empty.
- `get_cancellation_ref(h)` is the only way to obtain a non-empty view. Its one
  precondition is that `h` is not empty, asserted in
  `include/alt/cancellation_ref.hpp` with
  `"getting a cancellation_ref from an empty coroutine handle"`.
  What it returns for an erased handle is the next section.

There is deliberately no access to the chain's root handle or its home scheduler.
A view can read the stop state and trigger the one teardown the library already
performs; it cannot redirect either.

## The empty view, and the erased-handle trap

A view is empty when there is no chain to observe. That happens when it was
default constructed, when the awaiting coroutine's promise carries no
cancellation state, or when it is an `alt::task` that was not awaited from a
chain rooted at an `alt::cancellable_task` — a plain `alt::task` under
[`alt::sync_wait`](sync_wait.md), for instance. Inside an
`alt::cancellable_task`, an `await_suspend` templated over the promise type never
gets an empty view.

An empty view is a working view, not an error: it reports no stop requested, its
token can never be stopped, and `resume_or_abandon` resumes. That is what lets
one awaiter serve a cancellable chain and a plain `alt::task` with no branch of
its own.

**An awaiter whose `await_suspend` takes `std::coroutine_handle<>` always gets an
empty view**, whatever chain it is suspending in. `std::coroutine_handle<>` is
`std::coroutine_handle<void>`, which has no `promise()`, so there is nothing for
the accessor to ask. It compiles, which is the right answer for a generic
awaiter, and it silently leaves the awaiter with the entry-only behavior it had
before. To receive a non-empty view, declare `await_suspend` as a template over
the promise type:

```cpp
template<typename Promise>
void await_suspend(std::coroutine_handle<Promise> h);
```

The cancellation wrapper forwards the typed handle straight through to the inner
awaiter, so nothing else is needed to get one.

## Writing an awaiter that opts in

```cpp
#include <alt/cancellation_ref.hpp>

template<typename Operation>
class my_awaiter
{
public:
    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h)
    {
        m_chain  = alt::get_cancellation_ref(h);
        m_handle = h;

        // The chain's own token, without the caller having had to pass it in.
        m_operation.start(m_chain.get_stop_token(), this);
    }

    result await_resume() { return std::move(m_result); }

private:
    // Called from whatever thread the operation completes on.
    void on_complete(result r)
    {
        m_result = std::move(r);

        // Last statement: either branch may already have destroyed this object.
        m_chain.resume_or_abandon(m_handle);
    }

    Operation               m_operation;
    alt::cancellation_ref   m_chain;
    std::coroutine_handle<> m_handle;
    result                  m_result;
};
```

Three things about that shape are load-bearing.

`resume_or_abandon` must be the awaiter's **last statement**. Both branches can
free the frame the awaiter lives in: resuming can run the coroutine to
completion, and abandoning destroys the chain. Nothing may be read through the
view, the awaiter, or any other view of the same chain afterwards. The returned
`bool` itself is fine: it is a value in the caller's own frame, so testing it
costs nothing and is always safe. It is what the caller then reaches *through*
that is not.

It must be called **at most once per suspension**, because both branches end that
suspension. An operation that can complete twice, or that completes and is also
driven to completion by a stop callback, has to resolve that race itself before
it calls here.

And `await_suspend` takes the typed handle, per the previous section. The handle
stored for the resumption can be the erased one; it is only the accessor that
needs the promise type.

### Tearing down from inside `await_suspend`

The recipe above abandons from the completion callback, which is the common case.
An awaiter that instead decides inside `await_suspend` that it cannot start —
because `stop_requested()` is already true, say — may call `abandon()` there, and
then has one extra rule to keep: **return from `await_suspend` immediately, in a
way that does not resume the coroutine.** A `void await_suspend` returns; a
`bool` one returns `true`. The frame is gone by then, so resuming it, or
returning `false` and letting the compiler resume it, is a use-after-free. For
the handle-returning form, return `std::noop_coroutine()` — which is what the
library's own wrapper does on this path; `detail::cancellation_checked::await_suspend`
in `include/alt/detail/cancellation.hpp` carries the three cases side by side.

Note that this path is rarely needed. Cancellation is already checked on entry to
every suspension point, so a chain stopped before the `co_await` is torn down
before the awaiter's `await_suspend` runs at all —
`tests/test_cancellation_ref.cpp`'s `AStopBeforeTheAwaitStopsTheOperationStarting`
pins that.

## Both forms of the await, and which constraint applies to each

`await_transform` *deduces* the awaitable, through a forwarding reference, and
the wrapper it builds stores what it deduced **by value** — the `A m_awaitable`
member of `detail::cancellation_checked_awaitable<A>`. So a prvalue await makes
`A` the awaiter's own type, and requires it to be move constructible:

```cpp
co_await my_awaiter<my_operation>{/* ... */};   // needs a move constructor
```

That is a real limit, and this component does not remove it. An awaiter that
holds something immovable cannot be awaited in that form: a `std::stop_callback`,
which is the natural thing for an adapter to register, or a `std::execution`
operation state, which is immovable by design — issue #10 cites CWG2327 for
that, and says it is not a bug in alt.

It is dodgeable without placement-new, by awaiting a **named lvalue**. Because
`await_transform` deduces rather than copies, `A` is then `my_awaiter&`, the
wrapper's member is a reference rather than a value, and nothing is moved at any
point:

```cpp
my_awaiter<my_operation> awaiter{/* ... */};   // an immovable member is fine here
co_await awaiter;                              // named lvalue: nothing is moved
```

Keeping a reference a reference is deliberate rather than incidental — the
comment on `cancellation_checked_awaitable::operator co_await` in
`include/alt/detail/cancellation.hpp` says so, and it is what makes this form
work. `tests/test_cancellation_ref.cpp`'s `AnImmovableAwaiterWorksAsAnLvalue`
awaits an awaiter holding a `std::optional<std::stop_callback<...>>`, with a
`static_assert(!std::is_move_constructible_v<...>)` on it, so the page cannot
drift from the language rule.

The lvalue must outlive the whole `co_await` expression. A local of the awaiting
coroutine does.

## `abandon()` is not a general destroy-this-chain button

`abandon()` requires that a stop has already been requested on the chain. That
precondition is the design, not defensive padding.

This component exists so that an awaiter can **observe the chain's own
cancellation** at a resumption it owns. An unconditional teardown would be a
different and larger capability, and it would be reachable from inside the
combinators: a [`when_all`](when_all.md), [`when_any`](when_any.md) or
[`task_group`](task_group.md) child is its own root, and the joiner tells a
cancelled child from a satisfied one by the block's own flag. A child that tore
itself down without the block having asked would fire the child's completion
hook, drive the joiner down its satisfied path, and have it assemble a result no
child produced. With the precondition the child's only stop source is the
block's, so that cannot happen by construction.

**So what does an awaiter do when its operation stops for a reason the chain's
token never saw** — a sender whose `set_stopped` fires on its own schedule, an
operation its own owner cancelled? It resumes with a released result, exactly as
the six async primitives do (see [`alt::async_event`](async_event.md) and
`alt::wait_result`), and lets the ordinary cascade tear the chain down at the
next suspension point. It does not call `abandon()`. The awaiting body then sees
*why* its await ended, which a teardown would have denied it.

## Re-entrant teardown is supported

The usual shape for an adapter is that the operation completes **synchronously
inside `request_stop()`**: the awaiter registered a `std::stop_callback` on the
token the view handed it, so requesting a stop drives the operation to its end on
the requesting thread, and `resume_or_abandon` then runs from inside that
callback. With no `start_options::home`, the teardown destroys the root frame —
which owns the promise's `std::stop_source`, the chain's token and the
external-link `std::stop_callback` — while `request_stop()` is still walking that
stop state's callback list, and destroys the frame that the executing callback
itself lives in.

This is supported, in this narrow form: **it is safe provided the chain's stop
state retains an owner outside the frames the teardown destroys.**

Two separate things have to hold, and they are easy to run together.

**Destroying the callbacks is allowed.** That part is **[stopcallback.cons]**: a
`std::stop_callback` destructor called from within its own callback's invocation
on the current thread returns without waiting, which covers both the leaf's
callback and the chain's external link when that is the one mid-invocation. The
clause is correct and is cited correctly here. It is about the *destructor*
returning, though — it says nothing about the stop state the interrupted
`request_stop()` is still reading.

**Keeping the stop state alive is a separate question, and the answer depends on
the shape.** Destroying the promise's `std::stop_source`, the chain's token and
the external link only drops references; whether the state survives depends on
whether anything outside the destroyed frames still holds one.

- A **retained `alt::cancellation_handle`** from `alt::start` holds a counted
  source of the chain's own stop state, in the caller's frame, which the teardown
  does not touch. That satisfies the proviso directly.
- A **linked `start_options::token` with the chain's own handle discarded** does
  not. Every owner of the *chain's* stop state then lives in the frames the
  teardown destroys: the promise's `m_source`, `m_context.token`, and the
  `detail::stop_forwarder` inside the external link. The party that called
  `request_stop()` holds a source for the **external** state, not for the chain's,
  and the caller of the chain's own nested `request_stop()` is the
  `stop_forwarder` sitting in the frame being freed.

An earlier version of this page said the proviso was satisfied automatically,
"which the caller of `request_stop()` always is". That was wrong for exactly the
second shape above, and the shape is the one this page recommends. It is measured:
with no counted local in `detail::stop_forwarder::operator()`, requesting the stop
produces an AddressSanitizer heap-use-after-free, a `READ` of the freed stop state
in `_Stop_state_t::_M_lock()` reached from that function.

That shape is nonetheless safe today, because `detail::stop_forwarder::operator()`
in `include/alt/cancellable_task.hpp` takes a counted copy of the source on its
own stack — outside the destroyed frames — before requesting the stop, so the
state survives the nested walk whatever the chain's own owners do. Its DELICATE
note carries the full reasoning and the in-repo precedent,
`detail::combinator_state::cancel()`. Adapter authors do not have to do anything
for this; it is named here only so the guarantee is not read as a property of the
standard clause alone.

**That copy is not this component's.** Commit `f3f0a8b` on `main` added it while
porting the library to MSVC, before this page existed, and the work behind this
page rediscovered the same defect independently and arrived at the same fix. Main's
is the one that shipped, and it is the better account: MSVC's `request_stop()`
relocks the callback list after *every* callback, including the last, so without the
copy every cancellation test that destroys a parked frame hangs there — which makes
the defect a plain defect of the library on every platform, not a property of this
extension point. libstdc++ skips that relock after the last callback, and that is
why this library's pre-MSVC measurements, on this work and on `main` alike,
AddressSanitizer included, all came back clean.

What this page and `tests/test_cancellation_ref.cpp` do contribute are the two
things `f3f0a8b` left out: a regression test — that commit touches
`cancellable_task.hpp`, `combinator_state.hpp`, `projection.hpp` and
`test_projection.cpp`, and pins the stop-state fix nowhere — and the corrected
proviso above, which is this work's own and replaces a guarantee this page
previously stated unconditionally.

`tests/test_cancellation_ref.cpp` exercises three shapes — a stop through the
chain's own `cancellation_handle`, a stop through a linked `start_options::token`,
and `ATeardownWithASecondCallbackOnTheChainsTokenIsSafe`, the linked-token shape
with a second `std::stop_callback` on the chain's token so that libstdc++'s
callback walk has somewhere left to go after the abandoning one has freed the
chain. That third one is the instrument for the counted copy, and the only shape in
which libstdc++ reports the defect at all: with only the abandoning callback
registered, `_M_request_stop`'s "last callback" early return hides the freed state
and the first two shapes pass with or without it. So do not read either of the first
two as evidence that the proviso holds by itself — they are consistent with it being
false, which is how the earlier wording survived. On MSVC the shape is not needed to
see the defect; this test is what makes it visible on the Linux gates.

All three were run and passed under AddressSanitizer with UBSan and under
ThreadSanitizer on GCC 15.2, and
`docs/superpowers/specs/2026-11-17-cancellation-ref-design.md` records the first
two clean on `clang++-19` as well. None of the three has been run under MSVC, now a
supported toolchain (19.50+): CI's `Build & Test (MSVC, <preset>)` jobs are the first
time this file is compiled with it, and the third shape depends on an invocation order
no standard clause fixes, so it may not discriminate anything there — issue #47. A green sanitizer run is weak evidence for this
hazard class, as `include/alt/detail/waiter_list.hpp` records at length — there, as
that class shipped and before its liveness sentinel, a freed-heap access had no
instrumented access anywhere on its path and neither tool reported it. The
instrumented access here is `_M_lock()`'s read of the stop state's own word,
inlined from `<stop_token>`, which is why this one *is* visible once the shape
reaches it at all — and why the shape, not the tool, was the thing missing. The
`f3f0a8b` history is the standing reminder that an implementation detail of one
standard library can hide a defect from every gate that runs against it.

## Lifetime

Two rules, neither of which the test suite can check for you.

**A view must not outlive its chain.** The `detail::cancellation_context` it
points at lives in the root coroutine's frame; nothing here is reference counted,
and the view extends no lifetime. A view is valid for as long as the coroutine
that obtained it stays suspended, and must not be used once that coroutine has
been resumed or destroyed. Storing one in the awaiter, which lives in the
suspended frame, satisfies this: the root frame outlives every frame below it.
Storing one anywhere that can outlive the chain does not.

The chain's `std::stop_token` is the exception, and only because the standard
library reference counts the stop state. A token copied out of
`get_stop_token()` stays valid after the frames are gone — that is what lets
`alt::cancellation_handle` request a stop on a chain that already finished — but
the view it came from does not.

**`resume_or_abandon` is called at most once per suspension**, as above. Both
branches end the suspension, so a second call is acting on a frame that has
already been resumed or freed.

## What this does not change

A foreign awaitable that does *not* opt in is still checked on entry only, and
`co_await alt::cancellation_point{}` after the resume is still the mitigation for
one you do not control. The six async primitives still release a parked waiter
and resume its body with `std::errc::operation_canceled` rather than tearing the
chain down; that is their design and this component does not alter it. And a
chain parked on an operation that never completes still never reaches a
resumption at all, so an opted-in awaiter does not rescue it — see [a task parked
on something that never resumes never
self-destructs](cancellable_task.md#a-task-parked-on-something-that-never-resumes-never-self-destructs).

`alt::cancellation_ref` is not a sender, and the library gained no knowledge of
`std::execution` with it. It is the piece an adapter cannot build for itself; the
adapter stays downstream.

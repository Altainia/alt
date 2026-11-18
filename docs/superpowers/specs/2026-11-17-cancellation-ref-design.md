# A public extension point for alt's cancellation protocol: `alt::cancellation_ref`

**Date:** 2026-11-17
**Status:** Draft, awaiting approval. Builder check returned; its four questions are resolved in place (two settled as the author's calls, two by the owner on 2026-11-17).
**Issue:** #10
**Related:** `2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1 and 2),
`2026-09-10-coroutines-roadmap.md` (the sender/receiver exclusion), issue #4 (the
resume-time check this generalizes)

## Intent and the why

Issue #10 asks for two things wrapped together: a `std::execution` sender adapter for
`alt::task`, and the piece of alt that such an adapter cannot build for itself. Only the
second is in scope here, and it is in scope because it is not about senders at all.

`docs/cancellable_task.md` states the library's guarantee as point 6: once a stop is
requested, nothing in any coroutine body runs. The machinery honours that at every
suspension point's *entry*, and at three *resumptions* the library owns:

| Resumption alt owns | Where | What it does on a stop |
|---------------------|-------|------------------------|
| `alt::resume_on` returning from a scheduler | `detail::checked_resume_on::resume_or_abandon` | abandons the chain |
| a child `alt::task` transferring back to its parent | `task_promise_base::final_awaiter::await_suspend` | abandons the chain |
| the stop callback a wait on one of the six primitives registers | `detail::waiter_stop_release` | releases the waiter and **resumes** it with `operation_canceled` |

A resumption the library does *not* own cannot join that table, because the two things
needed to join it — `detail::cancellation_context` and `detail::abandon_chain` — are
`alt::detail`. So a hand-written awaiter that resumes a frame itself is checked on entry
only, and `docs/cancellable_task.md` already says so under "Foreign awaitables are
checked on entry only", offering `co_await alt::cancellation_point{}` after the resume as
the mitigation. That mitigation is exactly the workaround issue #10 reports its prototype
being stuck with, and it is weaker than what alt's own awaiters get: the body runs
between the resume and that `cancellation_point`.

This design opens that table to callers. It adds one non-owning view of a chain's
cancellation state, obtainable from the typed promise handle a custom awaiter already
receives, through which that awaiter can read the chain's stop token and abandon the
chain in place of resuming it. A caller's awaiter becomes a fourth row. Nothing about
senders enters the library; the sender adapter stays downstream and is then buildable
without reaching into `alt::detail`.

## Goals

- A custom awaiter can obtain the stop token of the chain it is suspending in, without
  the token being passed to it by hand by the caller.
- A custom awaiter can tear that chain down instead of resuming it, honouring
  `start_options::home`, so that no body code runs after a stop — the same outcome
  `alt::resume_on` gets.
- The delicate part of doing that stays inside the library. The teardown frees the frame
  the awaiter lives in, so "copy what you need to the stack first, then touch nothing"
  is a rule the library applies on the caller's behalf rather than one documented at the
  caller and got wrong once per adapter.
- An awaiter written against this works unchanged in a chain with no cancellation at all
  (a plain `alt::task` under `alt::sync_wait`), where it resumes normally.
- The library stays dependency-free and every one of CI's eleven jobs covers the new
  code, `Coverage` included. (Eight when this spec was written; `main` has since added
  MSVC support, whose `Build & Test (MSVC, <preset>)` job runs as a three-preset matrix.
  The three MSVC jobs cannot be run on the Linux development machine — see Constraints.)
- The three places that currently state the "foreign awaitables are checked on entry
  only" claim are corrected, because this change falsifies the unconditional form of it.

## Non-goals

- **Any `std::execution` or stdexec code in alt.** Route A of issue #10 is declined:
  no toolchain in the support matrix (GCC 13+, Clang 19+, MSVC 19.50+) ships
  `std::execution`, so a sender path would be compiled by none of CI's jobs, and stdexec would be alt's
  first third-party dependency where the target links only `Threads::Threads` today. The
  roadmap's exclusion of sender/receiver alignment stands.
- **`alt::task<T>` as a sender.** The same exclusion; issue #10 raises it as an open
  question and it stays open.
- **Generic resume-time interception for every foreign awaitable** (the guard-coroutine
  idea in issue #4, approach D in the brainstorm). Rejected on three grounds: a frame
  allocation per foreign suspension; the guard's promise would have to forward
  `cancellation_context_pointer()`, because `detail::relays_cancellation<Promise>` and
  `task_awaiter::await_suspend` both read the real promise through the handle they are
  given; and it would tear down the six waits that `async_event` and its siblings
  deliberately *release*, so it needs an exclusion list for alt's own awaitables and is
  then not generic. Opt-in by the awaiter's author is the whole mechanism here.
- **Changing what the six primitives do on a stop.** They release the parked waiter with
  `std::errc::operation_canceled` and let the body observe it. That is deliberate, it is
  what `alt::wait_result` exists for, and the narrowing of point 6 it causes is already
  documented in four places. Issue #10's framing treats "resume the body with no value"
  as a defect; for those six it is the design. The extension point therefore *offers*
  teardown and does not impose it: an awaiter may resume with a released result instead,
  which is how a downstream adapter would implement a sender whose `set_stopped` means
  something other than "this chain is over".
- **A public `cancellation_context`.** The view exposes the stop token and the teardown.
  It does not expose `root` or `home`, which a caller could otherwise overwrite.
- **A public `relays_cancellation` concept.** Its requirement names
  `detail::cancellation_context*`, so publishing it would publish that type's name. The
  concept moves to `detail/cancellation.hpp` and stays internal; the documentation states
  in prose which coroutines yield a non-empty view.
- **A second extension point for the awaited value.** Issue #10's route B sketch mentions
  "a customisation that receives the awaited value". No such hook is needed: an awaiter
  already owns its own `await_resume`, and the value never has to pass through alt.

## Anti-goals

What would make this a failure even with every gate green:

- **A function that quietly does nothing useful.** `get_cancellation_ref` handed an erased
  `std::coroutine_handle<>` cannot reach a promise and must return an empty view. That is
  the right answer for a generic awaiter and a trap for an author who expected
  otherwise, so it is documented at the function, not left to be discovered.
- **Doxygen that omits the lifetime rule.** `resume_or_abandon` may destroy the frame the
  calling awaiter lives in, on either branch. A comment that does not carry the same
  DELICATE warning `detail::abandon_chain` carries would ship a loaded gun with no label.
- **Tests that pass without a mutation of record.** The headline property — the body does
  not run — is only evidence if the same test file also shows the same awaiter *running*
  that body when it resumes instead of abandoning. Both variants land, in one file, named
  so the pair is visible.
- **A test that dies for the wrong reason.** `_GLIBCXX_ASSERTIONS` is on by default here
  and aborts with libstdc++'s own text. Every death-test matcher is this project's assert
  text.
- **Rewriting `tests/.clang-tidy`.** Append only, per `CLAUDE.md`.
- **Leaving the corrected claims half-swept.** The claim being falsified is "a foreign
  awaitable's resumption cannot be intercepted", and it appears in docs, in a header
  comment, and as a count ("of which there are three"). A sweep by phrase misses the
  count.

## Constraints

- C++23, GCC 13+ / Clang 19+ / MSVC 19.50+ (Visual Studio 2026). `std::stop_token` and
  `std::coroutine_handle` only; no new standard library dependency and no third-party one.
- **MSVC is unverified for this component.** The development machine is Linux, so nothing
  in `include/alt/cancellation_ref.hpp` or `tests/test_cancellation_ref.cpp` has ever been
  compiled by MSVC; CI's three `Build & Test (MSVC, <preset>)` jobs are the first time it
  will be. `main`'s `f3f0a8b` is proof that this family of code can carry a defect no
  libstdc++ gate reports, so treat a green Linux sweep here as partial.
- Header-only. `cppcheck` analyzes `src/*.cpp` only, so it will not cover this file; the
  statement that all of CI's jobs cover the change means they all *compile and run* it,
  with cppcheck's contribution being nil here, as for every other header-only component.
- `clang-tidy-19` and `clang++-19` pinned per `CLAUDE.md`. The whole-project gate aborts
  at the first finding, so each changed file is also checked directly, with the
  GoogleTest headers passed via `-isystem`.
- The view must be trivially copyable and the size of a pointer. A custom awaiter stores
  one in its coroutine frame between `await_suspend` and completion, which is the only
  way the token and the teardown are both available at completion time.
- `detail::abandon_chain` is `noexcept` and asserts a non-empty root. Everything built on
  it inherits both.

## Approaches

Four were considered. The first was declined in the issue's own terms before this
document; the other three were put side by side.

**A. `<alt/execution.hpp>`, a sender path inside alt.** Gives every alt user sender
support with no adapter to write. Declined: unbuildable and ungated on every supported
toolchain, requires a third-party dependency, and reverses a roadmap exclusion. The
open questions it drags in — several value completions, a non-`exception_ptr` error
channel, `task` as a sender — are the sender/receiver design the roadmap deferred, not
an adapter.

**B. A narrow public extension point.** One non-owning view plus one accessor. alt gains
no knowledge of senders; the downstream adapter gains the two things it was missing.
Trades off a new public surface on the delicate teardown path, and hands callers a
function that can destroy their own frame. **Chosen.**

**D. Generic resume-time interception.** No public API at all and every foreign
awaitable covered. Rejected for the three reasons under Non-goals.

**C. Decline and close #10.** The documented `cancellation_point` mitigation is correct
and costs nothing. Rejected because it leaves a capability gap in alt's own protocol
that has nothing to do with senders: no caller-written awaiter can participate in
resume-time cancellation, however much it wants to.

## The design

### New public header

`include/alt/cancellation_ref.hpp`, a standalone public header including only
`<alt/detail/cancellation.hpp>` and what it needs from the standard library. It is not
included from `task.hpp` or `cancellable_task.hpp`: an awaiter's own header needs the
view and nothing else from the coroutine family, and keeping the dependency arrow
one-way (`cancellation_ref.hpp` → `detail/cancellation.hpp`) costs nothing. Per
`CLAUDE.md`'s convention it belongs directly in `include/alt/` rather than in
`include/alt/detail/`, because it is includable on its own.

### `alt::cancellation_ref`

Shaped on `alt::scheduler_ref`, which set the house pattern for a non-owning reference
with an empty state: default-constructible to empty, `explicit operator bool`, and
preconditions stated as asserts with a message naming the type.

```cpp
class cancellation_ref
{
public:
    /** Constructs an empty view: a chain that nothing can stop. */
    cancellation_ref() noexcept = default;

    /** True when this view names a chain that carries cancellation state. */
    [[nodiscard]] explicit operator bool() const noexcept;

    /**
     * The chain's stop token. An empty, never-stoppable token when this view is
     * empty, so a caller may hand it onward unconditionally.
     */
    [[nodiscard]] std::stop_token get_stop_token() const noexcept;

    /** True once a stop has been requested on this chain. False when empty. */
    [[nodiscard]] bool stop_requested() const noexcept;

    /** Tears the chain down, honouring start_options::home. DELICATE: see below. */
    void abandon() const noexcept;

    /**
     * Resumes h, or tears the chain down if a stop was requested. Returns true
     * when it resumed. DELICATE: see below.
     */
    bool resume_or_abandon(std::coroutine_handle<> h) const noexcept;

private:
    friend /* the accessor */;
    explicit cancellation_ref(detail::cancellation_context* context) noexcept;

    detail::cancellation_context* m_context = nullptr;
};
```

`abandon()` asserts two preconditions and delegates to `detail::abandon_chain`,
inheriting its `noexcept` and its posting of the teardown to `home`:

1. `*this` is non-empty — `"abandoning an empty alt::cancellation_ref"`.
2. **A stop has been requested on this chain** — `"abandoning an alt chain with no stop
   requested"`.

The second one is a design decision rather than defensive padding, and it is the answer
to "is `abandon()` a general destroy-this-chain button?" It is not. This component exists
so that an awaiter can *observe the chain's cancellation* at a resumption it owns, which
is the fourth row of the Intent table and nothing more. An unconditional teardown would
be a new capability nobody asked for, and it is reachable-defect territory inside the
combinators, which was checked against the code rather than assumed:

> A `when_all`/`when_any`/`task_group` child is its own `cancellable_task` root, started
> by `alt::start` with an `on_completion` hook (`when_all.hpp:537-545`), and
> `when_all::await_resume` tells a cancelled chain from a satisfied one *only* by
> `combinator_state::cancel()`'s flag — its own comment (`when_all.hpp:676-700`) records
> 200/200 failures with "reading an alt coroutine result before it was produced" when
> that flag is not what decides. An opted-in awaiter inside a child that abandoned
> without the block having been cancelled would fire the child's completion hook, drive
> the counter to zero, resume the joiner down the *satisfied* path with `cancelled()`
> false, and assemble a slot no child filled.

With the precondition, that hazard is unreachable by construction: a child's only stop
source is the block's, `cancel()` sets its flag before requesting the children's stop,
and `alt::start`'s handle for a child is discarded (`when_all.hpp:758`), so nothing else
can stop a child.

**What an adapter does when its operation stops for a reason alt's token knows nothing
about** — a sender whose `set_stopped` fires on its own schedule — therefore follows from
the same rule: it resumes with a released result, exactly as the six primitives do, and
lets the ordinary cascade tear the chain down at the next suspension point. It does not
call `abandon()`. The reference page says this in those words, because it is the one
question an adapter author will have that the API shape alone does not answer.

`resume_or_abandon(h)` asserts `h` is non-empty, then:

- an empty view, or a view whose chain has no stop requested, resumes `h` and returns
  true;
- otherwise it abandons the chain and returns false.

It reads `m_context` into a local before either branch, and `h` is a by-value parameter,
so nothing in the function touches `*this` after the point at which the frame may be
gone. That ordering is the reason this function exists rather than being left to each
adapter: it is `detail::checked_resume_on::resume_or_abandon`'s body, which carries its
own comment about exactly this hazard, made available to callers.

The empty-view case resuming is deliberate and is what lets one awaiter serve both a
cancellable chain and a plain `alt::task` under `alt::sync_wait` with no branch of its
own.

### The accessor

```cpp
/**
 * The cancellation view of the coroutine h belongs to. Empty when that coroutine
 * carries no cancellation state, which includes every erased coroutine_handle<>.
 */
template<typename Promise>
[[nodiscard]] cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise> h) noexcept;
```

It asserts `h` is non-empty, then returns a view over `h.promise().cancellation_context_pointer()`
when `detail::relays_cancellation<Promise>` is satisfied, and an empty view otherwise.
`std::coroutine_handle<>` is `std::coroutine_handle<void>`, which has no `promise()`, so
it takes the empty branch rather than failing to compile — graceful for a generic awaiter,
and called out in the Doxygen and the reference page because it is otherwise a silent
surprise.

What a custom awaiter must do to reach a non-empty view: declare `await_suspend` as a
template over the promise type, so it receives the typed handle. It already does receive
one — `detail::cancellation_checked::await_suspend` forwards `std::coroutine_handle<Promise>`
straight through to the inner awaiter — so this needs no change to the cancellation
wrapper. An awaiter taking only the erased handle gets an empty view and the pre-existing
entry-only behaviour.

Inside an `alt::cancellable_task` the view is always non-empty. Inside an `alt::task` it
is non-empty when that task was awaited from a chain rooted at a `cancellable_task`, and
empty otherwise, which is the same relay `task_awaiter::await_suspend` already performs.

### One internal move

`detail::relays_cancellation` moves from `include/alt/detail/async_wait.hpp` to
`include/alt/detail/cancellation.hpp`, beside the `cancellation_context` its requirement
names. `async_wait.hpp` already includes `cancellation.hpp`, so no include changes and
no behaviour change; it is a move so that the new header does not have to include the
async-primitive machinery to name the concept.

### The usage this enables

For the record, and as the shape the reference page documents. This is the whole of what
issue #10's prototype could not write:

```cpp
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

        // The chain's own token, not one the caller had to pass in.
        m_operation.start(m_chain.get_stop_token(), this);
    }

    result await_resume() { return std::move(m_result); }

private:
    // Called from whatever thread the operation completes on.
    void on_complete(result r)
    {
        m_result = std::move(r);

        // Last statement: both branches may have destroyed this object.
        m_chain.resume_or_abandon(m_handle);
    }

    Operation               m_operation;
    alt::cancellation_ref   m_chain;
    std::coroutine_handle<> m_handle;
    result                  m_result;
};
```

The view is stored in the awaiter, which lives in the coroutine frame, which stays alive
while suspended; the `cancellation_context` it points at lives in the root frame, which
outlives every frame below it. Both facts are the ones `detail::checked_resume_on`
already relies on for the same storage.

### Re-entrant teardown is supported, and that is a stated guarantee

An adapter's operation usually completes **synchronously inside `request_stop()`**: it
registers a `std::stop_callback` on the token the view handed it, so a stop request
drives the operation to `set_stopped` on the requesting thread. The awaiter then calls
`resume_or_abandon` from inside that callback, and with no `home` the teardown destroys
the root frame — which owns `m_source`, `m_context.token` and `m_external` — while
`request_stop()` is still walking that stop state's callback list, and destroys the leaf
frame holding the callback that is executing.

This is supported. Nothing in alt does it today — all three resumptions the library owns
either run from a scheduler post or deliberately resume rather than tear down — so there
was no precedent to copy, and it was settled on the standard plus a measurement rather
than on either alone:

- **[stopcallback.cons]** permits the *destruction*. A `std::stop_callback` destructor
  called from within its own callback's invocation on the current thread returns without
  waiting, which covers both the leaf's callback and `m_external` (itself mid-invocation
  in the linked-token shape). That citation is correct and stands. Destroying `m_source`
  and `m_context.token` only drops references.
- **Keeping the stop state alive is a separate question**, and the design originally got
  it wrong. This clause read: "the stop state survives because `request_stop()`'s
  caller necessarily holds a source that outlives the call." That is false for the
  linked-token shape. With an external `start_options::token` and the chain's own
  `cancellation_handle` discarded, every owner of the *chain's* stop state lives inside
  the frames the teardown destroys — `m_source`, `m_context.token`, and the
  `detail::stop_forwarder` inside `m_external`. The caller of the chain's own nested
  `request_stop()` is that `stop_forwarder`, in the frame being freed; the caller of the
  outer one holds a source for the **external** state, not for the chain's.

  The correct condition is: **the chain's stop state must retain an owner outside the
  frames the teardown destroys.** A retained `cancellation_handle` from `alt::start`
  provides that. The linked-token-with-discarded-handle shape does not, and is safe only
  because `detail::stop_forwarder::operator()` takes a counted copy of the source on its
  own stack before requesting the stop, precedented by
  `detail::combinator_state::cancel`/`request_children_stop` and by `when_all`'s
  DELICATE note on the children's stop source.

  **That copy was not added for this component**, which an earlier version of this clause
  claimed. `main`'s commit `f3f0a8b` added it first, independently, while porting the
  library to MSVC: MSVC's `request_stop()` relocks the callback list after every callback
  including the last, so without the copy every cancellation test that destroys a parked
  frame hangs. The defect is therefore a plain defect of `cancellable_task` on every
  platform, not something this extension point introduced. This work rediscovered it and
  reached the same fix; the merge of `main` took `f3f0a8b`'s version wholesale, including
  its non-const `std::stop_source keep_alive` — the standard declares `request_stop()`
  non-const, as MSVC's library does, and the `const` local this work had written compiles
  only against libstdc++. What this work contributes is the regression test `f3f0a8b`
  lacks and the corrected proviso above.
- **What the design-time measurement actually showed.** It was run on GCC 15.2 and
  `clang++-19`, under AddressSanitizer + UBSan and under ThreadSanitizer, in both
  realistic shapes: a stop through the chain's own `cancellation_handle`, and a stop
  through a linked `start_options::token` with the chain's own handle discarded. All four
  runs were clean, the body did not run, and the stopping party's source was still usable
  afterwards. All of that is true and was reproduced.

  It was *read* as confirming the clause above, and it cannot. Every case it exercised had
  the abandoning `std::stop_callback` as the **only** registration on the chain's token,
  and libstdc++'s `_M_request_stop` early-returns on the last callback (`if (__last_cb)
  return true;`) without touching the state again — so the walk never reads the freed
  state and the measurement is clean whether the clause holds or not. It therefore never
  tested the condition the clause turns on. Add one further `std::stop_callback` on the
  chain's own token and the same shape is a heap-use-after-free: a `READ` in
  `_Stop_state_t::_M_lock()` reached from `stop_forwarder::operator()`. That second
  registration only became constructible with this component — before `cancellation_ref`
  there was no public way for a body to obtain its own chain's token — which is why the
  *libstdc++-visible shape* arrived with this work. The defect itself did not: it predates
  this branch, and MSVC reaches it without `cancellation_ref` at all, which is how
  `f3f0a8b` found it first. The missing thing on Linux was never the tool and never this
  component; it was a second registration on the chain's token, and libstdc++'s skipped
  relock after the last callback is why nothing before that saw anything.

The guarantee on the reference page is stated in the corrected narrow form: safe provided
the chain's stop state retains an owner outside the frames the teardown destroys. It is
**not** stated as "re-entrancy is always fine", because a green sanitizer run is weak
evidence for this hazard class — a freed-chain access reached only through a lock has no
instrumented access on its path, which `detail/waiter_list.hpp` records this library
paying for once already. Here the instrumented access does exist (`_M_lock()`'s read of
the state's own word, inlined from `<stop_token>`), so the tool was never the limitation;
the *shape* was.

Tests: the two shapes above, plus `ATeardownWithASecondCallbackOnTheChainsTokenIsSafe` —
the linked-token shape with a second, non-abandoning `std::stop_callback` registered on
the chain's token so libstdc++'s walk continues past the abandoning one. That third test
is the instrument for the keepalive; the first two pass with or without it. All under the
AddressSanitizer and ThreadSanitizer gates. These are the design's thread-crossing cases,
replacing the vaguer "something to instrument" the first draft asked for.

### The awaiter does not have to be movable, and the page says how

Issue #10's third limit stands and is **not** fixed here: `await_transform` *deduces*
the awaitable through a forwarding reference (`task.hpp:76-77`,
`cancellable_task.hpp:239-240`) and the wrapper it builds stores what it deduced by
value — the capture is `cancellation_checked_awaitable<A>`'s `A m_awaitable` member
(`detail/cancellation.hpp:254-255`), not a by-value `await_transform` parameter. So
`co_await my_awaiter{...}` as a prvalue deduces `A = my_awaiter` and needs a move
constructor, and a sender's operation state is immovable (CWG2327). Changing that
capture is a much larger change to the cancellation wrapper than this component, and
the issue itself says it is not a bug in alt. Note that issue #10 words this as
"`await_transform` takes the awaitable by value"; that is the imprecision, and it
matters here because the deduction is exactly what makes the lvalue form below work.

It is also dodgeable without placement-new, which is worth documenting because the
recipe above would otherwise mislead an author whose operation is immovable. Awaiting a
**named lvalue** deduces `A = my_awaiter&`, and `cancellation_checked_awaitable<A>` then
stores a reference rather than a value — which the comment at
`detail/cancellation.hpp:235-241` says it does deliberately, to keep a reference a
reference. No move is required of the awaiter at any point:

```cpp
my_awaiter<my_operation> awaiter{/* ... */};   // immovable member is fine
co_await awaiter;                              // named lvalue: nothing is moved
```

Measured rather than reasoned: the feasibility probe for this design awaited an awaiter
holding a `std::optional<std::stop_callback<...>>`, which is immovable because
`std::stop_callback` is, in exactly this form, on GCC 15.2 and `clang++-19`.

The reference page carries both forms and says which constraint applies to which. One
test awaiter holds a deliberately immovable member and is awaited as an lvalue, so the
page cannot drift from the language rule.

## Documentation changes

A new reference page and a sweep of the claim this change falsifies. The claim is "a
foreign awaitable's resumption cannot be intercepted, so foreign awaitables are checked
on entry only", and it is stated in more than one form, including as a count:

| File | What changes |
|------|--------------|
| `docs/cancellation_ref.md` | New reference page: the view, the accessor, the opt-in recipe above, the lifetime rules, the erased-handle trap, what the empty view means, and the explicit statement that an awaiter may still choose to resume with a released result as the six primitives do |
| `docs/cancellable_task.md` | "Foreign awaitables are checked on entry only" becomes conditional on the author not opting in, with a pointer to the new page; the two separate statements that the library owns "three" resumptions (near the top and in that section) become four, the fourth being a caller's awaiter; the `my_awaiter` example there is the uncovered case and stays, now contrasted with the opted-in version rather than only with `resume_on` |
| `include/alt/detail/cancellation.hpp` | The `checked_resume_on` comment says "It is done for the resumptions alt owns, of which this is one, and foreign awaitables remain checked on entry only" — corrected to name the opt-in |
| `docs/awaitable.md` | A cross-reference from the write-your-own-awaitable material to the new page |
| `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md` | The sender/receiver exclusion row gains a note that issue #10's dependency-free half landed as this component and that the sender adapter itself stays excluded, so the decision is recorded where the exclusion is |
| `README.md` | A row in the Components table |

Two claims deliberately left alone, having been checked and found still true: "A task
parked on something that never resumes never self-destructs" (an opted-in awaiter whose
operation never completes still never reaches a resumption, so the rule holds), and the
narrowing of point 6 for the six primitives (unchanged by this).

One further site the sweep found and deliberately does **not** change:
`docs/superpowers/specs/2026-10-26-async-primitives-design.md:90` says the check fires
"at the two resumptions alt owns". That is a dated design spec of work already shipped,
and two was the correct count on 2026-10-26; the same sentence in the *reference* docs is
the one that has to track the code. Dated specs in this directory are historical records,
so the builder leaves it, and leaves the count in this document's own Intent table, which
describes the state before the change.

## Tests

A new `tests/test_cancellation_ref.cpp`, added to the `alt_tests` sources list in
`tests/CMakeLists.txt`. It builds on `tests/coroutine_test_support.hpp` for
`frame_tracker` and `live_frames`, and on `tests/death_test_support.hpp` for the death
tests. `tests/test_cancellation.cpp`'s `test_root` shows the pattern for a minimal root
with a `cancellation_context`, but these tests use the real `alt::cancellable_task` and
`alt::start`, since the point is the public surface.

The headline property and its mutation, which land together:

| Case | What it pins |
|------|--------------|
| `ABodyDoesNotRunWhenTheAwaiterAbandons` | opted-in awaiter, stop requested while parked, operation then completes: the statement after the `co_await` never runs and `live_frames` reaches zero |
| `ABodyDoesRunWhenTheAwaiterResumesInstead` | **the mutation.** The same awaiter calling `h.resume()` in place of `resume_or_abandon`: the statement after the `co_await` does run. Without this the first case is consistent with the body never having been reachable |

The rest:

- `AViewInsideACancellableTaskIsNotEmpty`, and `AViewInsideAPlainTaskUnderSyncWaitIsEmpty`.
- `TheViewCarriesTheChainsStopToken`: a stop requested through the
  `alt::cancellation_handle` is observed through the token the awaiter read, and
  `stop_requested()` agrees.
- `ResumeOrAbandonResumesWhenNoStopWasRequested`: returns true, the body runs, the result
  is the awaiter's.
- `ResumeOrAbandonResumesWhenTheViewIsEmpty`: the same awaiter inside a plain `alt::task`
  driven by `alt::sync_wait`, where there is no stop state at all.
- `AbandonPostsTheTeardownToHome`: with `start_options::home` set to a
  `alt::manual_scheduler`, `resume_or_abandon` returns false and the frames are still
  alive until that scheduler is drained — the property `detail::abandon_chain` provides
  and that an adapter would otherwise have to reimplement.
- `AStopBeforeTheAwaitStopsTheOperationStarting`: the pre-existing entry check still
  fires ahead of an opted-in awaiter, so `await_suspend` is never reached and the
  awaiter's start count stays zero. This guards against the new path being read as a
  replacement for the entry check rather than an addition to it.
- `AnAwaiterTakingTheErasedHandleGetsAnEmptyView`: the documented trap, pinned so the
  documentation cannot drift from it.
- `AnImmovableAwaiterWorksAsAnLvalue`: an awaiter with a deliberately immovable member,
  awaited as a named lvalue, so the page's claim about issue #10's third limit is pinned
  by the compiler rather than by prose.
- `ACombinatorChildKeepsItsBlocksVerdict`: an opted-in awaiter inside a `when_all` child,
  where the block is cancelled and the child abandons. The verdict is
  `std::errc::operation_canceled` and no slot is read unfilled. This is the one place the
  new API could corrupt an existing component rather than only its own chain, and the
  `abandon()` precondition is what rules it out; the test is the guard of record for that
  reasoning.
- `ATeardownFromInsideRequestStopIsSafe` and
  `ATeardownFromInsideALinkedTokensRequestStopIsSafe`: the two re-entrant shapes the
  guarantee above covers, each asserting the body did not run and the stopping party's
  source is still usable afterwards. These carry the AddressSanitizer and ThreadSanitizer
  gates for this component. Note that neither one discriminates the guarantee's
  proviso — see the correction above — which is what
  `ATeardownWithASecondCallbackOnTheChainsTokenIsSafe` is for.
- A thread-crossing case on top of those: the operation completes on a `std::jthread`
  while the stop is requested from the test thread, synchronized with a latch rather than
  a sleep, so the resume branch is also instrumented off-thread.
  `AnOffThreadCompletionResumesSafely` covers the off-thread resume but orders everything
  with `join()` and never stops the chain, so it is not that case;
  `AnOffThreadCompletionRacesAStopRequest` is, releasing both sides through a
  `std::latch` and asserting the disjunction (frames gone either way, body ran iff
  `resume_or_abandon` resumed) rather than a fixed winner.
- Death tests, in the suite's house pattern (`NDEBUG` and ThreadSanitizer skip guards, a
  scoped `"threadsafe"` style, `TEST(CancellationRefDeathTest, ...)`): `abandon()` on an
  empty view, matching `"abandoning an empty alt::cancellation_ref"`, and
  `resume_or_abandon` with an empty handle, matching its own message.

Coverage: the `Coverage` preset's own run is the measurement, per `CLAUDE.md` — no
separate `ctest` pass ahead of it.

## What is left to the builder

- The exact wording of every Doxygen block and of `docs/cancellation_ref.md`, subject to
  the DELICATE warning being present on `abandon` and `resume_or_abandon` and to the
  erased-handle trap being stated at `get_cancellation_ref`.
- Whether the private constructor is reached by making the accessor a friend or by a
  `detail` tag type. Either is fine; the constructor must not be public, because a
  caller holding a `cancellation_context*` is the thing this component exists to avoid.
- Assert message wording beyond the one match above, and the death tests' skip messages,
  which are per-file by convention.
- Test case names, within the pairing requirement for the two headline cases.
- Any `NOLINT` suppression the pinned `clang-tidy-19` turns out to need, with a measured
  figure where the suppression carries one, and appended to `tests/.clang-tidy` where it
  belongs there instead.

## Version bump

Decided with the owner on 2026-11-17: **a minor bump.** The number was 1.12.3 → 1.13.0 when
this was decided; `main` subsequently took 1.13.0 for itself (commit `470c36e`, MSVC
support), so after the merge of `main` this branch is **1.13.0 → 1.14.0**. A new public header and
a new public type mean headers from 1.12.x and this library are not interchangeable, which
is what `README.md`'s `SameMinorVersion` rule expresses and what the package config
refuses on. It lands as its own commit on the branch, message `Bump version to 1.14.0`,
touching all six files in `CLAUDE.md`'s table — including the third place in
`tests/test_version.cpp`, where the `static_assert` spells the number bare and a grep for
the old string misses it, and the `find_package` compatibility prose in both `README.md`
and `CLAUDE.md`, whose operands illustrate the rule rather than this release.

# `alt::when_any` design (phase 4, stage B)

**Date:** 2026-11-15
**Status:** Draft
**Related:** `2026-11-14-combinators-design.md` (phase 4, both stages; stage A shipped in PR #14),
`2026-09-10-coroutines-roadmap.md` (phase order)

## Purpose

Phase 4's second half. `alt::when_all` shipped on 2026-11-15; `when_any` completes the
phase: await several awaitables, complete when the first one has, and tear the rest down.

Two things motivate it beyond symmetry. The roadmap's driving requirement is a coroutine
type a UI framework can tell to stop, and racing work against a cancellation or a deadline
is how that requirement shows up in application code. And phase 5 explicitly deferred
timeouts as a non-goal "on exactly that ground: race a completion source against a timer
once `when_any` exists".

This document supersedes the stage B section of the combinators design. Where the two
disagree, this one is current, and the disagreements are called out in **Corrections to the
combinators spec** below.

## Goals

- Race N awaitables of **heterogeneous** result types, variadic and sized-range forms, the
  same two shapes `when_all` offers.
- Yield the winner's **position** plus the winner's **value**, through a named result type
  rather than a bare `std::variant`.
- Stop every loser, and **do not resume the awaiting chain until every child has retired**.
- Report cancellation of the whole race as an observable `std::expected` error, exactly as
  stage A does.
- Document both ways a loser's work is lost.
- Make the use-after-free class that produced stage A's only Critical finding **detectable
  by the test suite**, and close the gap in `detail::wait_awaiter` that produced it.

## Non-goals

- **Timers and a clock source.** Still absent from this library. `when_any` makes a timeout
  *expressible*; it does not supply the timer. A caller supplies its own.
- **`when_any_succeed`.** A "first child that completes *successfully*" operation,
  deliberately out of scope here and since left unbuilt for want of a caller; see
  **Future work**.
- **Fixing the loser resource-loss mode.** Documented, not fixed. Carried forward from the
  combinators spec's decision 7.
- **A `std::variant`-typed public result.** Carried forward from decision 6.
- **Changing `when_all`.** Stage A is shipped and green; this stage adds a sibling and
  touches shared internals, not `when_all`'s semantics.

## Anti-goals

Each of these would make the component a failure even if it compiled and passed its tests.

- **Returning from a won race while losers are still running.** It would make stage A's
  lvalue precondition unenforceable: a caller would have to keep an awaitable's referents
  alive past a point it cannot observe. **This binds the winner path only.** The
  chain-cancellation path deliberately resumes at once while children are still unwinding,
  exactly as stage A's does, because the roadmap's driving requirement is that a window can
  close while one of its coroutines is still running work on a worker thread. Requiring
  teardown to finish before a cancelled await returns would contradict that requirement,
  which is why `when_all`'s precondition is phrased "must outlive **the children**" rather
  than "the await".
- **Hanging on a race with no entrants.** A `when_any` over zero children can never
  complete. Parking forever is the worst available answer.
- **An ordering claim that rests only on argument.** Stage A shipped one such claim as a
  Critical bug through a full review cycle. Every ordering this stage depends on must be
  either mutation-measured or mechanically checked.

## Constraints

From `CLAUDE.md` and the shipped codebase, not negotiable here:

- C++23; GCC 13+ or Clang 19+.
- `snake_case` throughout, `PascalCase` template typenames, `m_` on private and protected
  members, `const` by default, `std::expected` for recoverable errors, Doxygen on every
  public symbol.
- Header-only component in `include/alt/when_any.hpp`; helper headers that no caller would
  include alone go in `include/alt/detail/`.
- All eight CI gates green: GCC, Clang, optimized, ASan+UBSan, TSan, `clang-tidy-19`
  (pinned at `/usr/bin/clang-tidy-19` with `clang-19`/`clang++-19` and
  `CMAKE_BUILD_TYPE=Debug`), cppcheck, Coverage.
- `tests/.clang-tidy` is appended to, never rewritten.
- American spelling, except the approved `cancelled`/`cancellable`/`cancellation` forms.
- Work reaches `main` through a pull request; the version bump is a standalone commit on the
  branch.

## Approaches

Three decisions had real alternatives. Each was settled in conversation before this document
was written.

### How the winner's result reaches the caller

The combinators spec says `when_any` yields "the index of the winner plus that winner's
result, rather than a variant". For heterogeneous children that is not expressible as
written: the type of "the winner's result" is not known at compile time, and a tuple with
one position filled would force default-constructibility on every loser's value type, which
stage A deliberately never required.

| Approach | Trade-off |
|---|---|
| **A named result type** (chosen) | `when_any_result` exposes `index()` and a typed `get<I>()`. Heterogeneous works, nothing needs default-constructing, it documents well. Stores a variant internally, so decision 6 is honored as a constraint on the interface rather than the storage. |
| Index plus the slot tuple | No new type, but it hands callers a footgun — reading a loser's slot is a precondition violation — and leaks `detail::result_store`'s shape into the public API. |
| Homogeneous-only variadic | Simplest: `std::pair<std::size_t, T>`. Rejected because it cuts out the motivating case: racing a `T`-returning fetch against a `void`-returning timer is heterogeneous by nature. |

### Whether the await returns before the losers finish

| Approach | Trade-off |
|---|---|
| **Park until the last child retires** (chosen) | When the await returns, no child of the caller's is running, so an lvalue awaitable's referents need only outlive the await. Makes `when_any` a small delta on stage A's block, fan-out, per-child roots and completion counter. Costs latency: the winner's value is held back until the losers finish unwinding. |
| Resume on the winner's election | The literal reading of "completes when the first one has". Rejected: it makes the lvalue precondition unenforceable, and needs a new resumption mechanism because the chain could no longer park on the counter reaching zero. `task_group` already serves genuinely fire-and-forget work. |

### How the invisible use-after-free class is handled

Stage A's Critical finding was a heap-use-after-free that **neither AddressSanitizer nor
ThreadSanitizer reported**, because as stage A shipped `waiter_list`, `claim`'s only
contact with the freed block was a `std::scoped_lock` over a `std::mutex` — glibc's
`pthread_mutex_lock`, which neither tool instruments — and libstdc++'s trivial
`std::mutex` destructor means no `pthread_mutex_destroy` is intercepted either. Measured
then: 0 failures in 20 repeats per sanitizer preset with the defect present, against a
scratch-instrumented trace proving the window was reached.

**The partition is three windows, not two.** This is what the sentinel's fix round
established, and it is more precise than the two-way framing the rest of this section was
written with. `claim` now opens with `alive()`, an ordinary four-byte load of the list's
own storage, inlined into the caller:

| Window | What covers it |
|---|---|
| Destroyed, not yet freed | The sentinel, and only the sentinel. `~waiter_list` has written `dead_pattern`, the bytes are intact, and no memory error exists for a tool to find. |
| Freed, chunk still poisoned | AddressSanitizer, at `alive()`'s load, before the assert can fire. The sentinel's load is what gives the tool an instrumented access on this path; the lock alone never did. |
| Freed, handed out again and written over | **Neither reliably.** AddressSanitizer is silent once the chunk has been handed out again. The sentinel is probabilistic here: reused bytes hold neither pattern, so `alive()` rejects them — but they can hold `alive_pattern` by chance, which is one value in 2^32. Put the other way round, the positive test rejects all but one value in 2^32. |

Two riders. The asserts are asserts, so an `NDEBUG` build has the first window
uncovered and loses `alive()`'s load with it, which returns the second window to exactly
the blindness above. And the check is not atomic with the lock that follows it, so a
caller that passes `alive()` an instant before `~waiter_list`'s store still locks a
destroyed mutex: the sentinel narrows the first window rather than closing it, and the
precondition stays the owner's.

`when_any` turns that path into the normal one. In `when_all` the chain resumes while
children hold block references only on cancellation; in `when_any` it happens on every
success.

| Approach | Trade-off |
|---|---|
| Comments and argument only | What stage A did. Direct evidence of insufficiency: the bug survived a task review, five fix rounds and a whole-branch review, and was caught by a reviewer reading the code rather than by any tool. |
| **Make the class detectable** (chosen) | A sentinel in `waiter_list`, checked before the election and before the mutex. Turns an invisible use-after-free into a loud assert in every Debug, ASan and TSan preset. Fully reliable for the destroyed-but-not-yet-freed window — the window a test can actually reach — and, because `alive()` is an instrumented load, it is also what lets AddressSanitizer report the freed-but-poisoned one. Probabilistic for freed-and-reused memory, and absent under `NDEBUG`. With it in place stage A's regression test *can* fail on the defect, where before it could not fail at all — but only with **both** of `when_all`'s deregistration statements removed. Either one alone satisfies that test, measured 20/20 in all three presets, so it fails only when neither is present. |
| **Remove the class at its source** (chosen) | Reset `m_stop` on `wait_awaiter`'s refused-link path, so a refused link leaves no callback in existence. Fixes the gap for every present and future `wait_awaiter` user. Costs a change to a header six shipped components share, so it needs its own evidence and re-verification. Covers the refused-link path only. On the LINKED path the window is reachable — measured in `when_all`, one awaiter destruction in a few thousand frees the block with the joiner's callback still engaged — and is inert there only because the single party that could claim the freed list is the invocation that resumed the chain, on the same thread, past its claim. **That argument is an enumeration of `when_all`'s claimers, and `when_any` is where it may stop holding:** a combinator that parks more than one joiner on a counter, or parks before its fan-out ends, has a claimer that is neither. See `~when_all_awaiter`'s note. |

The last two were chosen together: the source fix removes the common case, and the sentinel
is what makes that fix — and stage A's existing one — checkable rather than argued.

## Decisions

1. **`when_any` yields a named result type.** `when_any_result` carries the winner's
   position and value. Its interface is `index()` plus a typed accessor that debug-asserts
   the caller is asking for the position that won. Internally a variant over the children's
   value types, mapped by stage A's `detail::combinator_value_t`, so `void` becomes
   `std::monostate` and `T&` becomes `std::reference_wrapper<T>` exactly as in `when_all`.

2. **The wait yields `std::expected<when_any_result<...>, std::error_code>`**, reporting
   `std::errc::operation_canceled` when the race is released by the chain's own
   cancellation. Identical convention to stage A, for the same reason: the resumed body
   runs on with no further check, so cancellation must be observable in the value.

   **A cancellation that arrives after a winner was elected still reports
   `operation_canceled`, discarding the winner.** The window is real rather than
   theoretical, because the chain stays parked while the losers unwind (decision 4), and a
   caller being torn down should stop rather than act on a result in a window that is
   closing. This matches `when_all`, whose `cancelled()` test already discards a fully
   assembled satisfied result. The cost is decision 4's extra member.

3. **Winner election is a CAS.** Several children can complete concurrently; the first hook
   whose compare-exchange lands is the winner and every later arrival loses. There is no
   tiebreak: "lowest index wins" would require withholding the verdict until every child had
   finished or been stopped, which defeats the operation.

4. **The election requests the losers' stop, and the chain stays parked until the
   completion counter reaches zero.** Reuses stage A's `detail::completion_counter`
   unchanged.

   `detail::combinator_state` gains one **additive** member: a stop-only
   `request_children_stop()`. Today the only way to stop the children is `cancel()`, which
   deliberately sets `m_cancelled` *before* requesting the stop, and that flag is the whole
   of how a combinator's `await_resume` tells a satisfied wait from one released over
   unfilled slots. If the election called `cancel()`, the flag would be set on the success
   path too and could no longer mean "the chain was cancelled" — which is exactly what
   decision 2 needs it to mean. Separating the two is also the clearer design: stopping the
   children and observing that the chain was cancelled are different operations, and
   conflating them is what would force a priority rule between them. No existing path
   changes, but this does touch a header six shipped components share, so it is verified
   with them.

5. **A child that completes by throwing is a completed child, and wins.** Its exception is
   rethrown at the await. Consistent with "completes when the first one has" and with
   `when_all` surfacing exceptions at the await. The alternative — a throw counts as a loss
   and the race continues — is a different operation, named separately under **Future
   work**.

6. **Two loss modes are documented.**
   - A **completed** loser's valid result is discarded. The combinators spec documents only
     the cancelled-loser case; this one is more common, being two plain tasks both simply
     finishing.
   - A **loser's exception** is discarded. This is an asymmetry with `when_all`, which
     rethrows the lowest-position exception, and it must be stated as such.
   - The cancelled-loser resource loss carried over from the combinators spec: a semaphore
     permit consumed and never returned, a `task_completion_source` value set and lost. An
     `async_mutex` loser is harmless, since the guard's destructor unlocks.

7. **A zero-child race is a compile error in the variadic form.** A race with no entrants
   can never complete. The range form cannot catch an empty range at compile time, so an
   empty range returns an error rather than parking forever; the builder picks the code, and
   `std::errc::invalid_argument` is the obvious candidate. Note that `when_all()` over zero
   children completing immediately is correct and is not the same shape.

8. **`waiter_list` gains a liveness sentinel**, set by its constructor and overwritten by
   its destructor, checked by `claim` and `link` before the mutex is touched — and in
   `claim` before the election as well, so that a late claim which would lose it and touch
   nothing still aborts. The assert fires in Debug, ASan and TSan. Whether it compiles
   away entirely in Release is the builder's call; the gates that matter for it are all
   Debug-based.

   **PR 2 must decide this deliberately rather than discover it.** `claim`'s assert sitting
   ahead of the election is what this decision asks for, and PR 1 pinned it there with
   `WaiterListDeathTest.AClaimThatWouldLoseTheElectionOnADestroyedListAborts`. In `when_all`
   that costs nothing: the only way a *benign* late-losing claim meets a dead list today is
   inside `~waiter_list`'s backstop drain, which is non-empty only if something re-awaited
   during the primitive's own explicit drain — already-unsupported territory. A combinator
   that **parks more than one joiner on a counter** changes that. "A sibling's late claim
   loses the election and touches nothing" becomes a *normal* shape during block teardown,
   and this assert aborts on it — a benign race, reported as a precondition violation, in
   every Debug, ASan, TSan and Coverage preset. So before `when_any` parks a second joiner,
   choose one of: keep the assert where it is and guarantee no sibling can claim after the
   list dies; narrow it to claims that would *win*, losing decision 8's stated reach and the
   death test with it; or give the counter's list a lifetime that outlives every sibling
   claim. Picking none of the three means finding out from a failing gate.

9. **`wait_awaiter::await_suspend` resets `m_stop` on the refused-link path.** Verified safe
   at `include/alt/detail/async_wait.hpp:275-284`: `~stop_callback` blocks for an invocation
   on another thread and returns immediately for one on the current thread, and the only
   inline invocation possible happens during `emplace`, which has completed by then. This
   lands **in its own pull request, ahead of any `when_any` code**, together with decision
   8's sentinel; see **Staging**.

## Staging

Two pull requests, in this order. The split exists because decisions 8 and 9 change headers
that `async_event`, `async_mutex`, `async_latch`, `async_semaphore`,
`task_completion_source` and `task_group` all share, and a change with that blast radius
should be revertible without touching `when_any`, reviewed on its own merits, and green on
`main` before new code depends on it. Both are independently valuable: they close a latent
gap for every waiter in the library, and the evidence for them exists today, since stage A's
C1 finding proves the gap without `when_any` needing to exist.

**PR 1 — make the invisible class detectable, and close it at its source.** Decision 8's
`waiter_list` sentinel and decision 9's refused-link reset, each with its own mutation
evidence, plus the negative control: remove the assert, confirm the use-after-free goes
unreported again, restore it. Re-verifies the six phase-5 components. It adds no public API,
but it does break ABI, so it took a **minor** bump, 1.10.0 to **1.11.0**, and 1.10.1 was
never released. The sentinel grows `sizeof(detail::waiter_list)` — measured 64 to 72, and on
through `async_event` 72 to 80 and every type that embeds one — and this library is not
header-only at those types: `src/async_event.cpp`, `async_latch.cpp`, `async_mutex.cpp`,
`async_semaphore.cpp` and `task_group.cpp` compile out-of-line definitions into `libalt`,
which ships as a `.deb`. A layout change is therefore a real break between a prebuilt
`libalt` and objects compiled against either header, which a patch bump would not have
announced. (The earlier patch-bump ruling here was mine, made before the ABI reach was
measured.)

**PR 2 — `when_any`.** Everything else here, including decision 4's additive
`combinator_state` member, decision 10's rewording across the three live files, and the
documentation. New public API, so a **minor** bump, **1.11.1 to 1.12.0**.

10. **Invariants (A) and (C) are sharpened here**, discharging the item stage A deferred.
    They read as near-duplicates in the combinators spec's Global Constraints, which is
    where stage A's comments took them from. (C)'s only distinct content today is its
    second clause, so the sharpening separates the two along the axis each is actually
    about — **who destroys** versus **when the awaiting side may die**:

    > **(A) Ownership.** No combinator owns or destroys a child frame. Children are
    > self-owning `alt::cancellable_task` roots, and a combinator only ever requests their
    > stop.
    >
    > **(C) Lifetime overlap.** A combinator may resume the awaiting chain — and so let
    > that chain destroy the combinator's own frame — while children are still alive. This
    > is why the shared state is reference counted off the frame instead of owned by it,
    > and (A) is what keeps the sibling hazard away across that overlap: the children that
    > outlive the frame destroy nothing but themselves.

    (B) is unchanged. **Three** live files carry the labeled invariants and are in scope for
    the rewording: `include/alt/when_all.hpp`, `include/alt/async_latch.hpp` and
    `docs/when_all.md`. The combinators spec and the stage A plan also carry them and are
    **out of scope** — they are the historical record of what was decided then, and this
    document is where the wording changes.

11. **A race that ends with no winner and no cancellation reports the released error.**
    This is reachable without a bug in `when_any`: a child's frame can be destroyed by its
    home scheduler being torn down with the teardown still queued, which fires the
    completion hook, drives the counter to zero and fills no slot, with `cancelled()` still
    false. Stage A's effective stance on that shape is "precondition violation" — it walks
    into `detail::result_store`'s "reading an alt coroutine result before it was produced"
    assert — but `when_all` cannot distinguish the case, whereas `when_any` can detect it
    for free, since no winner means no slot to read. So `when_any` reports
    `std::errc::operation_canceled` rather than asserting, and documents that a race whose
    children all vanish without completing is reported as released. My call, not the
    author's; flagged here so it can be checked.

    **CORRECTION, 2026-11-16, after the whole-branch review.** The requirement above stands
    and is implemented. The *reason* given for it is wrong, and both facts are left here
    rather than one of them edited away, because a decision made on a false premise should
    not be able to read afterwards as though it had been made on a true one.

    **The requirement stands.** `detail::when_any_awaiter::await_resume` returns the
    released error when the election reports `combinator_election::no_winner`, and the
    debug assert is kept *above* that branch rather than replaced by it — so the assert is
    still the backstop wherever asserts are compiled in, and the branch is what the
    component does in a build where they are not. Reporting
    `std::errc::operation_canceled` was the right call: the alternative in a release build
    is not a diagnosable failure but an out-of-range slot read in the range form, and in
    the argument-list form a `std::bad_variant_access` thrown out of an await whose
    documented error channel holds only `operation_canceled` and `invalid_argument`.

    **The mechanism named above does not produce the state.** The second sentence of this
    decision says a child's frame destroyed by its home scheduler being torn down "fires
    the completion hook, drives the counter to zero and fills no slot". It does not.
    `start_options::home`'s own documentation
    (`include/alt/cancellable_task.hpp:110-112`) says the opposite about exactly that
    scheduler: one "that is never drained, or that is destroyed with this work still
    queued, leaks the whole chain silently rather than crashing." A dropped `work_item` is
    two pointers; the frame is never destroyed, so the hook never fires, the counter never
    drains, and the awaiting chain parks forever. The outcome is a **hang**, not a
    satisfied wait with no winner. So this decision cited the one mechanism that cannot
    reach the shape it was written for.

    **What is actually known about reaching it.** Nothing. No test in the suite reaches the
    state, and every other route to a child retiring without completing goes through a
    stop, which is either `combinator_state::cancel()` — whose flag is stored before the
    children's stop and tested in `await_resume` ahead of the election — or
    `request_children_stop()`, which only an already elected winner calls. The only
    evidence the branch does anything at all is a negative control, and the control took
    **two** changes at once: measured 2026-11-16 against the suite at 800 tests, with
    `await_resume`'s verdict statement deleted *and* the branch removed, under `NDEBUG`,
    `WhenAnyTest.ACancelledRaceReportsOperationCanceled` aborts 3 of 3 on an uncaught
    `std::bad_variant_access` out of `when_any_result_at`'s last position. **Removing the
    branch alone is caught by nothing**: all 800 tests pass in debug with it gone, which
    the scoped re-review measured on the committed tree. An earlier draft of this
    paragraph quoted the control with only its second half, which claimed something that
    does not happen. The range form's `slots[SIZE_MAX]` is read from the code and was not
    measured at all. The honest account lives with the code, in `await_resume`'s note and
    in `docs/when_any.md`, and neither offers a reachability story.

    **The flag worked and the check did not.** This decision's last line asked to be
    checked on precisely this point, and nothing checked it: the implementation shipped
    with the assert and no branch, and `docs/superpowers/plans/2026-11-16-when-any.md`'s
    own coverage self-check recorded the decision as implemented by a plan step that never
    specified it. That self-check did not merely MISS the question — it **closed** it. A
    reader of the plan who wanted to know whether decision 11 was covered found the answer
    "yes" in writing, so the one document whose job was to catch the gap is the document
    that made the gap invisible, and the three stages then agreed with each other all the
    way to the finished branch. The claim was about COVERAGE rather than about code, which
    is why nothing compiled or ran could contradict it; a grep of Task 3 Step 5's own text
    for the branch it named would have refuted it in one line, and that is the check to
    run on a coverage claim. The gap was found only by the whole-branch review of the
    finished branch. All three records are corrected in place with their corrections
    attached; this one is the source the other two argued from.

## Corrections to the combinators spec

- **Decision 6 as written is not implementable for heterogeneous children.** It is honored
  as a constraint on the public interface; the storage is a variant. See **Approaches**.
- **The stage B sketch does not say whether the await waits for the losers.** It does on the
  winner path, by decision 4, and that is load-bearing rather than incidental. It does not
  on the cancellation path; see the anti-goals.
- **`combinator_state` is not reused entirely unchanged**, as an earlier draft of this
  document claimed. Decision 4 adds one member to it, for the reason given there.
- **The spec documents one loss mode; there are two.** Decision 6 above.
- **Invariants (A) and (C) are near-duplicates.** Decision 10.

## Future work

**`when_any_succeed`** — completes with the first child that completes *successfully*,
treating a throwing child as a loser. Explicitly not designed here, and **not built**: the
owner went looking for a caller on 2026-11-17 and did not find one, so it stays unbuilt
until a real one turns up. What follows is what that search established, recorded so the
next reader starts from it instead of re-deriving it.

**The storage question is already answered, and the framing it rested on was wrong.** An
earlier version of this entry said the choice between reporting the last exception and
reporting an aggregate "interacts with how many exceptions the shared block must store,
which is a design question of its own". The shipped component falsifies that.
`detail::when_any_child` fills its slot on every path — `catch(...)` stores
`slot->set_exception(std::current_exception())`, at `include/alt/when_any.hpp:415` — and it
does so **before** it elects, because `detail::combinator_election`'s publication argument
requires that order. So every child's exception is already in the block, one slot each,
winner or loser; a loser's slot is written and never read, which that function's own note
states. An aggregate costs no storage the component does not already pay. **The open
question is therefore purely the caller-facing contract**: what the wait returns, and how a
caller reaches N exceptions rather than one.

**It is not the fan-out shape, which is the shape most likely to be reached for.** Sending a
notification to several sinks, logging the ones that fail, retrying only if none got
through, is *not* this component. On election the winner's walk tears the remaining children
down, so stopping the rest the moment one succeeds either does nothing useful or loses sends
that had independent value — and "some failed but at least one got through" needs every
outcome, which this component discards by construction, handing back one winner. That shape
is a `when_all` over children wrapped to yield `std::expected` rather than to throw, which
is expressible today with no new component. `when_all` rethrows the lowest position's
exception at the await, which is what makes the wrapping required rather than optional.

**Where it would earn its keep, and the test for recognising a real caller:**
interchangeable attempts at *one* goal — replicas, mirrors, a hedged request — where
cancelling the rest is the point and one result is all anyone wants. Three conditions
together: the children are interchangeable, stopping the others is desirable, and exactly
one result is needed. Short of all three, the caller wants `when_all` or a plain loop.

`docs/superpowers/plans/2026-11-16-when-any.md`'s "Not in this plan" entry repeats the
superseded storage framing. It is left standing as the historical record of what was known
when that plan was written, on the same principle decision 10 applied to the combinators
spec and the stage A plan.

## What is left to the builder

- `when_any_result`'s exact spelling: the accessor's name, whether a visitor is offered
  alongside the indexed getter, and how the range form's homogeneous case is expressed
  (the same type over one value type, or a distinct pair-like one).
- Where the winner's index and the election flag live in the shared block, and the exact
  memory ordering on the CAS and its paired reads.
- Whether a losing child writes its result into a slot at all, or whether the election is
  consulted first to skip the write.
- The sentinel's representation and whether it is active in Release.
- The error code for an empty range.
- Test strategy, including: a test that two near-simultaneous completions yield exactly one
  winner; a test that no child is running when a **won** await returns (and, separately, that a cancelled await does NOT wait for teardown); a test that a throwing
  first child wins and rethrows; a test for the empty range; a mutation check per test
  asserting a race or an ordering; and a **negative control for the sentinel** — remove the
  assert, confirm the use-after-free goes unreported again, restore it.

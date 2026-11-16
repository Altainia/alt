# Design: `alt` coroutine combinators (phase 4)

**Date:** 2026-11-14 (revision 2, 2026-11-15)
**Status:** Draft for review (revision 2)
**Related:** `2026-09-10-coroutines-roadmap.md` (phase 4),
`2026-09-10-coroutine-task-and-cancellation-design.md` (phases 1-2),
`2026-10-26-async-primitives-design.md` (phase 5)

> **How this document came about.** The author asked to start phase 4 and opted out of
> the design conversation, so every call here is the designer's rather than theirs. The
> calls are collected at the end for checking.
>
> **Revision note.** Revision 1's central safety guarantee was false, and a builder check
> found it by reading the code rather than the prose. It claimed that a combinator could
> request a stop on each child and have every child gone before the teardown returned.
> Two ordinary paths break that, one of them single-threaded and deterministic; both are
> recorded below because the reasoning matters more than the outcome. Revision 1 also
> rejected the sound resolution on a cost the design was already paying N times over.

## Intent

Phases 1-3 built the coroutine, the cancellation cascade and the schedulers; phase 5 let
a coroutine wait for something that is not another coroutine. Nothing yet lets a
coroutine wait for **several things at once**. A handler fetching three resources, a UI
action that must finish before a deadline, a shutdown that waits for every outstanding
request — none of those are expressible today without hand-rolling a counter.

| Component | What it is for |
|---|---|
| `when_all` | Await several awaitables; complete when every one has. |
| `when_any` | Await several awaitables; complete when the first one has, and cancel the rest. |

## Goals

- A coroutine can await several awaitables concurrently and get every result.
- A coroutine can race awaitables and take the first, with the losers torn down rather
  than leaked or left running.
- **Cancelling a chain that is inside a combinator tears down every child**, and the
  teardown stays bounded: it must not wait on work this thread cannot drive.
- `when_any` makes timeouts expressible, which phase 5 excluded as a non-goal on exactly
  that ground: "race a completion source against a timer once `when_any` exists".

## Non-goals

- **Timers.** Still no clock source in this library. `when_any` makes a timeout
  *expressible* by racing against a caller-supplied awaitable; it does not supply one.
- **`when_all` over an infinite or streaming range.** Phase 6's `async_generator<T>` is
  where that belongs.
- **Structured concurrency enforcement.** `task_group` is the owner's teardown point; a
  combinator is an expression, not a scope.
- **Giving a cancelled loser back what it consumed.** See `when_any` below; the library
  has no mechanism for it and this phase does not invent one.
- **Alignment with C++26 `std::execution`.** Excluded by the roadmap.

## Anti-goals

- **A combinator that can leave a child writing into memory the chain has freed.**
- **A combinator that lets one child's teardown free a sibling's frame.**
- **A teardown that blocks.** Blocking deadlocks whenever a child needs the thread doing
  the blocking, which is the `run_loop` case this library exists for.
- **Amending phase 2's documented cancellation promise.** `cancellable_task` says it
  destroys its entire await chain at the next suspension point any frame reaches
  (`include/alt/cancellable_task.hpp:303-321`). A combinator must not turn that into
  "unless a combinator is in the chain, in which case it waits".

> Revision 1 also listed "no heap allocation per combinator". **Retired.** A combinator
> over N children already allocates N child coroutine frames — a child needs its own
> frame to be its own root, and the completion hook exists only on `cancellable_task`'s
> promise — plus a shared stop state. The anti-goal's cited precedent (`work_item`,
> `exception_handler`, `completion_handler` erasing handlers without allocating) is about
> type erasure, not per-operation state. Keeping it would have cost correctness to save a
> fraction of what the design already spends.

## The problem that shapes the whole design

`cancellation_context` is **linear-chain shaped**: one root, one pointer relayed down one
chain. `task_promise_base::set_cancellation_context` takes a single pointer, and
`task.hpp:270-275` relays it to the one child being awaited. Phase 5's spec named this
when it put the completion accounting in `detail/`: `task_group` and `when_all` are both
"several live children of one owner, which the single-`root` cancellation context does
not express."

### The sibling use-after-free

If a combinator relays the awaiting chain's context to all N children, a child that
reaches a suspension point with a stop pending calls `abandon_chain(ctx)`, destroying
`ctx.root` — the root of the whole chain. The cascade destroys the await temporary
holding the combinator, which owns every child. A sibling's frame is freed while that
sibling may be inside its own `await_suspend`, or sitting in a detached `waiter_list`
chain already claimed by a satisfier about to resume it. The second is worse: the
satisfier holds a raw `waiter_node*` into a frame that no longer exists.

### It would invalidate an argument four shipped components rest on

`include/alt/async_latch.hpp:58-77` recorded this a month in advance: `async_event::set`,
`async_latch::count_down`, `async_semaphore::release`, all four primitive destructors and
the `~waiter_list` backstop each resume a whole detached chain at once, and the argument
is "nothing in the library lets one waiter own a sibling's frame, because there is no
`when_all` or race combinator and a task is lazy and owns only the child it awaits."
The note names the remedy it would force — "resuming one node per detach, and not holding
the next node across a resumption" — across four shipped, heavily tested components.

## Decision 1: every child is its own root

A combinator does **not** relay the awaiting chain's context to its children. Each child
is a started `cancellable_task` whose context root is its own frame.

A builder check tried to break the resulting claim and could not, but **the reason
revision 1 gave was wrong**, and the real one has three parts that must be written down
because a plausible implementation violates any of them:

- **(A) The combinator never owns or destroys a child frame.** It only ever *requests
  stops*. Children are self-owning, so there is no destroy path from the combinator to a
  sibling at all. This forbids holding children as `alt::task`s the combinator destroys.
- **(B) The claim election cannot free an already-claimed node.** A loser whose node a
  satisfier already won loses the CAS in `waiter_list::claim`, so its
  `waiter_stop_release` resumes and frees nothing. The only way a child frame dies is the
  child itself running, which happens only when the walk reaches it.
- **(C) The combinator never *destroys* a child.** Revision 2 first wrote this as "never
  resumes the awaiting chain while any child is alive", which is stronger than the hazard
  needs and incompatible with decision 2. Resuming the chain early is safe for the sibling
  hazard precisely because of (A): nothing the chain's teardown destroys is a child frame.
  What resuming early *does* require is decision 5 below — see the correction there.

Revision 1's reason — "a child observing a stop tears down only itself" — says nothing
about a stop arriving at a sibling that is *already claimed and queued*, which is the
case that actually needed answering.

**Stage A therefore updates `async_latch.hpp`'s note to record the claim-election
argument** rather than "nothing lets one waiter own a sibling's frame". The latter stops
being literally true the moment a builder deviates from (A), and a future change to
`claim` or `resume_claimed` would silently break the real argument with nothing pointing
at it.

## Decision 2: the result slots live off the frame, refcounted

Revision 1 claimed that requesting N stops left every child gone before the teardown
returned. **That is false on two ordinary paths**, both verified against the code:

**(a) A concurrent satisfier beats the stop.** `detail::waiter_stop_release` only resumes
a child when `waiter_list::claim` returns `detached` (`include/alt/detail/async_wait.hpp:100-106`).
If a satisfier on another thread already won that node, `claim` returns `lost` and **the
stop callback does nothing**. `request_stop` returns having released nothing while the
child resumes on the satisfier's thread, writes its result and fires its completion hook.
Every one of the five child kinds revision 1 called safe documents that a waiter "resumes
on the thread that satisfied the wait, inside `set()`" — so this is the designed
behaviour, not an edge case. `task_group` survives it only because its teardown is
`request_stop()` *then* `co_await join()`; revision 1 deleted the step that made the
mechanism sound and kept the citation.

**(b) A home scheduler, single-threaded and deterministic.** `abandon_chain` *posts*
`destroy_trampoline` and returns when `ctx.home` is non-empty
(`include/alt/detail/cancellation.hpp:75-82`). So the parent root's destruction is
deferred; when it drains, the cascade destroys the combinator's frame and its slots,
and the children's own posted destructions run *afterwards*, calling `complete()` on a
destroyed counter. Reproducible with `manual_scheduler` and no second thread.

Three resolutions were available. The decision is the third below.

- **Keep the chain suspended until the count reaches zero.** Correct, and allocation-free,
  because nothing can destroy a chain that is parked. But cancellation of a chain
  containing a combinator stops being bounded, and phase 2's documented promise stops
  being true — a window's frames would stay alive until a child's uncancellable pool work
  finished. That is precisely what the roadmap names as the driving requirement of the
  whole effort, so this is **ruled out by the project's own purpose**, not merely
  disliked.
- **Narrow the precondition to what synchronous teardown actually requires.** That is not
  "no child mid-computation elsewhere" but "no child satisfiable from another thread, and
  no `home` scheduler anywhere in the chain" — which excludes `task_completion_source`
  entirely, the four primitives under concurrent use, and every UI-affine chain. It cannot
  be asserted and a violation is a silent use-after-free. **Ruled out.**
- **Refcounted shared state off the frame.** The slots, the counter and the child stop
  source live in one block the combinator's awaiter and every child hold a reference to.
  The chain can be abandoned the instant a stop is observed, bounded and non-blocking;
  children drain into state the chain no longer owns, and the last reference away frees
  it. **Adopted.**

One allocation for the shared block, against the N frames and the stop state the design
already pays. The combinator's awaiter holds a reference for as long as the chain is
parked; each child holds one until its completion hook has fired.

## Decision 3: the combinator is a bespoke awaiter, not a `task<T>`

Revision 1 left "`task<T>` or bespoke awaitable" to the builder. The safety argument
picks it. Inside an `alt::task`, `await_transform` wraps every suspension point in
`cancellation_checked`, whose `await_suspend` calls `abandon_chain` as soon as the token
is stopped (`include/alt/detail/cancellation.hpp:133-170`). So a combinator written as a
coroutine could not drain its children after a stop — the drain point would destroy the
chain instead. The combinator must be an awaiter that owns its own `std::stop_callback`
and parks the awaiting coroutine directly, outside the cancellation wrapper.

It propagates **on observe**: its stop callback requests the children's stop immediately,
and because of decision 2 it need not wait for them before letting the chain die.

## Decision 4: one stop source for the child set, and children inherit `home`

Per-child *roots* are what avoid the sibling hazard; per-child *stop sources* are
incidental — nothing in this design cancels one child individually, since the parent
cancels all and `when_any` cancels all remaining losers. One `std::stop_source` in the
shared block, with every child's `start_options::token` linked to it.

Children inherit the awaiting chain's `home`, so a child holding UI-affine state is
destroyed on the thread `start_options::home` exists to promise. Decision 2 is what makes
this safe: the shared block outlives both the parent's posted destruction and the
children's.

## Components

**`when_all`** — completes when every child has. Results are positional: a `std::tuple`
for a fixed argument list, a `std::vector` for a range, with a `void` child occupying its
position as `std::monostate` so positions stay stable.

Awaiting a `when_all` yields `std::expected<Results, std::error_code>`, with
`std::errc::operation_canceled` when the wait was released rather than satisfied — the
same satisfied-or-released shape as every phase-5 primitive. Each position inside
`Results` carries its child's value, copied out of the child before its frame dies; a
child that threw has its exception rethrown when the results are assembled, which is
`detail::result_store`'s existing behaviour.

> **Correction (revision 2, same day).** This section first said cancellation was **not**
> an observable result, on the ground that a cancelled chain never resumes. That is false
> under decision 2. Keeping the teardown bounded means the combinator's stop callback
> resumes the awaiting chain rather than waiting for the children — and
> `cancellation_checked::await_resume` is a **bare delegation** to the inner awaiter
> (`include/alt/detail/cancellation.hpp:213-216`), so nothing checks the stop at that
> resumption. The parent's body therefore runs its next statements immediately. Had the
> result carried no cancellation channel, those statements would have read unfinished
> slots. The `[[nodiscard]]` comment two lines above that delegation states the same thing
> for the primitives: for three of the four it "is the only thing standing between a
> released waiter and a body that proceeds as though the wait had succeeded." `when_all`
> is a fourth such waiter and takes the same shape.

Revision 1's error was different and also real: it said each position was
`std::expected<T, std::error_code>` *and* that exceptions were stored and rethrown. Those
are different types, and `result_store` has no `error_code` channel. The error channel
belongs on the wait as a whole, not on each position.

**`when_any`** — completes when the first child does, then requests the others' stop and
resumes the awaiting chain. It yields the **index** of the winner plus that winner's
result, rather than a variant: the index is what a caller switches on, and a variant over
N heterogeneous results is awkward to consume and worse to document.

A loser whose node a satisfier won in the same instant it was cancelled is resumed
*satisfied* and torn down at its next suspension point, discarding what it took. That is
a **resource-loss mode by construction**, not a rare race, because cancelling the losers
is `when_any`'s normal path: a semaphore permit consumed and never returned, a
`task_completion_source` value set and lost. An `async_mutex` loser is harmless, since the
guard's destructor unlocks. This is documented, not fixed; see non-goals.

## Staging

Two pull requests, as phase 5 did.

**Stage A** — `when_all`: the shared block, the fan-out, the per-child roots, the
counting, result assembly, and the `async_latch` note update recording the claim-election
argument.

**Stage B** — `when_any`: winner election (a CAS, since several children can complete
concurrently), cancel-the-losers, and the resource-loss documentation.

## What is left to the builder

- The shared block's exact layout and refcounting mechanism.
- Whether `detail::completion_counter` is used as-is or needs a "resume one handle at
  zero" path that avoids a `waiter_list` for the single-joiner case.
- How a `void` position is represented in the `vector` form.
- The child wrapper's shape, within decision 1's constraint (A).
- Test strategy, including a direct test that a cancelled combinator leaves no child
  running, a test for each of the two paths that broke revision 1, and a mutation check
  per test asserting a race or an ordering.

## Calls I made, for the author to check

1. **Every child is its own cancellation root**, with invariants (A), (B) and (C) stated
   as binding and recorded in `async_latch`'s note.
2. **Refcounted shared state off the frame**, which retires revision 1's no-allocation
   anti-goal. The alternative that keeps the chain parked was ruled out by the roadmap's
   own driving requirement, not by preference.
3. **A bespoke awaiter rather than a `task<T>`**, forced by `cancellation_checked`.
4. **One stop source for the child set; children inherit `home`.**
5. **The wait yields `std::expected<Results, std::error_code>`**, reporting
   `operation_canceled` when released, with each position carrying its child's value and a
   thrown child's exception rethrown at assembly. Corrected from this revision's own first
   draft, which claimed cancellation was unobservable; see the correction in the `when_all`
   section. No divergence from phase 5's convention after all.
6. **`when_any` yields an index plus the winner's result**, not a variant.
7. **The loser resource-loss mode is documented rather than fixed.**
8. **Two stages, `when_all` first.**

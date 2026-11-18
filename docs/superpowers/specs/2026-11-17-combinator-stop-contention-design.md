# Witnessing a combinator stop under contention — design

Issue #28. Short design: this is a small change, one new test plus note edits.

## Intent and the why

`WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock` pins both sides of
the child's node election, so a stop that is ineffective *serially* fails an equality at
zero. What it does not witness is a stop ineffective only **under contention**, and its
own note records why the obvious fix was removed: an `EXPECT_GT` over the raced phase's
wins failed about 1 run in 250, the satisfier taking a mean of 3.5 of 200 nodes under
AddressSanitizer and none at all in 2 of 500 runs.

Issue #28 offered two ways out — accept the gap, or instrument the library so
`~when_all_awaiter` could witness the overlap. Investigation narrowed the gap enough that
neither is the right answer.

## What the investigation found

The contended **election** is already covered, twice, at levels that need no rate:

- `TaskCompletionSourceTest.AStopRacingSetValueResumesTheWaiterExactlyOnce`
  (`tests/test_task_completion_source.cpp:510`) — a `task_completion_source` satisfier
  against a `request_stop()` over the same parked waiter, rendezvous-contended. The same
  two parties and the same pairing as `when_all`'s child node: `publish()` →
  `detach_one(action)` versus `waiter_stop_release` → `claim()`. Both sides genuinely win
  (setter 7–13 of 100), a mutated stop release fails it 20/20, and its standing guards
  are `resumptions == 1` and `live_frames == 0`.
- `WaiterList.AConcurrentSatisfierAndStopDoNotBothResume`
  (`tests/test_waiter_list.cpp:807`) — the CAS election itself; a broken
  compare-exchange detected 200 of 200.

So "a stop ineffective only under contention" is not open in general. What is open is
narrower: **the combinator's own relay path** — chain token → `m_link` → `cancel()` →
children's source — contended with a child's drain into the shared block
(`on_combinator_child_complete`'s `complete()` then `release()`). That path's length is
why the raced phase reaches the stop's side on 0.13% of its repeats.

`CombinatorStateTest.ARaceBetweenAStopAndTheLastReleaseIsClean`
(`tests/test_combinator_state.cpp:187`) races a stop against the last `release()`, but
the block is constructed with **zero children**, so no `complete()` is ever in flight
against a `cancel()`. That is the hole.

## Goals

- A rate-free, structural witness of a child's drain into the block contended with the
  combinator's stop walk, at the level where it can be arranged rather than sampled.
- Make the recorded gap in `tests/test_when_all.cpp` accurate: credit the coverage that
  exists and narrow what is still open.
- Close issue #28 with that record.

## Non-goals

- No assertion over the raced phase's mix in `test_when_all.cpp`. That is the measured
  flake channel the issue exists to record, and it stays recorded rather than asserted.
- No new `when_all`-level phase. The stop's relay path through a whole chain is what makes
  the collision rare there; shortening it is not possible from the test side.

## Anti-goals

- Any assertion whose pass depends on an interleaving. If a guard can fail on a timing
  shift with the library correct, it does not go in.
- Any change to `include/`. If the test cannot be written against today's public and
  `detail` surface, the work stops and comes back here.

## Approaches

1. **Document and close.** Credit the two existing tests, narrow the note, close #28. Cheap
   and honest, but leaves the relay-path overlap untested.
2. **Close it at `combinator_state` level** — chosen. A rendezvous-contended test of the
   child's completion hook against the stop walk, with the block, its counter, a parked
   joiner and a linked token, and no `when_all` involved. No library change, no macro, no
   rate. Folds in 1.
3. **Library-side instrumentation, as #28 suggested.** A compile-gated probe counting
   overlap of the two regions. The passive form still cannot be asserted without a rate,
   so it only improves failure diagnostics; making it assertable needs a rendezvous seam
   inside `waiter_list::claim`, i.e. test synchronization machinery in the most delicate
   class in the library. Costliest, buys least.

Chosen: **2, with 1 folded in.**

## The design

One new test in `tests/test_combinator_state.cpp`, named for what it races — working
title `ACancelRacingAChildsDrainIntoTheBlockIsClean` — over `race_repeats` repeats.

Per repeat, set up a block that models `when_all`'s pinned phase exactly:

- `heap_owner owner;` with `owner.state = std::make_unique<combinator_state>(1, &owner,
  &heap_owner::destroy)` — one child, so the counter stands at 2 (child plus starter) and
  the references at 1 (the awaiter's). The heap owner is required, not the `deleter_probe`:
  the block must really be freed for a race over freed memory to be observable.
- `state->add_ref()` for the child, then `state->counter().complete()` to retire the
  starter, which is the order `when_all_awaiter::await_suspend` takes after its fan-out.
  References 2, outstanding 1.
- `std::stop_source chain;` then `state->link(chain.get_token())`.
- Park a joiner: a purpose-built `alt::cancellable_task` that awaits
  `state.counter().join()`, records the outcome, and **then calls `state.release()`** —
  which is what models `~when_all_awaiter` dropping the awaiter's reference after
  `await_resume`. Started with `alt::start(..., {.token = chain.get_token()})`.

Then the race, two threads on the existing `alt_test::rendezvous`:

- **Thread A, the child's drain:** `state->counter().complete();` then `state->release();`
  — `on_combinator_child_complete`'s two statements, in the hook's order.
- **Thread B, the stop's walk:** `chain.request_stop()`.

**Callback registration order is load-bearing and is why the joiner is started with an
external token rather than from `alt::start`'s own handle.** `promise::configure`
registers its `stop_forwarder` on the external token before `handle.resume()` parks the
joiner, so on `chain` the registrations are `m_link` first and the forwarder second.
libstdc++ walks a token's callbacks in reverse registration order, so `request_stop()`
releases the joiner first and runs `cancel()` second — `when_all`'s shipped order. Taking
the token from the handle `alt::start` returns would invert it. This is the same
dependency `WhenAllTest.AHomeSchedulerDefersTheParentsDestructionPastItsChildrens` and
`TheChildrensStopOutlivesABlockTheLastChildFreed` already ship with, and the test's own
comment must say so: on a standard library that walks forward, this test arranges the
other order and the design is still sound.

### Guards

All hold whichever way the race goes. None is a rate.

- `owner.calls == 1` — the deleter ran exactly once. Neither party's release is lost and
  neither double-frees.
- `record.resumptions == 1` — the joiner was resumed exactly once. The counter's zero
  transition and the stop's claim contend for it; exactly one may win.
- `live_frames == 0` — the joiner's frame is gone.
- The verdict is one of the two legal ones: satisfied, or released with
  `std::errc::operation_canceled`. Which one happens is the race, so both are legal.
- AddressSanitizer and ThreadSanitizer over all of it. This is where a `cancel()` that
  stored its flag into a block the drain had already freed would report, and it is the
  reason the heap owner is used.

### What it catches that nothing else does

The block is freed by whichever of the two releases is last, with `cancel()` possibly
still to run on `chain`'s walk. That is the hazard `combinator_state::cancel()`'s DELICATE
note is about — its counted copy of the children's stop state, and the flag-before-stop
order — reached with a child's `complete()` in flight, which no test in the suite does
today. The mutation of record to measure against is swapping `cancel()`'s two statements,
which the existing note measures as a heap-use-after-free under AddressSanitizer through
`when_all`; this test should reach it directly.

Per CLAUDE.md's "Measure before claiming", every figure that ends up in the test's comment
is to be taken by building the mutated variant, not predicted.

### The note edits (approach 1, folded in)

In `tests/test_when_all.cpp`, in the "What no phase checks" note around line 2416 and in
the `satisfier_handshake` note's contention paragraph:

- Credit `TaskCompletionSourceTest.AStopRacingSetValueResumesTheWaiterExactlyOnce` and
  `WaiterList.AConcurrentSatisfierAndStopDoNotBothResume` as covering the contended
  election, with the same parties.
- Narrow the recorded gap from "a stop ineffective only under contention" to the relay
  path, and name the new `CombinatorStateTest` as what covers it.
- Keep the `EXPECT_GT` cautionary tale and its measured flake rate. That is why the gap is
  recorded rather than asserted, and it does not change.
- Update the issue-#28 reference to say the issue is closed and what closed it.

Then close #28 with a comment carrying that record.

## Constraints

- No change to anything under `include/`. The test must be writable against today's
  surface.
- Eight CI gates, all of which must be run locally: GCC, Clang, optimized,
  ASan+UBSan, TSan, clang-tidy (pinned `clang-tidy-19`, CI's own invocation), cppcheck,
  Coverage. A sweep that omits one is not a green branch.
- `tests/.clang-tidy` is appended to, never rewritten. Expect a
  `readability-function-cognitive-complexity` finding on a race body with repeats; the
  existing race tests show how that is handled.
- `race_repeats` in `test_combinator_state.cpp` is already 200. Reuse it.

## What is left to the builder

- The test's final name.
- Whether the per-repeat outcome mix is recorded with `RecordProperty`, as
  `AStopRacingSetValueResumesTheWaiterExactlyOnce` records which side won. My call: yes,
  and additionally assert nothing about it.
- Whether the joiner coroutine is a new local helper in `test_combinator_state.cpp` or a
  shared one. My call: local, since its `state.release()` tail is specific to this test.
- Exact wording of the `test_when_all.cpp` note edits, within the substance above.

## Version bump

Decided: **patch, 1.12.2 to 1.12.3**. A test-and-comments change ships no interface or
inline-size change. Its own commit on the branch, all six files CLAUDE.md lists.

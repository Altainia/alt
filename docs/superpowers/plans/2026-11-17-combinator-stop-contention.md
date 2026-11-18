# Combinator Stop Contention Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close issue #28 by witnessing a combinator's stop walk contended with a child's drain into the shared block, at `combinator_state` level, and by correcting what `tests/test_when_all.cpp` records as open.

**Architecture:** One new rendezvous-contended test in `tests/test_combinator_state.cpp` that reproduces `when_all`'s pinned-phase arrangement with no `when_all` involved: a heap-owned block with one child, a cancellable joiner parked on its counter whose body drops the awaiter's reference, one thread running `on_combinator_child_complete`'s two statements and the other running `request_stop()` on a linked `std::stop_source`. All guards are structural — exactly one deleter call, exactly one joiner resumption, no live frames, a satisfied-or-cancelled verdict — so none can fail on a timing shift. Then `tests/test_when_all.cpp`'s notes are corrected to credit the coverage that already exists and to narrow the gap to what this test now covers.

**Tech Stack:** C++23, GoogleTest, CMake presets, libstdc++ `<stop_token>`.

**Spec:** `docs/superpowers/specs/2026-11-17-combinator-stop-contention-design.md`

## Global Constraints

- **No change to anything under `include/`.** If the test cannot be written against today's public and `detail` surface, stop and report rather than adding a seam.
- Branch `feature/combinator-stop-contention` off `main`. `main` is never pushed to directly. Do not merge until explicitly asked.
- All eight CI gates run locally before the PR: GCC, Clang, optimized (`release-tests`), ASan+UBSan, TSan, `clang-tidy`, `cppcheck`, Coverage. A sweep that omits one is not a green branch.
- `clang-tidy` must be CI's own invocation, flag for flag, pinned to `clang-tidy-19` and `clang++-19`. See CLAUDE.md's clang-tidy section.
- `tests/.clang-tidy` is appended to, never rewritten.
- Naming: `snake_case` throughout; `m_` prefix on private and protected members.
- `const` by default on locals unless mutation is required.
- American spelling, except the approved double-l `cancelled`/`cancellable`/`cancellation` forms.
- Every figure that lands in a comment is **measured, not predicted**: build the mutated variant and take the number. Never write an expected number ahead of the run.
- Version bump: patch, **1.12.2 to 1.12.3**, as its own commit on the branch, all six files CLAUDE.md lists.
- Commit messages end with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. PR descriptions end with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.

## Review Focus

Five things the spec implies that no task's assertions would otherwise pin. Each has a step in the task that owns it.

1. **One side may win every repeat,** which would make the test exercise half of what it claims. `AStopRacingSetValueResumesTheWaiterExactlyOnce` measures its own mix (setter 7–13 of 100) for exactly this reason. Task 1, Step 6 measures the mix and records it; Task 1, Step 7 records it at runtime with `RecordProperty`.
2. **A standard library that walks stop callbacks in registration order** would silently invert the arrangement: the test would witness `cancel()`-then-joiner instead of joiner-then-`cancel()`, pass, and claim coverage it does not have. Task 1, Step 8 writes that dependency into the comment, naming the two existing `WhenAllTest` cases that already carry it.
3. **A heap-use-after-free report under AddressSanitizer is a library finding, not a test bug.** The block really is freed by whichever release lands last, with `cancel()` possibly still pending on the walk. Task 1, Step 5 says to stop and report rather than weaken the test if that happens.
4. **`live_frames` is process-global and `CombinatorStateTest` has no `SetUp` that resets it.** Task 1, Step 3 resets per repeat inside the test body rather than changing the fixture, so the file's other tests are untouched.
5. **The test's cost under the sanitizers.** 200 repeats, each allocating a block, starting a coroutine and spawning two threads; a gate that takes minutes reads as a hang. Task 1, Step 9 records the measured wall-clock time per preset and drops `race_repeats` for this test only if it exceeds the slowest existing race test in the file by more than roughly 2x.

---

### Task 1: The contended drain-against-stop test

**Files:**
- Modify: `tests/test_combinator_state.cpp` — add includes, local helpers, and one test
- Modify: `tests/.clang-tidy` — only if a new suppression proves necessary, appended

**Interfaces:**
- Consumes: `alt::detail::combinator_state` (`add_ref`, `release`, `counter`, `link`, `cancelled`); `alt::detail::completion_counter::join`; `alt::start(alt::cancellable_task, const alt::start_options&)`; `alt_test::rendezvous`, `alt_test::frame_tracker`, `alt_test::live_frames`, `alt_test::reset_coroutine_test_state`; the file's existing `heap_owner` and `race_repeats`.
- Produces: nothing other tasks consume.

- [ ] **Step 1: Add the includes and the local helpers**

Add to `tests/test_combinator_state.cpp`'s include block, in the file's existing order: `<alt/awaitable.hpp>`, `<alt/cancellable_task.hpp>`, `<system_error>`.

In the file's anonymous namespace, add — modelled on `tests/test_completion_counter.cpp:27-60`, which has the same three pieces:

```cpp
/** The condition every released wait reports. See alt::wait_result. */
constexpr std::errc released_error = std::errc::operation_canceled;

/** What the joiner recorded about its wait, for the test to inspect. */
struct wait_record
{
    /** How many times the statement after the co_await ran. Two is a double resume. */
    int resumptions = 0;

    /** Whether the join reported success. */
    bool satisfied = false;

    /** The error a released join reported. */
    std::error_code error;
};

/** Records one join's outcome. */
void record_outcome(wait_record& record, const alt::wait_result& result);
```

Then the joiner itself. Its `release()` tail is the whole point and needs a Doxygen comment saying so:

```cpp
/**
 * @brief A joiner that drops the block's awaiter reference after its wait ends.
 *
 * Models when_all's awaiting chain: the co_await's awaiter -- and with it the
 * joiner's own std::stop_callback -- is destroyed at the end of the full
 * expression, and only then is the reference dropped, which is
 * ~when_all_awaiter's order.
 */
alt::cancellable_task draining_joiner(combinator_state& state, wait_record& record, int tag);
```

Body: a `frame_tracker` on `tag`, then `record_outcome(record, co_await state.counter().join());`, then `state.release();`. It needs the same `cppcoreguidelines-avoid-reference-coroutine-parameters` NOLINTNEXTLINE the sibling helpers in `test_completion_counter.cpp` carry, with a reason naming who owns the block and the record.

- [ ] **Step 2: Write the test, and make the arrangement a helper**

Add `TEST_F(CombinatorStateTest, ACancelRacingAChildsDrainIntoTheBlockIsClean)`. Keep the per-repeat body in a file-local helper taking the repeat index and a tally struct, the way `run_one_refcount_race` factors this file's other race: the test body is then a loop plus the assertions, which keeps `readability-function-cognitive-complexity` off it.

Per repeat, in this order:

1. `reset_coroutine_test_state();`
2. `heap_owner owner;` and `owner.state = std::make_unique<combinator_state>(1, &owner, &heap_owner::destroy);` — one child, so the counter stands at 2 and the references at 1.
3. `combinator_state* const state = owner.state.get();`
4. `state->add_ref();` — the child's reference, taken before the child starts, as `combinator_state`'s class note requires.
5. `state->counter().complete();` — retires the starter, which is what `when_all_awaiter::await_suspend` does after its fan-out. Outstanding is now 1.
6. `std::stop_source chain;` then `state->link(chain.get_token());` — **first** registration on `chain`.
7. `wait_record record;` and `const auto handle = alt::start(draining_joiner(*state, record, 1), {.token = chain.get_token()});` — `promise::configure` registers its forwarder on `chain` as the **second** registration, before `resume()` parks the joiner. Assert `live_frames == 1` and `record.resumptions == 0` here, so a repeat whose joiner failed to park reports that rather than the race's guards.
8. The race, on `alt_test::rendezvous` with `std::atomic<std::size_t> arrived{0}` and two parties:
   - the drain thread: `rendezvous(arrived, 2);` then `state->counter().complete();` then `state->release();` — `on_combinator_child_complete`'s two statements in the hook's order;
   - this thread: `rendezvous(arrived, 2);` then `chain.request_stop();`
   Use `std::thread` with an explicit `join()`, as `ARaceBetweenAStopAndTheLastReleaseIsClean` does, not `std::jthread`, so the join is ordered before the assertions.

Assertions after the join, each with `<< "repeat " << repeat`:

```cpp
ASSERT_EQ(record.resumptions, 1);
ASSERT_EQ(owner.calls.load(std::memory_order_relaxed), 1);
ASSERT_EQ(live_frames, 0);

if(!record.satisfied)
{
    ASSERT_EQ(record.error, released_error);
}
```

A satisfied verdict needs no further check — it says the counter reached zero — so only the released path asserts its error, which is `expect_value_or_released`'s shape in `tests/test_task_completion_source.cpp:105`. Tally which of the two verdicts the repeat took, for Step 7.

**Nothing may read the block after the join.** `owner.calls == 1` says it has been freed, so `state->cancelled()` — the obvious extra assertion — is a use-after-free and must not be written. If the cancelled flag is worth observing, the joiner's body is the only place with a live block to read it from, and it would have to be captured into `wait_record` there; that is a decision for Step 2's implementer, and the default is not to.

- [ ] **Step 3: Run it in the debug preset**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R CombinatorStateTest.ACancelRacingAChildsDrainIntoTheBlockIsClean
```

Expected: PASS. A failure here is a finding — record which assertion fired and stop rather than loosening it.

- [ ] **Step 4: Run it under both sanitizers**

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R CombinatorStateTest.ACancelRacingAChildsDrainIntoTheBlockIsClean
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R CombinatorStateTest.ACancelRacingAChildsDrainIntoTheBlockIsClean
```

Expected: PASS under both.

- [ ] **Step 5: If either sanitizer reports, stop and report**

A heap-use-after-free or a data race here is a claim about the shipped library, not about the test: the arrangement is `when_all`'s pinned phase, and `combinator_state::cancel()`'s DELICATE note and `m_link`'s declaration-order note are what are supposed to make it safe. Capture the full report, say which note's argument it contradicts, and bring it back. **Do not weaken the test to make it pass.**

- [ ] **Step 6: Measure the mutation this test is for**

The mutation of record is swapping `cancel()`'s two statements in `include/alt/detail/combinator_state.hpp` — the flag store and the `request_stop()` on the counted copy. Apply it in a scratch copy of the tree, build `debug` and `debug-asan`, and run this test alone 20 times in each. Record the detection count and which assertion or sanitizer reported. Revert the mutation.

Also measure the mix, for Review Focus item 1: over three runs of the unmutated test, how many of the `race_repeats` repeats ended satisfied and how many released. If one side takes every repeat in all three runs, say so in the comment rather than claiming the race reaches both.

- [ ] **Step 7: Record the mix at runtime**

Add `RecordProperty` calls for the two verdict tallies, asserting nothing about them, so a reader of a failure sees which mix the failing run saw. This is `test_when_all.cpp`'s own convention for a number that is a sample rather than a guard.

- [ ] **Step 8: Write the comment**

A comment block above the test carrying, in the file's voice:

- What the two parties are and that they are the hook's two statements against the stop's walk.
- **The registration-order dependency**, and why the joiner takes an external token rather than the handle `alt::start` returns: `promise::configure` registers its forwarder before `resume()` parks the joiner, so `m_link` is first on `chain` and the forwarder second, and libstdc++'s reverse walk therefore releases the joiner before running `cancel()` — `when_all`'s shipped order. Name `WhenAllTest.AHomeSchedulerDefersTheParentsDestructionPastItsChildrens` and `TheChildrensStopOutlivesABlockTheLastChildFreed` as already carrying the same dependency, and say that on a forward-walking standard library this test arranges the other order with the design still sound.
- Why the heap owner rather than `deleter_probe`: the block must really be freed for a race over freed memory to be instrumented.
- Why the guards are the ones they are, and that none is a rate.
- What `CombinatorStateTest.ARaceBetweenAStopAndTheLastReleaseIsClean` does not cover that this does: that block is constructed with zero children, so no `complete()` is ever in flight against a `cancel()`.
- The Step 6 figures, each with the preset and the date, per CLAUDE.md's rule that a measured figure can go stale.

- [ ] **Step 9: Record the cost**

Time this test alone in `debug`, `debug-asan` and `debug-tsan`, and compare with `ConcurrentAddRefAndReleaseRunTheDeleterExactlyOnce` in the same file. If it is more than roughly twice the slower of the two, reduce this test's repeat count to its own constant rather than lowering `race_repeats` for the file, and note the measured times as the reason.

- [ ] **Step 10: Format, then run the file's whole suite in all four presets**

```bash
clang-format -i tests/test_combinator_state.cpp
```

Then, in each of `debug`, `debug-asan`, `debug-tsan` and `release-tests`:

```bash
ctest --preset <preset> --output-on-failure -R 'CombinatorStateTest|CompletionCounterTest|WaiterList|WhenAllTest'
```

Expected: PASS in all four.

- [ ] **Step 11: Commit**

```bash
git add tests/test_combinator_state.cpp tests/.clang-tidy
git commit
```

Message: `Witness a combinator stop contended with a child's drain`, with a body saying what the test arranges, what it guards, and the Step 6 detection figures.

---

### Task 2: Correct what test_when_all.cpp records as open

**Files:**
- Modify: `tests/test_when_all.cpp` — the `satisfier_handshake` note's contention paragraph (around lines 1418-1432) and the "What no phase checks" note (around lines 2371-2430)

**Interfaces:**
- Consumes: the test name added in Task 1.
- Produces: nothing.

- [ ] **Step 1: Credit the coverage that already exists**

In the `satisfier_handshake` note's paragraph beginning "What the free-running phase is still here for", and in the "Neither does this phase say anything about the stop under CONTENTION" paragraph above `EXPECT_EQ(stop_pinned.satisfier_wins, 0)`, add that the contended **election** is covered elsewhere and is not what is open here:

- `TaskCompletionSourceTest.AStopRacingSetValueResumesTheWaiterExactlyOnce` — the same two parties and the same pairing as this child's node, `publish()` → `detach_one(action)` against `waiter_stop_release` → `claim()`, rendezvous-contended, both sides measured to win, guards that are a resumption count and `live_frames` rather than a rate.
- `WaiterList.AConcurrentSatisfierAndStopDoNotBothResume` — the CAS election itself.

- [ ] **Step 2: Narrow the recorded gap and name what now covers it**

Rewrite the "Witnessing it means instrumenting the library side" sentence at `tests/test_when_all.cpp:2416`. It should now say that the overlap is witnessed, at `combinator_state` level rather than from here, by the Task 1 test — the child's drain against the stop's walk, arranged rather than sampled — and that what remains out of this test's scope is observing that overlap **through `when_all`'s own relay path**, whose length is why the raced phase reaches the stop's side on 0.13% of its repeats.

Keep, unchanged in substance: the retired `EXPECT_GT`'s measured flake rate (a mean of 3.5 of 200 under AddressSanitizer, none at all in 2 of 500 runs, about 1 run in 250), the "arranged but unwitnessed is worth more than an assertion watching the wrong variable" position as the reason nothing here is asserted, and every recorded figure.

- [ ] **Step 3: Update the issue reference**

The note at `tests/test_when_all.cpp:2423-2424` says the contended case "is tracked as issue #28". Change it to say #28 is closed and by what, keeping the pointer to the measured flakiness that retired the bound.

- [ ] **Step 4: Verify no claim went stale**

Per CLAUDE.md's "Fixing a claim": sweep for every other place in the tree that generalizes over this gap, by claim rather than by phrase. At minimum grep for `#28`, `contention`, `contended`, `unwitnessed` and `arranged but` across `tests/`, `include/` and `docs/`, and check each hit against what Task 1 changed.

```bash
grep -rn "#28\|contend\|unwitnessed\|arranged but" tests/ include/ docs/
```

- [ ] **Step 5: Format and run the affected tests**

```bash
clang-format -i tests/test_when_all.cpp
ctest --preset debug --output-on-failure -R 'WhenAllTest'
```

Expected: PASS. Comment-only edits, so a failure means a stray code change.

- [ ] **Step 6: Commit**

```bash
git add tests/test_when_all.cpp
git commit
```

Message: `Narrow the stop-contention gap test_when_all records`, with a body naming the two tests now credited and what is still out of scope.

---

### Task 3: Version bump

**Files:**
- Modify: `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

- [ ] **Step 1: Bump 1.12.2 to 1.12.3 in all six files**

Per CLAUDE.md's Version bump table. In `README.md` and `CLAUDE.md`, move the project version occurrences and adjust the `find_package` compatibility example's operands so the prose still agrees with the snippet above it — those operands illustrate the rule, not this release.

- [ ] **Step 2: Run the version test**

```bash
cmake --build --preset debug && ctest --preset debug --output-on-failure -R VersionTest
```

Expected: PASS.

- [ ] **Step 3: Commit on its own**

```bash
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.12.3"
```

The message is exactly that line plus the attribution. Nothing else goes in this commit.

---

### Task 4: Full gate sweep and pull request

**Files:**
- No source changes expected. Any fix a gate forces is committed on its own.

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: the pull request.

- [ ] **Step 1: Run all eight gates**

Each of the eight, in full, with the exact invocations in CLAUDE.md's Quality checks section: `debug` (GCC), a Clang configure and test, `release-tests`, `debug-asan`, `debug-tsan`, the pinned `clang-tidy-19` build, `cppcheck`, and the `debug-coverage` `coverage` target. Capture each one's final status line. Do not read a gate through a truncated capture or an unrebuilt binary.

Note on Coverage, from CLAUDE.md: the `coverage` target runs the suite itself, so no `ctest` step precedes it, and against a reused build directory it prints `libgcov profiling error: ... overwriting an existing profile data with a different checksum` lines that are harmless. Issue #32 carries the measurements.

- [ ] **Step 2: Report the sweep**

State each of the eight by name with its result, and say plainly if any was skipped or failed. A sweep that omitted one is not a green branch however it reports itself.

- [ ] **Step 3: Open the pull request as a draft, then mark it ready**

```bash
git push -u origin feature/combinator-stop-contention
gh pr create --draft --base main --title "Witness a combinator stop contended with a child's drain"
```

A bare branch push runs no CI; a draft PR fires all eight jobs. Body: what #28 asked, what the investigation narrowed it to, what the new test arranges and guards, what the note edits correct, and `Closes #28` so the merge closes the issue. End with the `🤖 Generated with [Claude Code]` line.

- [ ] **Step 4: Wait for CI, then report**

```bash
gh pr checks --watch
```

Report each job's result. Do not report green from a watcher whose failure path is silent — confirm the eight job names individually.

- [ ] **Step 5: Stop**

Do not merge. CLAUDE.md: merging waits until explicitly asked.

# Waiter Liveness Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the use-after-free class that produced stage A's only Critical finding detectable by the test suite, and close it at its source in `detail::wait_awaiter`.

**Architecture:** `detail::waiter_list` gains an atomic liveness pattern, set by the constructor, overwritten by the destructor before it drains, and checked by every public member that takes the list's mutex. `detail::wait_awaiter::await_suspend` resets its stop callback on the refused-link path, so a refused link leaves no callback in existence. Nothing else changes: no public API, no behavior on any path that was already correct.

**Tech Stack:** C++23, header-only, GoogleTest, CMake presets.

**Spec:** `docs/superpowers/specs/2026-11-15-when-any-design.md` — decisions 8 and 9, and the Staging section (this plan is PR 1 of two; `when_any` is PR 2 and gets its own plan).

## Global Constraints

- C++23; GCC 13+ or Clang 19+.
- `snake_case` throughout; `PascalCase` template typenames; `m_` on private and protected members; `const` by default; Doxygen `/** */` on every public symbol, updated when a symbol's meaning changes.
- American spelling, except the approved `cancelled`/`cancellable`/`cancellation` forms.
- `tests/.clang-tidy` is appended to, never rewritten.
- **Every task runs all eight CI gates before its commit**, not just the one whose build is quickest: GCC debug, Clang-19 debug, `release-tests`, ASan+UBSan, TSan, the whole-project `clang-tidy` gate pinned to `/usr/bin/clang-tidy-19` with `clang-19`/`clang++-19` and `CMAKE_BUILD_TYPE=Debug`, cppcheck, and Coverage with its `coverage` target. These are header-only changes to files that six shipped components include, so there is no such thing as a locally scoped change here.
- **Delete each test binary and confirm its md5 changed before believing any `ctest` output.** This branch's predecessor had three stale-binary incidents across three different agents. Grep for `tests passed|tests failed`; do not tail.
- **A mutation measurement records pass, fail and hang separately.** Wrap each repeat in `timeout -s KILL` and note that `timeout(1)` exits **124** when it kills the child, not 137.
- Work reaches `main` through a pull request; `main` is never pushed to directly.

## Review Focus

Five failure modes the spec implies that no task's happy path exercises. Each has its test named in the owning task.

1. **A `claim` concurrent with `~waiter_list`, rather than after it.** The sentinel is stored before the drain precisely so this is caught, but the interleaving is not deterministically reachable from a test. Task 1 documents it as guarded-but-unexercised rather than claiming coverage.
2. **The guard is absent in Release.** `assert` compiles away under `NDEBUG`, so a use-after-free reachable only in an optimized build stays as invisible as it is today. Task 1 states this in the comment rather than leaving a reader to assume otherwise.
3. **A refused link whose stop callback is already mid-flight on another thread.** `m_stop.reset()` must block until it returns; if it did not, the reset would be the hazard rather than the fix. Task 2 tests it.
4. **A refused link on a token that was already stopped before the wait began.** The callback fires inline during `emplace`, so the reset runs against an already-completed invocation. Task 2 tests it.
5. **The two suite comments asserting that no death tests exist.** Task 1 adds the first one, which makes `tests/test_task_group.cpp:29-31` and `tests/test_task_completion_source.cpp:40-42` false as written. Task 3 corrects them.

---

### Task 1: The liveness sentinel in `waiter_list`

**Files:**
- Modify: `include/alt/detail/waiter_list.hpp` (constructor, destructor at `:254-257`, every public member that locks `m_mutex`, private members at `:684-688`)
- Test: `tests/test_waiter_list.cpp`

**Interfaces:**
- Produces: nothing public. `waiter_list`'s API is unchanged; the sentinel is private and the checks are `assert`s.

- [ ] **Step 1: Write the failing death test**

First death test in this suite — see Task 3 for the two comments that must stop claiming otherwise. Single-threaded by construction, so `fork` is safe here.

```cpp
TEST(WaiterListDeathTest, ClaimingAWaiterOnADestroyedListAborts)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");

	std::optional<alt::detail::waiter_list> list{std::in_place};
	alt::detail::waiter_node                node;

	list.reset();

	EXPECT_DEATH(static_cast<void>(list->claim(node)), "claiming an alt waiter on a destroyed list");
}
```

The `std::optional` is deliberate: `reset()` ends the list's lifetime while leaving its storage intact, which is the destroyed-but-not-yet-freed window a test can actually reach. Reaching the freed-and-returned window needs timing no rendezvous can pin, which is what stage A established.

Add the companion skip guards in the same file, as named constants rather than inline magic: the test is meaningless under `NDEBUG` (the assert is gone) and death tests fork, which ThreadSanitizer does not support. `GTEST_SKIP()` with a stated reason under both.

- [ ] **Step 2: Write the second death test, for `link`**

```cpp
TEST(WaiterListDeathTest, LinkingAWaiterOnADestroyedListAborts)
```

Same shape, asserting on "linking an alt waiter on a destroyed list". Two entry points rather than one, because `claim` and `link` are reached by different parties — a stop callback and a waiter — and a guard on only one of them would leave the other silent.

- [ ] **Step 3: Run both to verify they fail**

Run: `ctest --preset debug -R WaiterListDeathTest --output-on-failure`
Expected: FAIL — no abort happens, because no guard exists yet.

- [ ] **Step 4: Add the sentinel and its checks**

In `include/alt/detail/waiter_list.hpp`:

- Two private constants: `alive_pattern = 0x57414954U` and `dead_pattern = 0xDEAD1157U`.
- A private member `std::atomic<std::uint32_t> m_liveness{alive_pattern}`, declared **first** among the members so it is destroyed last.
- A private `[[nodiscard]] bool alive() const noexcept` returning `m_liveness.load(std::memory_order_relaxed) == alive_pattern`. **Compare against `alive_pattern`, not against `dead_pattern`**: freed storage that has been reused holds neither, and only the positive test rejects it.
- The member exists in every configuration; only the checks are `assert`-guarded. Making the member conditional on `NDEBUG` would change the class layout between translation units compiled with different flags, which is an ODR violation waiting to happen, and the cost is four bytes.
- An `assert(alive() && "<verb>ing an alt waiter on a destroyed list")` at the top of each public member that takes `m_mutex` — both `link` overloads, both `detach_all` overloads, both `detach_one` overloads, `claim`, `size` and `empty` — placed **before** the lock, since the lock is the thing that touches freed memory.

- [ ] **Step 5: Rewrite the destructor so the sentinel is stored before the drain**

The destructor currently calls the public `detach_all()`, which would trip its own new assert. It must store `dead_pattern` **first** and then drain through the private `detach_all_locked()` under its own lock:

```cpp
~waiter_list()
{
	// Stored before the drain, not after: a claim racing this teardown is the hazard
	// the precondition names, and a sentinel set afterwards would miss exactly that
	// interleaving while catching only the easier post-destruction one.
	m_liveness.store(dead_pattern, std::memory_order_relaxed);

	waiter_node* chain = nullptr;
	{
		const std::scoped_lock lock{m_mutex};
		chain = detach_all_locked();
	}
	resume_claimed(chain, false);
}
```

- [ ] **Step 6: Run the death tests to verify they pass**

Run: `ctest --preset debug -R WaiterListDeathTest --output-on-failure`
Expected: PASS, both.

- [ ] **Step 7: Measure the end-to-end claim — stage A's regression test now bites**

This is the step that establishes the sentinel is worth having, and it must be measured rather than argued. `WhenAllTest.AStopRacingTheEndOfAnAwaitWhoseJoinerNeverLinked` currently **cannot fail** on the mutation it exists for: stage A measured 0 failures and 0 hangs in 20 repeats per sanitizer preset with the defect present.

Revert stage A's fix — move `m_join.reset()` in `~when_all_awaiter` (`include/alt/when_all.hpp`) back below the `release()` call — rebuild, and run that test 20 times in each of debug, ASan and TSan. Record pass, fail and hang counts separately. Restore the fix afterward and confirm the tree is clean.

A zero here is a reportable result, not a reason to go looking for a different mutation: it would mean the sentinel does not reach the real hazard, which is a finding about this task's whole premise and must be surfaced rather than worked around.

- [ ] **Step 8: Correct `waiter_list`'s own notes that cite AddressSanitizer as evidence**

Several notes in this file rest on an ASan report. Stage A established that a use-after-free whose only contact with the freed object is a mutex lock has **no instrumented access on its path**: ASan does not intercept glibc's `pthread_mutex_lock`, libstdc++'s `std::mutex` destructor is trivial so no `pthread_mutex_destroy` is intercepted, and ThreadSanitizer drops its synchronization variables on free. Those notes hold only where something instrumented also touches the freed list. Say so where they appear, and point at the sentinel as what now covers the rest.

- [ ] **Step 9: Run all eight gates, then commit**

```bash
git add include/alt/detail/waiter_list.hpp tests/test_waiter_list.cpp
git commit -m "Guard waiter_list against use after its own destruction"
```

---

### Task 2: Reset the stop callback on `wait_awaiter`'s refused-link path

**Files:**
- Modify: `include/alt/detail/async_wait.hpp` (the refused-link tail of `await_suspend`, `:275-284`, and the class note at `:221-229`)
- Create: `tests/test_async_wait.cpp` — `detail::wait_awaiter` has no test file of its own today, being exercised only through the primitives that derive from it, and these three tests are about the base rather than any one primitive
- Modify: `tests/CMakeLists.txt` (add the new file to the `alt_tests` sources list, per the project's test convention)

**Interfaces:**
- Consumes: Task 1's sentinel, which is what makes this task's tests able to fail at all.
- Produces: nothing public. `wait_awaiter`'s API and its behavior on every path that was already correct are unchanged.

- [ ] **Step 1: Write the failing test for the ordinary refused link**

A long-lived awaiter whose link is refused, whose primitive then dies, and whose token is then stopped. Without the reset, the still-registered `waiter_stop_release` runs `claim` on the destroyed list and Task 1's sentinel aborts; with it, nothing is registered and the stop does nothing.

```cpp
TEST(AsyncWaitTest, ARefusedLinkLeavesNoCallbackBehindToClaimADeadList)
```

The awaiter must outlive the `co_await` expression for the window to exist at all, which is why a plain `co_await event` cannot express this and stage A's `when_all` could: hold the awaiter as a member of a test-local type, drive `await_suspend` directly with a `cancellable_task`'s handle so the token is stoppable, and destroy the primitive before requesting the stop.

- [ ] **Step 2: Write the failing test for an already-stopped token**

```cpp
TEST(AsyncWaitTest, ARefusedLinkOnAnAlreadyStoppedTokenLeavesNothingRegistered)
```

Review Focus 4: the callback fires inline during `emplace`, so the reset runs against an invocation that has already completed. Asserts the same end state, reached by a different route.

- [ ] **Step 3: Write the failing test for a callback mid-flight on another thread**

```cpp
TEST(AsyncWaitTest, ARefusedLinkWaitsForAStopCallbackAlreadyRunningElsewhere)
```

Review Focus 3: `~stop_callback` blocks for an invocation on another thread, and the reset depends on that. Build it with a rendezvous — the stopping thread inside the callback, the awaiting thread entering the refused-link tail — and assert the reset did not return until the callback had. If no rendezvous can pin this without instrumenting the library, say so and assert the weaker end state instead; an honest "this test observes the outcome but not the ordering" beats a test whose name claims more than it checks.

- [ ] **Step 4: Run all three to verify they fail**

Run: `ctest --preset debug -R AsyncWaitTest --output-on-failure`
Expected: FAIL — the first two by the sentinel's abort, the third per its own assertion.

- [ ] **Step 5: Add the reset**

In `await_suspend`'s refused-link tail, `m_stop.reset()` **before** `m_parked` and `m_released` are written. Reset first so that no callback can be mid-flight over the node or the list while those writes happen, which is what the existing note at the refusal says cannot be assumed. `reset()` on a disengaged optional is a no-op, so the path where no callback was registered needs no branch.

No deadlock is possible here: `link` takes and releases the list's mutex within its own call, so this thread holds no lock when it blocks on an in-flight callback that wants that mutex.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `ctest --preset debug -R AsyncWaitTest --output-on-failure`
Expected: PASS, all three.

- [ ] **Step 7: Measure the mutation**

Delete the `m_stop.reset()` line, rebuild, and run each of this task's three tests 20 times in debug, ASan and TSan. Record pass, fail and hang separately. Restore and confirm the tree is clean. Note which test catches it and which do not: the per-test attribution is the part worth keeping, and stage A's record shows a glibc abort can mask it.

- [ ] **Step 8: Update the class note**

The note at `:221-229` explains that `m_stop` is declared after `m_node` so the callback cannot be mid-flight over a dead node, and refers a reader to `cancellable_task`'s promise for a hazard removed structurally. Record that the refused-link path is now removed structurally too, and that declaration order remains load-bearing for the **linked** path, where the callback legitimately exists while the waiter is parked. Name stage A's `~when_all_awaiter` as the case where that still matters.

- [ ] **Step 9: Run all eight gates, then commit**

```bash
git add include/alt/detail/async_wait.hpp tests/test_async_wait.cpp tests/CMakeLists.txt
git commit -m "Deregister a refused link's stop callback before returning"
```

---

### Task 3: Make the surrounding record true

**Files:**
- Modify: `tests/test_task_group.cpp:29-31`, `tests/test_task_completion_source.cpp:40-42` (the two "no death tests" comments)
- Modify: `include/alt/detail/combinator_state.hpp`, `include/alt/when_all.hpp` (notes citing AddressSanitizer as evidence)
- Modify: `docs/when_all.md` if it makes the same citation

**Interfaces:** documentation only; no API or behavior change.

- [ ] **Step 1: Correct the two death-test comments**

Both say the suite "has no death tests (no `EXPECT_DEATH` or `ASSERT_DEATH` anywhere)" and that introducing the first one "would be a larger change than this component". Task 1 made the first clause false and the second obsolete. Rewrite each to state the real residual gap: the asserts they name are still unexercised, and the reason is now that nobody has written those tests, not that the mechanism is unavailable. Do **not** add death tests for those asserts here — see **Not in this plan**.

- [ ] **Step 2: Correct the ASan-citing notes outside `waiter_list`**

`combinator_state.hpp`'s `cancel()` note cites an ASan `heap-use-after-free` as its evidence, and that citation is **sound** — libstdc++ relocks the stop state, which ASan does instrument — so it stays, with one clause saying why it holds where the `waiter_list` ones did not. Any note that cites ASan for a hazard whose only contact is a mutex lock is the opposite case and must be corrected. Check each citation rather than rewriting them as a class.

- [ ] **Step 3: Verify no shipped comment now carries a stale count or claim**

Grep the tree for test counts and for `EXPECT_DEATH`/`ASSERT_DEATH` claims. Task 1 and Task 2 each added tests, so any comment stating a total is now wrong. This project has shipped a stale count three times; the grep is cheaper than the fourth.

- [ ] **Step 4: Run all eight gates, then commit**

```bash
git commit -am "Make the branch's sanitizer and death-test notes true as written"
```

---

### Task 4: Version bump

**Files:** all six that `CLAUDE.md` names.

- [ ] **Step 1: Bump to 1.10.1**

> **Superseded:** executed as **1.11.0** (minor), see the commit message. `sizeof(detail::waiter_list)` grows by the sentinel, and five `src/*.cpp` compile out-of-line definitions of types that embed it into `libalt`, which ships as a `.deb` — so this is an ABI break and a patch bump would have been wrong. The steps below are kept as the record of what was planned, not as the live instruction.

A **patch** bump: this PR adds no public API. Decided with the author, whose minor-bump decision was made for `when_any` and applies to PR 2.

`CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`.

- [ ] **Step 2: Run all eight gates, then commit**

```bash
git commit -am "Bump version to 1.10.1"
```

---

## Not in this plan

- **`when_any` itself.** PR 2, with its own plan written after this lands.
- **Death tests for the previously declined asserts** — `task_group`'s destructor and `spawn`'s two ownership claims, `task_completion_source`'s set-twice and await-twice. The mechanism now exists and each is cheap, but they are unrelated to this PR's hazard and would widen a shared-header change into an audit. Task 3 leaves both comments honest about the gap; the follow-up belongs on its own branch.
- **Any guard active under `NDEBUG`.** Review Focus 2 records the consequence. Making the check unconditional would put a load and a branch on every link and claim in the library, which is a performance decision with no measurement behind it and no bug asking for it.
- **`combinator_state`'s additive `request_children_stop()`.** Spec decision 4, needed only by `when_any`, so it lands in PR 2 beside its only caller.

## Self-Review

**1. Spec coverage.** Decision 8 is Task 1. Decision 9 is Task 2. The Staging section's "negative control: remove the assert, confirm the use-after-free goes unreported again, restore it" is Task 1 Step 7, realized as the stronger end-to-end form — reverting stage A's real fix rather than removing the assert, which measures the same thing on the actual hazard instead of a synthetic one. The Staging section's "re-verifies the six phase-5 components" is the eight-gate sweep in Global Constraints, which runs their whole suites. Decision 10's rewording and the rest of the spec belong to PR 2 and are listed under **Not in this plan**.

**2. Step scan.** Two steps carry code bodies: Task 1 Step 1 (a test, where the assertion *is* the decision) and Task 1 Step 5 (the destructor, where the store's position relative to the drain is the whole point and no signature implies it). Everything else is a signature, a constant, a test name with what it must assert, or a command with its expected output. No step says "handle edge cases" and no step names a type no task defines.

**3. Type consistency.** `alive_pattern`, `dead_pattern`, `m_liveness` and `alive()` are defined in Task 1 Step 4 and referred to by those names in Steps 5, 6 and 7 and in Task 2's Interfaces. `detach_all_locked()` is the existing private member at `waiter_list.hpp:585`, spelled as the source spells it. The refused link an event gives — an `async_event` already set makes `ready()` true at the link — is the cheapest way to reach Task 2's precondition, and `tests/test_async_wait.cpp` is a new file because `wait_awaiter` has none.

**4. Review Focus.** Five modes, each owned: 1 and 2 are documented-not-covered in Task 1 with the reason stated; 3 and 4 are tests in Task 2 Steps 2 and 3; 5 is Task 3 Step 1. Mode 1 is the one I am least comfortable leaving uncovered, and it is recorded as a known gap rather than silently skipped, which is the same shape as the gap the two death-test comments already record.

**5. Proportion.** The spec is 300 lines and this plan is roughly 280, a ratio below 1. Two one-line code changes and the evidence for them; the length is test names, assertions and two measurement protocols, none of which a plan can delegate.

**One thing I changed while writing this.** The spec's negative control was "remove the assert, confirm the use-after-free goes unreported, restore it." That measures the guard against a hazard a test constructs. Reverting stage A's `m_join.reset()` instead measures it against the hazard that actually shipped — the one that passed 20/200 repeats under both sanitizers and was found by reading. If the sentinel does not catch that, it does not earn its place, whatever a synthetic test says. Task 1 Step 7 is therefore the gate on this whole PR, and a zero there is a finding about the premise rather than a detail to work around.

# Precondition Death Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pin the five documented preconditions of `alt::task_group` and
`alt::task_completion_source` with death tests, so an assert that is later weakened or
deleted fails a test instead of passing quietly.

**Architecture:** Five `EXPECT_DEATH` cases added to the two existing test files,
following the pattern established in `tests/test_waiter_list.cpp`: a plain
`TEST(<Component>DeathTest, ...)`, the NDEBUG and ThreadSanitizer skip guards from
`tests/death_test_support.hpp`, a scoped `"threadsafe"` style, and a matcher that is
the assert's own text. No library code changes.

**Tech Stack:** C++23, GoogleTest 1.16, CMake presets, `clang-tidy-19`, `lcov`.

**Spec:** `docs/superpowers/specs/2026-11-17-precondition-death-tests-design.md`

## Global Constraints

- Every test's entire setup goes **inside** the `EXPECT_DEATH` statement block. The
  statement runs only in the child, so nothing is constructed in the parent and no
  parent-side lifetime or leak question arises.
- Each matcher is the project's own assert text, never a generic `Assertion`.
  `_GLIBCXX_ASSERTIONS` is on by default here and aborts with its own message.
- The suite's death tests take no fixture, so these get no
  `reset_coroutine_test_state()`. They must not read `live_frames` or `destroyed_tags`.
- `tests/death_test_support.hpp` is included, not re-derived. Skip messages stay local
  to each file and name that file's own assert.
- `tests/.clang-tidy` is appended to, never rewritten.
- Any `NOLINTNEXTLINE(readability-function-cognitive-complexity)` comment states a
  figure measured with pinned `clang-tidy-19`, never one copied from another file.
- No `tests/CMakeLists.txt` change: both files are already in `alt_tests`.
- Naming is `snake_case` for locals; GoogleTest suite and case names stay PascalCase as
  the suite already writes them.

## Review Focus

Failure modes the design implies that the tests themselves cannot assert. Each has a
verification step in the task that owns it.

1. **Under `NDEBUG` the tests must skip, not fail.** `EXPECT_DEATH` on a statement that
   does not die is a failure, so a missing skip guard turns the `release-tests` preset
   red. Verified in Task 3 by running that preset.
2. **Under ThreadSanitizer the tests must skip.** TSan does not support `fork`, so the
   child would die of the tool rather than of the assert. Verified in Task 3.
3. **Under AddressSanitizer the tests must still run and pass.** ASan prints its own
   output around an abort; the matcher must still match. Verified in Task 3.
4. **The `Coverage` gate must survive an aborting child.** `gcov` data written from a
   child that dies of `SIGABRT` can produce merge warnings. Verified in Task 3.
5. **A test that passes without its assert being what killed it.** There is no red
   phase here, since the asserts predate the tests, so each test is checked by removing
   its assert and confirming that test alone fails. Tasks 1 and 2, Step 5.
6. **A suppression that is not needed must not be added.** Each of the five bodies is
   measured before any `NOLINTNEXTLINE` is written; the macro alone in a test body
   measured 0 in `tests/test_waiter_list.cpp`, so some of these may need none.

---

### Task 1: task_group's three death tests

**Files:**
- Modify: `tests/test_task_group.cpp` — scaffolding in the anonymous namespace
  (around `:43`), retire the gap comment at `:28-39`, append three tests at end of file
- Reference: `src/task_group.cpp:12,17,18` (the asserts), `tests/test_waiter_list.cpp:683-720`
  (the pattern), `tests/test_when_any_result.cpp:30-46` (the compact scaffolding form)

**Interfaces:**
- Consumes: `alt_test::asserts_are_compiled_in`, `alt_test::thread_sanitizer_is_active`,
  `alt_test::scoped_death_test_style` from `tests/death_test_support.hpp`;
  `parked_root(alt::async_event& gate, int& started, int tag)` and
  `immediate_root(int& ran)`, both already in this file's anonymous namespace.
- Produces: nothing other tasks consume.

- [ ] **Step 1: Add the scaffolding**

Add `#include "death_test_support.hpp"` and `#include <string_view>`. Inside the
anonymous namespace, add the three `using alt_test::...` declarations and two
`constexpr std::string_view` skip messages, following
`tests/test_when_any_result.cpp:30-46`. `skipped_without_asserts` names *these three*
asserts — the destructor's liveness check and `spawn`'s two ownership claims — and says
NDEBUG compiles them away. `skipped_under_thread_sanitizer` says a death test forks and
TSan does not support fork, and names the presets that do run them.

- [ ] **Step 2: Write the three tests**

Append to the file. Each body is the two skip guards, then
`const scoped_death_test_style style{"threadsafe"};`, then one `EXPECT_DEATH` whose
statement block holds the whole setup:

```cpp
TEST(TaskGroupDeathTest, DestroyingAGroupWithALiveTaskAborts)
// statement block, in this declaration order -- see Step 3:
//   alt::async_event gate; int started = 0; alt::task_group group;
//   group.spawn(parked_root(gate, started, 1));
// matcher: "destroying an alt::task_group with live tasks"

TEST(TaskGroupDeathTest, SpawningWithACallersStopTokenAborts)
//   std::stop_source signal; alt::task_group group; int ran = 0;
//   group.spawn(immediate_root(ran), {.token = signal.get_token()});
// matcher: "alt::task_group supplies its own stop token"

TEST(TaskGroupDeathTest, SpawningWithACallersCompletionHookAborts)
//   alt::task_group group; int ran = 0;
//   hook invoke is a captureless lambda converted to void(*)(void*) noexcept
//   group.spawn(immediate_root(ran), {.on_completion = {.invoke = ..., .state = nullptr}});
// matcher: "alt::task_group owns start_options::on_completion"
```

`spawn` is not `[[nodiscard]]`, so no `static_cast<void>` is needed on its result.

- [ ] **Step 3: Write the comment that explains the declaration order**

Above the first test, state why `gate` is declared before `group`: the group is
destroyed first, while the task is still parked, so `outstanding()` is 1 and the assert
fires. Declared after, the event is destroyed first, releases the waiter, the count
reaches zero and the destructor passes. Also replace the gap comment at `:28-39` with a
note saying what these three now pin, and that a `spawn` racing the destructor is still
not reachable from a test.

- [ ] **Step 4: Build and run the three tests**

Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure -R TaskGroupDeathTest`
Expected: 3 tests pass. A failure reading "exited with code 0" means the assert did not
fire; a failure reading that the output did not match means it died of something else.

- [ ] **Step 5: Prove each test guards its assert, by removing the assert**

There is no red phase to inherit: the asserts already exist, so these tests pass the
first time they are run, and a test that passes for the wrong reason would look
identical. The mutation check is the substitute. For each of the three, comment out just
that assert in `src/task_group.cpp`, rebuild, and run the three tests.

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure -R TaskGroupDeathTest`
Expected: the one test whose assert was removed fails, reporting that the statement
exited with code 0, and the other two still pass. Restore the assert and re-run before
moving on; the file must be back to `git diff --quiet src/task_group.cpp` clean.

- [ ] **Step 6: Measure cognitive complexity and add only the suppressions needed**

Run `clang-tidy-19` on this file directly, with GoogleTest's headers passed via
`-isystem` rather than `-I`, at the default threshold and then at threshold 0. Add a
`NOLINTNEXTLINE(readability-function-cognitive-complexity)` only to bodies that are
actually reported, and write the measured figure into the comment. Do not copy
`tests/test_waiter_list.cpp`'s numbers.

- [ ] **Step 7: Verify clang-tidy is clean for this file**

Run the per-file `clang-tidy-19` invocation again.
Expected: no findings in `tests/test_task_group.cpp`.

- [ ] **Step 8: Format and commit**

```bash
clang-format -i tests/test_task_group.cpp
git add tests/test_task_group.cpp
git commit -m "Pin task_group's three documented preconditions with death tests"
```

---

### Task 2: task_completion_source's two death tests

**Files:**
- Modify: `tests/test_task_completion_source.cpp` — scaffolding in the anonymous
  namespace, retire the gap comment at `:38-47`, append two tests at end of file
- Reference: `include/alt/task_completion_source.hpp:272,293` (the asserts)

**Interfaces:**
- Consumes: the same three `alt_test` symbols as Task 1;
  `waiting_root<T>(alt::task_completion_source<T>&, completion_record<T>&, int tag)` and
  `completion_record<T>`, both already in this file's anonymous namespace.
- Produces: nothing other tasks consume.

- [ ] **Step 1: Add the scaffolding**

As Task 1 Step 1, with this file's own two skip messages. `skipped_without_asserts`
names the set-twice and await-twice preconditions.

- [ ] **Step 2: Write the two tests**

```cpp
TEST(TaskCompletionSourceDeathTest, SettingASourceTwiceAborts)
//   alt::task_completion_source<int> source; source.set_value(1); source.set_value(2);
// matcher: "setting an alt::task_completion_source twice"

TEST(TaskCompletionSourceDeathTest, AwaitingASourceTwiceAborts)
//   alt::task_completion_source<int> source;
//   completion_record<int> first; completion_record<int> second;
//   const auto first_handle  = alt::start(waiting_root<int>(source, first, 1));
//   const auto second_handle = alt::start(waiting_root<int>(source, second, 2));
// matcher: "awaiting an alt::task_completion_source twice"
```

The first waiter parks on the unset source; the second's `co_await` is what aborts.
Both are inside the statement block, so neither runs in the parent.

- [ ] **Step 3: Replace the gap comment**

Replace `:38-47` with a note saying what these two pin, and that two threads setting
concurrently is still not reachable from a test — the double-set check sits inside the
waiter list's critical section, so no rendezvous a test can write lands two setters
inside it.

- [ ] **Step 4: Build and run the two tests**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure -R TaskCompletionSourceDeathTest`
Expected: 2 tests pass.

- [ ] **Step 5: Prove each test guards its assert, by removing the assert**

As Task 1 Step 5. The asserts are at `include/alt/task_completion_source.hpp:272` and
`:293`; comment out one at a time, rebuild, and confirm that exactly the matching test
fails while the other passes. Restore and re-run, leaving
`git diff --quiet include/alt/task_completion_source.hpp` clean.

- [ ] **Step 6: Measure and add only the suppressions needed**

As Task 1 Step 6, for this file.

- [ ] **Step 7: Verify clang-tidy is clean for this file**

Expected: no findings in `tests/test_task_completion_source.cpp`.

- [ ] **Step 8: Format and commit**

```bash
clang-format -i tests/test_task_completion_source.cpp
git add tests/test_task_completion_source.cpp
git commit -m "Pin task_completion_source's set-twice and await-twice preconditions"
```

---

### Task 3: The eight-gate sweep

**Files:**
- Modify: `tests/.clang-tidy` only if a new suppression is genuinely needed, appended
- Modify: the two test files, only to fix what a gate reports

**Interfaces:**
- Consumes: Tasks 1 and 2 complete and committed.
- Produces: a branch that every CI job passes.

- [ ] **Step 1: Debug build and full suite**

Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: all pass, the five new tests among them.

- [ ] **Step 2: AddressSanitizer + UBSan** — Review Focus 3

Run: `cmake --preset debug-asan && cmake --build --preset debug-asan && ctest --preset debug-asan --output-on-failure`
Expected: all pass. The five run here rather than skipping, and the matcher still
matches with ASan's own output around the abort.

- [ ] **Step 3: ThreadSanitizer** — Review Focus 2

Run: `cmake --preset debug-tsan && cmake --build --preset debug-tsan && ctest --preset debug-tsan --output-on-failure`
Expected: all pass, with the five **skipped** and the skip message naming TSan's lack of
fork support. Confirm by reading the five names in the output, not by the exit code
alone.

- [ ] **Step 4: Optimized test run** — Review Focus 1

Run: `cmake --preset release-tests && cmake --build --preset release-tests && ctest --preset release-tests --output-on-failure`
Expected: all pass, with the five **skipped** under NDEBUG and the skip message naming
the guard that is gone. This is the gate that catches a missing skip guard.

- [ ] **Step 5: Whole-project clang-tidy, CI's invocation flag for flag**

```bash
cmake -B build/tidy -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCLANG_TIDY_PATH=/usr/bin/clang-tidy-19 -DCMAKE_BUILD_TYPE=Debug \
    -DALT_CLANG_TIDY=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/tidy
```
Expected: builds with zero findings. The gate aborts at the first finding, so a clean
run here is what the per-file checks in Tasks 1 and 2 cannot establish on their own.

- [ ] **Step 6: cppcheck**

Run: `cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck`
Expected: zero findings. Only analyzes `src/*.cpp`, so this change should not move it.

- [ ] **Step 7: Coverage** — Review Focus 4

```bash
cmake --preset debug-coverage && cmake --build --preset debug-coverage
ctest --preset debug-coverage --output-on-failure
cmake --build --preset debug-coverage --target coverage
```
Expected: `lcov` produces `build/debug-coverage/coverage/index.html`. Watch for `gcov`
merge warnings from the aborting children; the job fails if the report cannot be
produced.

- [ ] **Step 8: clang-format check**

Run: `clang-format --dry-run --Werror tests/test_task_group.cpp tests/test_task_completion_source.cpp`
Expected: no output.

- [ ] **Step 9: Commit anything the sweep changed**

```bash
git add -A && git commit -m "Satisfy the quality gates for the precondition death tests"
```
Skip this step if the sweep changed nothing.

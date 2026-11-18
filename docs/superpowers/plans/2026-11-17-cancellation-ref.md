# `alt::cancellation_ref` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give a caller-written awaiter the two things it needs to join the resumptions alt owns — the chain's stop token, and a way to tear the chain down instead of resuming — so that no coroutine body runs after a stop.

**Architecture:** One public header holding a pointer-sized, non-owning view over the existing `detail::cancellation_context`, plus a function template that obtains it from the typed promise handle a custom awaiter already receives through `detail::cancellation_checked::await_suspend`. No change to the cancellation wrapper, no new dependency, nothing about senders.

**Tech Stack:** C++23, header-only, `std::stop_token` and `std::coroutine_handle` only. GoogleTest. CMake presets.

**Spec:** `docs/superpowers/specs/2026-11-17-cancellation-ref-design.md`

## Global Constraints

- C++23. GCC 13+ or Clang 19+. No new standard-library dependency and no third-party one.
- `snake_case` everything except template typenames, which are `PascalCase`. Private members prefixed `m_`.
- `const` by default. Doxygen `/** */` on every public function, class and struct.
- `include/alt/cancellation_ref.hpp` is a standalone public header. It must **not** be included from `task.hpp` or `cancellable_task.hpp`; the dependency arrow runs one way, `cancellation_ref.hpp` → `detail/cancellation.hpp`.
- `cancellation_ref` must stay trivially copyable and the size of one pointer. A `static_assert` in the test file pins both.
- Preconditions are `assert` with a message naming the type, in the shape `scheduler_ref::post` and `detail::abandon_chain` use.
- Per task: `cmake --build --preset debug && ctest --preset debug --output-on-failure`. The full eight-gate sweep is Task 7 and is not repeated per task.
- `tests/.clang-tidy` is **append-only**. Never rewritten.
- Any `NOLINT` suppression carrying a number must have that number measured with pinned `clang-tidy-19`, not copied from another site.
- Branch `feature/cancellation-ref`. Commit per task. Do not merge to `main`.

## Review Focus

Five conditions the spec implies that no task above pinned on its own; each line's test is added to the task named.

1. **An empty `std::coroutine_handle` passed to `get_cancellation_ref`** — `h.promise()` on an empty handle is undefined, so this must assert rather than read garbage. Test in Task 2.
2. **An empty handle passed to `resume_or_abandon`** — resuming an empty handle is undefined. Must assert. Test in Task 3, which is where that member first exists.
3. **A non-empty view whose token is not stoppable** — a hand-rolled root can hold a `cancellation_context` with a default-constructed token. `get_stop_token()` must hand back that empty token (so an adapter's `std::stop_callback` on it is an inert no-op) and `stop_requested()` must be false, not a crash. Test in Task 1.
4. **A view outliving its chain** — the `cancellation_context` lives in the root frame, so a view copied out and used after the chain finished is a dangling read. Not safely testable; it is a documented lifetime rule, and Task 5 must state it at the class and on the reference page.
5. **`resume_or_abandon` called more than once for one suspension** — both branches end the suspension, so a second call resumes or destroys a frame that is already gone. Not safely testable; documented as a precondition in Task 5, and the Task 3 Doxygen says the call must be the awaiter's last statement.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/alt/cancellation_ref.hpp` | **Create.** `alt::cancellation_ref` and `alt::get_cancellation_ref`. The whole public surface. |
| `include/alt/detail/cancellation.hpp` | **Modify.** Receives `relays_cancellation`; the stale `checked_resume_on` comment corrected in Task 5. |
| `include/alt/detail/async_wait.hpp` | **Modify.** Loses the `relays_cancellation` definition, keeps using it. |
| `tests/test_cancellation_ref.cpp` | **Create.** Every case in this plan. |
| `tests/CMakeLists.txt` | **Modify.** One line in the `alt_tests` sources list. |
| `docs/cancellation_ref.md` | **Create.** Reference page. |
| `docs/cancellable_task.md`, `docs/awaitable.md`, `README.md`, roadmap spec | **Modify.** The claim sweep and the component row. |
| `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md` | **Modify.** Version bump, Task 6. |

---

### Task 1: The view's observation surface and the accessor

**Files:**
- Create: `include/alt/cancellation_ref.hpp`
- Modify: `include/alt/detail/cancellation.hpp` (add `relays_cancellation`), `include/alt/detail/async_wait.hpp` (remove its definition)
- Create: `tests/test_cancellation_ref.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `detail::cancellation_context` (members `root`, `token`, `home`, `stop_requested()`) from `include/alt/detail/cancellation.hpp`.
- Produces: `alt::cancellation_ref` with `explicit operator bool() const noexcept`, `std::stop_token get_stop_token() const noexcept`, `bool stop_requested() const noexcept`, a private `explicit cancellation_ref(detail::cancellation_context*) noexcept`; and `template<typename Promise> cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise>) noexcept`. Tasks 2 and 3 add `abandon()` and `resume_or_abandon()` to the same class.

- [ ] **Step 1: Move `relays_cancellation` into `detail/cancellation.hpp`**

Cut the concept verbatim from `include/alt/detail/async_wait.hpp` (it sits just below the `alt::wait_result` alias, in `namespace alt::detail`) and paste it into `include/alt/detail/cancellation.hpp` directly above `abandon_chain`'s documentation block. Keep its existing doc comment.

`async_wait.hpp` already includes `detail/cancellation.hpp`, so nothing changes there. But add `#include <concepts>` to `detail/cancellation.hpp`: the concept spells `std::same_as`, and that header has no `<concepts>` of its own today — it would compile transitively through `alt/awaitable.hpp`, which is exactly the include this project does not rely on.

- [ ] **Step 2: Verify the move changed nothing**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: PASS, same count as before the move.

- [ ] **Step 3: Write the failing tests**

Create `tests/test_cancellation_ref.cpp`, add `test_cancellation_ref.cpp` to the `alt_tests` sources list in `tests/CMakeLists.txt` (after `test_cancellation.cpp`), and write:

```cpp
static_assert(std::is_trivially_copyable_v<alt::cancellation_ref>);
static_assert(sizeof(alt::cancellation_ref) == sizeof(void*));

TEST_F(CancellationRefTest, ADefaultConstructedViewIsEmpty)
// EXPECT_FALSE(static_cast<bool>(alt::cancellation_ref{}));
// EXPECT_FALSE(alt::cancellation_ref{}.stop_requested());
// EXPECT_FALSE(alt::cancellation_ref{}.get_stop_token().stop_possible());

TEST_F(CancellationRefTest, AViewInsideACancellableTaskIsNotEmpty)
// an awaiter that records alt::get_cancellation_ref(h); awaited from an
// alt::cancellable_task started with alt::start. EXPECT_TRUE on the view,
// EXPECT_TRUE on get_stop_token().stop_possible().

TEST_F(CancellationRefTest, AViewInsideAPlainTaskUnderSyncWaitIsEmpty)
// the same awaiter inside an alt::task driven by alt::sync_wait with a
// manual_scheduler. EXPECT_FALSE on the view.

TEST_F(CancellationRefTest, TheViewCarriesTheChainsStopToken)
// stop requested through the alt::cancellation_handle alt::start returned:
// EXPECT_TRUE(view.stop_requested()) and EXPECT_TRUE(token.stop_requested()).

TEST_F(CancellationRefTest, AViewOverANonStoppableTokenIsNotEmptyButCannotStop)
// Review Focus 3. A hand-rolled context (the test_root pattern in
// tests/test_cancellation.cpp) whose token is default-constructed:
// EXPECT_TRUE on the view, EXPECT_FALSE on get_stop_token().stop_possible(),
// EXPECT_FALSE on stop_requested().

TEST_F(CancellationRefTest, AnAwaiterTakingTheErasedHandleGetsAnEmptyView)
// an awaiter whose await_suspend takes std::coroutine_handle<> rather than a
// template: EXPECT_FALSE on the view even inside a cancellable_task.
```

The awaiter these share parks its handle in `alt_test::parked` and records the view in a file-scope variable, in the shape `tests/test_cancellation.cpp`'s `park` uses. The fixture is `CancellationRefTest` with a `SetUp` calling `reset_coroutine_test_state()` and clearing this file's own flags, matching `CancellationTest`.

- [ ] **Step 4: Run to verify they fail**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — no such file `alt/cancellation_ref.hpp`.

- [ ] **Step 5: Implement the header**

Create `include/alt/cancellation_ref.hpp`: `#pragma once`, include `<alt/detail/cancellation.hpp>`, `<cassert>`, `<coroutine>`, `<stop_token>`, and define in `namespace alt` the class and the accessor named in Interfaces. `operator bool` is `m_context != nullptr`; `get_stop_token()` returns `m_context->token` when non-empty and `std::stop_token{}` otherwise; `stop_requested()` returns false when empty. `get_cancellation_ref` asserts `h` and returns the view when `detail::relays_cancellation<Promise>` is satisfied, an empty view otherwise. Reach the private constructor by declaring the accessor a friend.

Doxygen must state: what an empty view means, that an erased `std::coroutine_handle<>` always yields an empty view and why (no `promise()`), and that an awaiter must template `await_suspend` over the promise type to get a non-empty one.

- [ ] **Step 6: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CancellationRef`
Expected: PASS, 6 tests.

- [ ] **Step 7: Commit**

```bash
git add include/alt/cancellation_ref.hpp include/alt/detail/cancellation.hpp include/alt/detail/async_wait.hpp tests/test_cancellation_ref.cpp tests/CMakeLists.txt
git commit -m "Add alt::cancellation_ref and its accessor"
```

---

### Task 2: `abandon()` and its two preconditions

**Files:**
- Modify: `include/alt/cancellation_ref.hpp`, `tests/test_cancellation_ref.cpp`

**Interfaces:**
- Consumes: Task 1's class; `detail::abandon_chain(const cancellation_context&) noexcept`.
- Produces: `void abandon() const noexcept` on `cancellation_ref`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(CancellationRefTest, AbandonTearsTheChainDown)
// awaiter records the view and parks; request_stop() through the handle; then
// view.abandon() from the test. EXPECT_EQ(alt_test::live_frames, 0) and the
// body-ran flag stays false.

TEST_F(CancellationRefTest, AbandonPostsTheTeardownToHome)
// the same, started with {.home = alt::scheduler_ref{pump}} over an
// alt::manual_scheduler: after abandon() EXPECT_GT(live_frames, 0), then
// pump.drain() and EXPECT_EQ(live_frames, 0). NOTE: run_all() is deprecated; drain() is the current name.

TEST(CancellationRefDeathTest, AbandoningAnEmptyViewAborts)
// matcher "abandoning an empty alt::cancellation_ref"

TEST(CancellationRefDeathTest, AbandoningAChainWithNoStopRequestedAborts)
// matcher "abandoning an alt chain with no stop requested"

TEST(CancellationRefDeathTest, GettingAViewFromAnEmptyHandleAborts)
// Review Focus 1. alt::get_cancellation_ref(std::coroutine_handle<some_promise>{})
// matched against its own assert message.
```

Death tests follow the house pattern at `tests/test_waiter_list.cpp:683-720`: both skip guards (`asserts_are_compiled_in`, `thread_sanitizer_is_active`), a scoped `"threadsafe"` style, plain `TEST`, no fixture — so any coroutine they build resets nothing and must not rely on `live_frames`. Include `death_test_support.hpp` and give this file its own `skipped_without_asserts` message naming these asserts.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — no member `abandon`.

- [ ] **Step 3: Implement `void abandon() const noexcept`**

Asserts `m_context != nullptr` with `"abandoning an empty alt::cancellation_ref"`, then asserts `m_context->stop_requested()` with `"abandoning an alt chain with no stop requested"`, then calls `detail::abandon_chain(*m_context)`.

The Doxygen carries the DELICATE warning in the form `detail::abandon_chain`'s does: this destroys the frame the calling awaiter lives in, so it must be the awaiter's last statement and nothing may be read through `*this` afterwards. It also states why the second precondition exists — this is not a general destroy-this-chain button; an operation that stopped for a reason the chain's token knows nothing about resumes with a released result instead, as alt's own six waits do.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CancellationRef`
Expected: PASS, 11 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/cancellation_ref.hpp tests/test_cancellation_ref.cpp
git commit -m "Add cancellation_ref::abandon with its stop-requested precondition"
```

---

### Task 3: `resume_or_abandon()`, with the mutation pair that proves it

**Files:**
- Modify: `include/alt/cancellation_ref.hpp`, `tests/test_cancellation_ref.cpp`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: `bool resume_or_abandon(std::coroutine_handle<> h) const noexcept` — true when it resumed.

- [ ] **Step 1: Write the failing tests**

The headline pair lands together. Both use the same awaiter type parameterized on what it does at completion, so the only difference between the two tests is that one branch of that choice:

```cpp
TEST_F(CancellationRefTest, ABodyDoesNotRunWhenTheAwaiterAbandons)
// opted-in awaiter; stop requested while parked; operation then "completes" and
// calls view.resume_or_abandon(handle). EXPECT_FALSE(body_ran) and
// EXPECT_EQ(live_frames, 0).

TEST_F(CancellationRefTest, ABodyDoesRunWhenTheAwaiterResumesInstead)
// THE MUTATION. The same awaiter calling handle.resume() in place of
// resume_or_abandon. EXPECT_TRUE(body_ran). Without this, the test above is
// consistent with the body never having been reachable at all.

TEST_F(CancellationRefTest, ResumeOrAbandonResumesWhenNoStopWasRequested)
// returns true, body runs, the awaiter's result is observed.

TEST_F(CancellationRefTest, ResumeOrAbandonResumesWhenTheViewIsEmpty)
// the same awaiter inside a plain alt::task under alt::sync_wait: returns true.

TEST_F(CancellationRefTest, AStopBeforeTheAwaitStopsTheOperationStarting)
// request_stop() before the co_await is reached: the pre-existing entry check
// fires first, so the awaiter's await_suspend is never entered. Pin it with a
// start counter: EXPECT_EQ(starts, 0). Guards against this path being read as a
// replacement for the entry check rather than an addition to it.

TEST(CancellationRefDeathTest, ResumeOrAbandonWithAnEmptyHandleAborts)
// Review Focus 2, matched against its own assert message.
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — no member `resume_or_abandon`.

- [ ] **Step 3: Implement `bool resume_or_abandon(std::coroutine_handle<> h) const noexcept`**

The body order is the point of the function and is not left to the implementer:

```cpp
assert(h && "resuming or abandoning an alt chain with an empty coroutine handle");

// Copied to a local before either branch: both free the frame *this may live in.
detail::cancellation_context* const context = m_context;

if(context != nullptr && context->stop_requested())
{
    detail::abandon_chain(*context);
    return false;
}

h.resume();
return true;
}
```

Doxygen: the same DELICATE warning as `abandon()`, plus — the call must happen at most once per suspension, since both branches end it (Review Focus 5) — plus the note that an empty view resumes, which is what lets one awaiter serve a cancellable chain and a plain `alt::task` with no branch of its own.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CancellationRef`
Expected: PASS, 17 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/cancellation_ref.hpp tests/test_cancellation_ref.cpp
git commit -m "Add cancellation_ref::resume_or_abandon"
```

---

### Task 4: The guarantees that need no new library code

Four properties the spec promises that the API shape alone does not demonstrate. No library change — if any of these needs one, stop and report it rather than editing the header to suit the test.

**Files:**
- Modify: `tests/test_cancellation_ref.cpp`

**Interfaces:** Consumes Tasks 1–3. Produces nothing.

- [ ] **Step 1: Write the tests**

```cpp
TEST_F(CancellationRefTest, ATeardownFromInsideRequestStopIsSafe)
// The adapter's primary path. The awaiter registers a std::stop_callback on
// view.get_stop_token() whose body calls resume_or_abandon. request_stop()
// through the chain's own handle then tears the chain down synchronously, inside
// that call: EXPECT_FALSE(body_ran), EXPECT_EQ(live_frames, 0), and the handle is
// still usable afterwards (EXPECT_TRUE(handle.stop_requested())).

TEST_F(CancellationRefTest, ATeardownFromInsideALinkedTokensRequestStopIsSafe)
// The same, with the stop arriving through start_options::token from an external
// std::stop_source and the chain's own handle discarded, so the teardown destroys
// m_external while it is mid-invocation. The external source is still usable
// afterwards.

TEST_F(CancellationRefTest, AnImmovableAwaiterWorksAsAnLvalue)
// Issue #10's third limit. The awaiter holds a deliberately immovable member
// (a std::optional<std::stop_callback<...>> is one, since std::stop_callback is)
// and is awaited as a named lvalue. Compiling is most of the assertion; assert the
// view it recorded is non-empty so the test cannot pass vacuously.

TEST_F(CancellationRefTest, ACombinatorChildKeepsItsBlocksVerdict)
// The one place this API could corrupt another component. An opted-in awaiter
// inside a child of alt::when_all; the block is cancelled; the child abandons.
// EXPECT the when_all result is an error equal to std::errc::operation_canceled,
// and that no slot is read unfilled (a satisfied verdict over unfilled slots
// aborts in result_store, so a pass here is the assertion).

TEST_F(CancellationRefTest, AnOffThreadCompletionResumesSafely)
// The resume branch instrumented off-thread: the operation completes on a
// std::jthread while the test thread does not stop the chain. Synchronize with a
// latch or binary_semaphore, never a sleep.
```

- [ ] **Step 2: Run them**

Run: `ctest --preset debug --output-on-failure -R CancellationRef`
Expected: PASS, 22 tests.

- [ ] **Step 3: Run the two sanitizer gates, which are what these cases are for**

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R CancellationRef
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R CancellationRef
```
Expected: PASS under both, with no sanitizer report. The death tests skip under ThreadSanitizer and say so.

- [ ] **Step 4: Commit**

```bash
git add tests/test_cancellation_ref.cpp
git commit -m "Pin re-entrant teardown, the immovable-awaiter form and combinator integrity"
```

---

### Task 5: Documentation, and the sweep of the claim this falsifies

**Files:**
- Create: `docs/cancellation_ref.md`
- Modify: `docs/cancellable_task.md`, `docs/awaitable.md`, `include/alt/detail/cancellation.hpp`, `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md`, `README.md`

- [ ] **Step 1: Write `docs/cancellation_ref.md`**

In the shape of the other reference pages (`docs/scheduler.md` is the closest sibling, documenting `scheduler_ref`). It must cover: the view and the accessor; the opt-in recipe, with **both** the prvalue and the named-lvalue forms and which constraint applies to each; that an operation stopping for a reason the chain's token never saw resumes with a released result rather than calling `abandon()`; the re-entrant teardown guarantee in its narrow form — safe provided the chain's stop state retains an owner outside the frames the teardown destroys (this clause originally read "some owner of the stop state outlives the call, which the caller of `request_stop()` always is", which is false for the linked-token-with-discarded-handle shape; see the correction in the design spec), resting on [stopcallback.cons] for the destruction and on `stop_forwarder`'s keepalive for the state's survival, and not on a sanitizer run; the lifetime rules (Review Focus 4 and 5: a view must not outlive its chain, and `resume_or_abandon` is called at most once per suspension); and the erased-handle trap.

- [ ] **Step 2: Sweep the falsified claim**

The claim is "a foreign awaitable's resumption cannot be intercepted, so foreign awaitables are checked on entry only". Sweep by claim, not by phrase — it is also stated as a count. The sites, from `grep -rn "checked on entry only\|resumptions this library owns\|resumptions alt owns" docs/ include/ README.md`:

| Site | Change |
|------|--------|
| `docs/cancellable_task.md:47` | "the resumptions this library owns" list — add the fourth, a caller's awaiter that opted in |
| `docs/cancellable_task.md:56` | the forward reference — now conditional on the author not opting in |
| `docs/cancellable_task.md:322-339` | the section itself: "of which there are three" becomes four; the `my_awaiter` example stays as the *un*-opted-in case, now contrasted with the opted-in version and linked to the new page |
| `include/alt/detail/cancellation.hpp` (the `checked_resume_on` comment, "foreign awaitables remain checked on entry only") | corrected to name the opt-in |
| `docs/awaitable.md` | a cross-reference from the write-your-own-awaitable material |
| `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md` | the sender/receiver exclusion row gains a note: issue #10's dependency-free half landed as this component; the sender adapter itself stays excluded |
| `README.md` Components table | a row for `<alt/cancellation_ref.hpp>` |

Leave alone, having been checked: `docs/superpowers/specs/2026-10-26-async-primitives-design.md:90` ("the two resumptions alt owns") — a dated spec is a historical record and two was correct on its date; `docs/cancellable_task.md`'s "A task parked on something that never resumes never self-destructs" (an opted-in awaiter whose operation never completes still never reaches a resumption); and the narrowing of point 6 for the six primitives, unchanged by this.

- [ ] **Step 3: Verify the sweep left nothing**

Run: `grep -rn "checked on entry only\|of which there are three\|resumptions this library owns\|resumptions alt owns" docs/ include/ README.md`
Expected: every surviving hit is either in the new page, in this plan, in the design spec, or one of the three sites listed as deliberately left.

- [ ] **Step 4: Commit**

```bash
git add docs/ include/alt/detail/cancellation.hpp README.md
git commit -m "Document cancellation_ref and correct the entry-only claim"
```

---

### Task 6: Version bump to 1.14.0

Its own commit, per `CLAUDE.md`. Minor, decided with the owner: a new public header and type mean the previous minor's headers and this library are not interchangeable.

The target was 1.13.0 when this plan was written, against a `main` at 1.12.3. `main` then reached 1.13.0 itself (commit `470c36e`, with MSVC support), colliding with this branch's bump, so the merge of `main` re-bumped the branch to **1.14.0** on top of it. The reasoning is unchanged; only the number moved.

**Files:** `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

- [ ] **Step 1: Change all six**

`CMakeLists.txt` `project(Alt VERSION 1.14.0 ...)`; `version.hpp` macros and constexpr variables; `src/version.cpp`'s string. In `tests/test_version.cpp` change **three** places: the `EXPECT_EQ` macros, the `static_assert`s on `alt::version_patch` and friends — which spell the number bare, so a grep for the old version string misses them — and the version string. In `README.md` and `CLAUDE.md`, the project version occurrences plus the `find_package` compatibility prose, whose operands illustrate the rule rather than this release and must still agree with the snippet above them.

- [ ] **Step 2: Verify**

Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure -R Version`
Expected: PASS. A missed `static_assert` fails the **build**, not `ctest`, so a green build is part of the check.

Run: `grep -rn "1\.13\.0" . --exclude-dir=build --exclude-dir=.git`
Expected: only historical mentions — dated specs and plans under `docs/superpowers/`.

- [ ] **Step 3: Commit**

```bash
git commit -am "Bump version to 1.14.0"
```

---

### Task 7: The full gate sweep and the pull request

A local sweep that skips one gate is not a green branch, however it reports itself.

- [ ] **Step 1: `clang-format` the changed files**

```bash
clang-format -i include/alt/cancellation_ref.hpp include/alt/detail/cancellation.hpp include/alt/detail/async_wait.hpp tests/test_cancellation_ref.cpp
```

- [ ] **Step 2: Run all eight gates**

GCC debug + tests; Clang debug + tests; `release-tests`; `debug-asan`; `debug-tsan`; the pinned `clang-tidy` invocation from `CLAUDE.md` (`clang-19`/`clang++-19`/`/usr/bin/clang-tidy-19`, `CMAKE_BUILD_TYPE=Debug`); `cppcheck`; and `debug-coverage` with its `coverage` target — **no separate `ctest` ahead of it**, the target runs the suite itself. The `libgcov profiling error ... overwriting an existing profile data` lines against a reused build directory are harmless; see issue #32.

Expected: all green. `cppcheck` covers `src/*.cpp` only, so it contributes nothing here — that is expected, not a failure.

- [ ] **Step 3: Fix any clang-tidy findings**

The whole-project gate aborts at the first finding, so also check each changed file directly with `clang-tidy-19`, passing the GoogleTest headers with `-isystem` rather than `-I`. Suppressions go in `tests/.clang-tidy` by **appending**, or as a `NOLINT` with a measured figure.

- [ ] **Step 4: Open the pull request**

```bash
git push -u origin feature/cancellation-ref
gh pr create --base main --title "Add alt::cancellation_ref, a public extension point for the cancellation protocol" --body "..."
```

The body states what landed, what it deliberately does not (no `std::execution` code, no sender adapter, `alt::task` is not a sender), closes issue #10 with a note that its route A stays declined and its open questions stay open, and ends with the attribution line. Do not merge; that is the owner's call.

- [ ] **Step 5: File the deferrals as issues**

Anything found and parked during implementation becomes a `gh issue create`, never a line in this plan — the plan workspace is deleted when the branch finishes, and PR #19's deferred list went with it. Reference the issue number wherever the deferral is recorded.

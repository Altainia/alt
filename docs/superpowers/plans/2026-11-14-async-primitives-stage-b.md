# Async primitives stage B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the `start_options` completion hook, `alt::task_completion_source<T>` and `alt::task_group`, completing phase 5.

**Architecture:** A completion hook on `start_options`, fired from a promise member's destructor so it covers normal completion and cancelled teardown alike, is the channel a `cancellable_task` has never had. `task_group` counts outstanding tasks through that hook, using a reusable `detail::completion_counter` (a count plus a joinable `detail::waiter_list`) so phase 4's `when_all` can share the accounting. `task_completion_source<T>` is a `detail::wait_awaiter` over a `detail::result_store<T>`, the bridge from a callback API to a `co_await`.

**Tech Stack:** C++23, GCC 13+ / Clang 19+, CMake presets, GoogleTest. Builds entirely on stage A's `detail::waiter_list` and `detail::wait_awaiter`, shipped in 1.8.0.

**Spec:** `docs/superpowers/specs/2026-10-26-async-primitives-design.md` (stage B is the second half of its **Staging** section)

## Global Constraints

- Namespace `alt`; engine internals in `alt::detail`. Helper-only headers go in `include/alt/detail/`.
- `snake_case` for everything except template typenames, which are `PascalCase`. Private members prefixed `m_`.
- Declare variables `const` unless mutation is required.
- `std::expected` for recoverable errors; `std::errc::operation_canceled` is the released condition, carried as `std::error_code`.
- Doxygen `/** */` on every public function, class, struct, enum and union; update an existing comment when a change alters its meaning.
- `[[nodiscard]]` on every value-returning await where ignoring the result would proceed as though the wait succeeded.
- American spelling throughout, except the approved double-l forms `cancelled` / `cancellable` / `cancellation`.
- `task_completion_source<T>` must be header-only (it is a template). `task_group` is a compiled component: `.hpp` + `src/task_group.cpp`, following phase 3's precedent.
- Every new test file is registered in `tests/CMakeLists.txt`'s `alt_tests` sources list.
- Six gates must pass before the PR: `debug`, `debug-asan`, `debug-tsan`, `release-tests`, clang-tidy, cppcheck, plus clang-format on changed files. **clang-tidy must be run with CI's tool**: `-DCLANG_TIDY_PATH=/usr/bin/clang-tidy-19 -DCMAKE_CXX_COMPILER=clang++-19`. A newer clang-tidy does not reproduce CI's findings.
- Branch `feature/async-primitives-stage-b`; all work reaches `main` through a pull request; the version bump is its own commit.
- **Mutation checks are required for every test the Review Focus names and for every test that asserts a race or an ordering**, not for every test in the suite. The spec asks for one per test; stage A's experience is that a mutation check on a test asserting a plain value measures the compiler, while the ones that matter are exactly the tests whose failure mode is a missed interleaving. Each mutation step records the measured number of failures out of `race_repeats`, and records a zero as a zero.

## Departures from the spec, with the argument

Both are the plan's calls, recorded here because the spec says otherwise and a reviewer will notice. Add each as a revision note to the spec in the task that implements it.

1. **The awaited value stays in the source's `result_store<T>`; it is not moved into the waiter's node at claim time.** The spec asks for the latter so "a resumed coroutine never reads storage the setter may have freed". But `detail::waiter_node` has no typed slot, and adding a type-erased one changes stage A's delicate shipped mechanism for a hazard that cannot occur: resumption is synchronous inside `set_value`, and `await_resume` runs before the resumed body continues, so the source is alive at every read. The two other ways out of the wait — the stop callback and the source's destructor — both report released and read nothing. What this does rely on is the family's existing contract that a primitive outlives its own resumption walk, which stage A already states.
2. **`task_completion_source<T&>` yields `std::expected<std::reference_wrapper<T>, std::error_code>`.** The spec says `T&` follows `result_store`'s shape, but `std::expected<T&, E>` is ill-formed. Measured, not assumed, on GCC 15 with `-std=c++23`: `std::expected<int&, std::error_code>` fails `static_assert(!is_reference_v<_Tp>)` at `<expected>:337` and then fails again forming `operator->`'s return type. The `std::reference_wrapper` form compiles clean under `-Wall -Wextra` and `int& seen = *result;` binds to the caller's object, which was checked by address.

## Review Focus

Five failure modes the spec implies, each with its test pinned to the task that owns the code.

1. **`set_value` landing between a waiter's `ready()` check and its link** — the lost wakeup stage A's `link(node, condition)` exists to close. A waiter must never park on a source that is already set. (Task 3)
2. **A stop request racing `set_value` for the same node** — exactly one party resumes, and a cancelled waiter is never told it was satisfied while the value is consumed by nobody. (Task 3)
3. **`complete()` driving the count to zero while a joiner is linking** — the joiner either sees zero and does not park, or parks and is detached by that same `complete()`. There is no third interleaving, and a rendezvous test must force the window rather than hope for it. (Task 2)
4. **A task that completes synchronously inside `spawn`** — the hook fires before `start` returns, so the count must be raised before the task starts and `spawn` must touch no member afterwards. (Task 4)
5. **A joiner cancelled while parked on `join()`** — it is released with `operation_canceled` while tasks are still live, which leaves the group unjoined; the destructor's assert is the documented consequence. (Task 4)

---

### Task 1: The `start_options` completion hook

The additive phase-2 change the author approved. Nothing else in stage B has a channel without it.

**Files:**
- Modify: `include/alt/cancellable_task.hpp` (add `completion_handler` beside `exception_handler`; add `start_options::on_completion`; add the promise member and set it in `configure`)
- Modify: `docs/cancellable_task.md` (new section; `## Interface` block)
- Modify: `README.md:171` (the `<alt/cancellable_task.hpp>` row gains `completion_handler`)
- Test: `tests/test_cancellable_task.cpp`

**Interfaces:**
- Consumes: `alt::start_options`, `detail::cancellable_task_promise::configure(std::coroutine_handle<>, const alt::start_options&)`, `alt::start(cancellable_task, const start_options&)`.
- Produces:
  ```cpp
  namespace alt {
      struct completion_handler
      {
          void (*invoke)(void* state) noexcept = nullptr;
          void* state                          = nullptr;
      };
      // start_options gains, after on_exception:
      completion_handler on_completion{};
  }
  ```
  The `noexcept` is in the function pointer's type, not a comment, because the hook fires from a destructor. Task 4 installs one; nothing else in the library reads it.

- [ ] **Step 1: Write the failing tests**

In `tests/test_cancellable_task.cpp`, in the file's existing `TEST_F(CancellableTaskTest, …)` style. A free `static void` with a `void*` state, or a capture-free lambda converted to the pointer type, serves as the hook in each.

```cpp
TEST_F(CancellableTaskTest, TheCompletionHookFiresOnNormalCompletion)
// one task that completes without suspending; hook count == 1 after start returns

TEST_F(CancellableTaskTest, TheCompletionHookFiresOnCancelledTeardown)
// park the task, request_stop, resume_parked(); hook count == 1

TEST_F(CancellableTaskTest, TheCompletionHookFiresAfterTheBodysLocalsAreDestroyed)
// a frame_tracker local in the body; record alt_test::live_frames inside the hook
EXPECT_EQ(live_frames_seen_by_the_hook, 0);

TEST_F(CancellableTaskTest, ANeverStartedTaskFiresNoCompletionHook)
// construct a cancellable_task, never call start, let it go out of scope
EXPECT_EQ(hook_calls, 0);

TEST_F(CancellableTaskTest, ARequestStopFromInsideTheCompletionHookTouchesNoDestroyedMember)
// the hook calls request_stop() on the handle for the task that is dying, which is
// the re-entrancy the member ordering exists to survive; must not crash under ASan
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R CancellableTask`
Expected: FAIL — `start_options` has no member `on_completion`.

- [ ] **Step 3: Add `completion_handler` and `start_options::on_completion` in `include/alt/cancellable_task.hpp`**

Place `completion_handler` immediately after `exception_handler`, documented as its sibling: the same allocation-free erased shape, for the other thing an owner wants to know. `on_completion` goes last in `start_options`, documented as fired once from the task's own frame destruction, on whichever thread destroyed it, covering normal completion and cancelled teardown because that destructor is the only point both paths pass through.

- [ ] **Step 4: Add `detail::completion_notifier` and the promise member**

```cpp
namespace alt::detail {
    struct completion_notifier
    {
        alt::completion_handler handler{};
        ~completion_notifier();   // calls handler.invoke(handler.state) when invoke != nullptr
    };
}
```

Declare `completion_notifier m_completion{};` as the **first** member of `cancellable_task_promise`, so reverse-order destruction destroys it **last**, and set it in `configure` from `options.on_completion`.

The existing comment at the member block says these members carry no declaration-order requirement. That stops being true here, so replace it with a block stating the new one — and state it correctly, because the obvious version of this argument is wrong.

**Superseded (2026-11-14, task 1 review).** This step first read: "Declaring `m_completion` anywhere else fires the hook while a `std::stop_callback` over a half-destroyed promise is still registered." That is backwards. Members are destroyed in reverse declaration order, so declaring `m_completion` **last** destroys it **first**, and the hook then fires with every other member still fully constructed — there is no half-destroyed promise in that order at all, and `stop_forwarder` holds a reference-counted *copy* of the source rather than a pointer into the promise, as its own Doxygen says. The replacement argument is narrower and true: destroying `m_external` **deregisters** the stop callback from the external token, and `std::stop_callback`'s destructor blocks until a concurrently running invocation returns, so firing the hook last guarantees that no stop request arriving during the hook — from any thread — can route into this frame. That is defense in depth, not a fix for a reachable fault, and the step 6 measurement agrees: no order fails a test.

The block therefore does **not** carry a `DELICATE` header. This project reserves that for orderings that are load-bearing, and a `DELICATE` marker over a hazard a maintainer can disprove from the lines below it is worse than none. State it as a deliberate choice, give the deregistration reason, and say plainly that no fault was measured in either order.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CancellableTask`
Expected: PASS, and the rest of the suite unchanged.

- [ ] **Step 6: Mutation check**

Move `m_completion` to be the last member instead of the first, rebuild `debug-asan`, and record what `ARequestStopFromInsideTheCompletionHookTouchesNoDestroyedMember` reports. Write the measured result into the test's comment — including "no failure observed" if that is what happens, in which case say plainly that the ordering is reasoned rather than pinned by a test. Restore the member order.

- [ ] **Step 7: Document it**

A `## An owner can be told when the task is gone` section in `docs/cancellable_task.md`: what fires it, that it is the only point both completion paths pass through, that it runs after the body's locals are destroyed with only the frame storage left, that it must not throw (the type says so) and must not touch the task, and that a caller wanting a per-task notification *inside* the body uses an `alt::scope_exit` local instead, which also runs on both paths. Add `completion_handler` to the `## Interface` block and to the README row.

- [ ] **Step 8: Commit**

```bash
git add include/alt/cancellable_task.hpp tests/test_cancellable_task.cpp docs/cancellable_task.md README.md
git commit -m "Add a completion hook to alt::start_options"
```

---

### Task 2: `detail::completion_counter`

The reusable accounting the spec's designer call #1 asks for: a count plus a joinable waiter list, in `detail/` so phase 4's `when_all` can share it.

**Files:**
- Create: `include/alt/detail/completion_counter.hpp` (header-only, matching `detail/waiter_list.hpp`'s precedent)
- Create: `tests/test_completion_counter.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `detail::waiter_list` (`link(node, Condition)`, `detach_all()`, `size()`, `empty()`), `detail::waiter_node`, `detail::resume_claimed(waiter_node*, bool)`, `detail::wait_awaiter<Derived>` with its `ready()`/`list()` contract, `alt::wait_result`.
- Produces:
  ```cpp
  namespace alt::detail {
      class completion_counter
      {
      public:
          completion_counter() noexcept = default;
          ~completion_counter();
          completion_counter(const completion_counter&)            = delete;
          completion_counter(completion_counter&&)                 = delete;
          completion_counter& operator=(const completion_counter&) = delete;
          completion_counter& operator=(completion_counter&&)      = delete;

          void add(std::size_t n = 1) noexcept;
          void complete() noexcept;
          [[nodiscard]] std::size_t outstanding() const noexcept;

          class join_awaiter: public wait_awaiter<join_awaiter>
          {
          public:
              explicit join_awaiter(completion_counter& counter) noexcept;
              [[nodiscard]] bool ready() const noexcept;        // outstanding() == 0
              [[nodiscard]] waiter_list& list() const noexcept;
          private:
              completion_counter* m_counter;
          };

          [[nodiscard]] join_awaiter join() noexcept;
      };
  }
  ```
  Awaiting `join()` yields `alt::wait_result`. Task 4 holds one of these by value and forwards `join()` through it.

- [ ] **Step 1: Write the failing tests**

`tests/test_completion_counter.cpp`, with a fixture in the style of `tests/test_async_event.cpp`'s and the same `alt_test` support header.

```cpp
static_assert(!std::is_copy_constructible_v<alt::detail::completion_counter>);
static_assert(!std::is_move_constructible_v<alt::detail::completion_counter>);
static_assert(std::is_same_v<alt::await_result_t<alt::detail::completion_counter::join_awaiter>, alt::wait_result>);

TEST_F(CompletionCounterTest, JoiningAnIdleCounterIsReadyWithoutSuspending)
TEST_F(CompletionCounterTest, AddMakesTheCounterOutstanding)          // outstanding() == 1
TEST_F(CompletionCounterTest, AddTakesACountGreaterThanOne)           // add(3); outstanding() == 3
TEST_F(CompletionCounterTest, JoiningABusyCounterSuspendsUntilTheLastCompletion)
TEST_F(CompletionCounterTest, CompleteBelowTheLastOneReleasesNoJoiner) // add(2), complete(), joiner still parked
TEST_F(CompletionCounterTest, ReachingZeroReleasesEveryJoiner)         // three joiners, all satisfied
TEST_F(CompletionCounterTest, ASatisfiedJoinReportsSuccess)            // result.has_value()
TEST_F(CompletionCounterTest, AddAfterReachingZeroMakesItBusyAgain)    // not single-use, unlike async_latch
TEST_F(CompletionCounterTest, DestroyingACounterReleasesItsJoinersUnsatisfied)
    EXPECT_EQ(record.result.error(), std::make_error_code(std::errc::operation_canceled));
TEST_F(CompletionCounterTest, ACancelledJoinerIsReleasedWithOperationCanceled)
    // joiner inside a cancellable_task; request_stop while parked
TEST_F(CompletionCounterTest, CompletionFromAnotherThreadReleasesTheJoiner)
```

Review Focus item 3, which needs a mechanism rather than repetition — a `std::latch` or two atomics releasing both threads immediately before the contended operation, in the shape `tests/test_waiter_list.cpp` already uses:

```cpp
TEST_F(CompletionCounterTest, AJoinerLinkingAsTheCountReachesZeroIsNeverLost)
// repeat race_repeats times: one thread rendezvous then calls complete() on a
// count of 1; the other rendezvous then awaits join(). Assert the joiner always
// finishes satisfied -- parked and detached, or never parked at all.
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R CompletionCounter`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement `completion_counter` in `include/alt/detail/completion_counter.hpp`**

The count is a `std::atomic<std::size_t>`; the joiners live in a `waiter_list`.

`complete()` decrements first and takes the list's mutex only when the decrement reached zero, with an assert that the count was not already zero (`"completing an alt::detail::completion_counter with nothing outstanding"`). `detach_all()` then releases everything parked, and `resume_claimed(chain, true)` is the **last statement**: a resumed joiner may destroy whatever owns this counter.

That needs the ordering argument spelled out as a `DELICATE` block, because it is the whole correctness of the type and it is not the shape `async_event::set` uses:

- `join_awaiter::ready()` is `outstanding() == 0`, and `wait_awaiter` consults it a second time under the list's mutex as the condition on the link. So a joiner parks only if it read a non-zero count while holding that mutex.
- A `complete()` that reached zero then acquires the same mutex. Either the joiner got there first — it may park, and this `detach_all` takes it — or `complete()` got there first, in which case the joiner's later acquisition synchronizes with this release, so it sees zero and does not park.
- There is no third interleaving, which is why no conditional detach is needed and why `detach_all` is called only on the zero transition.

Document the one thing this does not promise: an `add()` racing a `complete()` that reaches zero may release a joiner while new work is outstanding. An owner tearing down stops adding first.

The destructor releases parked joiners unsatisfied — `resume_claimed(m_joiners.detach_all(), false)` — for `async_event`'s reason, stated by reference to it rather than re-argued.

- [ ] **Step 4: Register the test and run it**

Add `test_completion_counter.cpp` to `alt_tests` in `tests/CMakeLists.txt`.
Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure -R CompletionCounter`
Expected: PASS.

- [ ] **Step 5: Mutation check on the contended test**

Replace `ready()`'s body with a plain `return false;` so a joiner always parks whatever the count says, and the link's second check can no longer save it. Run `AJoinerLinkingAsTheCountReachesZeroIsNeverLost` under `debug-tsan`. Record in the test's comment how many of `race_repeats` failed. If the number is zero, say so in the comment and say what the test does cover instead — a test that cannot fail under the defect it names is a comment, not a guard.

- [ ] **Step 6: Run the race under TSan**

Run: `cmake --preset debug-tsan && cmake --build --preset debug-tsan && ctest --preset debug-tsan --output-on-failure -R CompletionCounter`
Expected: PASS with no race reports.

- [ ] **Step 7: Commit**

```bash
git add include/alt/detail/completion_counter.hpp tests/test_completion_counter.cpp tests/CMakeLists.txt
git commit -m "Add detail::completion_counter, the joinable completion accounting"
```

---

### Task 3: `task_completion_source<T>`

**Files:**
- Create: `include/alt/task_completion_source.hpp` (header-only)
- Create: `tests/test_task_completion_source.cpp`
- Create: `docs/task_completion_source.md`
- Modify: `tests/CMakeLists.txt`, `README.md` (new row after the `async_mutex` row), `docs/superpowers/specs/2026-10-26-async-primitives-design.md` (revision note for departure 1 and 2)
- Modify: `docs/awaitable.md` (one cross-reference: `as_expected` is not meant to be composed over this)

**Interfaces:**
- Consumes: `detail::result_store<T>` (`set_value`, `set_exception`, `is_set`, `value() &&`), `detail::waiter_list`, `detail::wait_awaiter<Derived>`, `detail::resume_claimed`, `alt::wait_result`.
- Produces:
  ```cpp
  namespace alt {
      namespace detail {
          template<typename T> struct completion_value      { using type = T; };
          template<typename T> struct completion_value<T&>   { using type = std::reference_wrapper<T>; };
      }

      template<typename T>
      using completion_result = std::expected<typename detail::completion_value<T>::type, std::error_code>;

      template<typename T = void>
      class task_completion_source
      {
      public:
          using value_type = T;

          task_completion_source() noexcept = default;
          ~task_completion_source();
          task_completion_source(const task_completion_source&)            = delete;
          task_completion_source(task_completion_source&&)                 = delete;
          task_completion_source& operator=(const task_completion_source&) = delete;
          task_completion_source& operator=(task_completion_source&&)      = delete;

          template<typename U> void set_value(U&& value);  // T non-void, non-reference
          void set_value();                                 // T = void
          void set_value(T& value);                         // T = U&
          void set_exception(std::exception_ptr error);
          [[nodiscard]] bool is_set() const noexcept;

          class completion_awaiter: public detail::wait_awaiter<completion_awaiter>
          {
          public:
              explicit completion_awaiter(task_completion_source& source) noexcept;
              [[nodiscard]] bool ready() const noexcept;
              [[nodiscard]] detail::waiter_list& list() const noexcept;
              [[nodiscard]] completion_result<T> await_resume() const;
          private:
              task_completion_source* m_source;
          };

          [[nodiscard]] completion_awaiter operator co_await() noexcept;
      };
  }
  ```
  `completion_awaiter` is a nested class, so it reaches the source's private store directly; no `guard_for`-style accessor is needed. `await_resume` is not `noexcept`: it rethrows a stored exception.

- [ ] **Step 1: Write the failing tests**

`tests/test_task_completion_source.cpp`, fixture in the house style.

```cpp
static_assert(!std::is_copy_constructible_v<alt::task_completion_source<int>>);
static_assert(!std::is_move_constructible_v<alt::task_completion_source<int>>);
static_assert(std::is_same_v<alt::completion_result<int>, std::expected<int, std::error_code>>);
static_assert(std::is_same_v<alt::completion_result<void>, alt::wait_result>);
static_assert(std::is_same_v<alt::completion_result<int&>, std::expected<std::reference_wrapper<int>, std::error_code>>);

TEST_F(TaskCompletionSourceTest, AwaitingASourceSetBeforeTheAwaitIsReadyWithoutSuspending)
TEST_F(TaskCompletionSourceTest, AwaitingAnUnsetSourceSuspendsUntilSetValue)
TEST_F(TaskCompletionSourceTest, SetValueDeliversTheValue)          // *result == 42
TEST_F(TaskCompletionSourceTest, SetValueMovesTheValueOutOfTheSource)
    // a move-only payload; the awaiting coroutine observes the moved-in object
TEST_F(TaskCompletionSourceTest, AVoidSourceReportsSuccessWithNoValue)
TEST_F(TaskCompletionSourceTest, AReferenceSourceYieldsAReferenceToTheCallersObject)
    int target = 7;  /* set_value(target); */  int& seen = *result;
    EXPECT_EQ(std::addressof(seen), std::addressof(target));
TEST_F(TaskCompletionSourceTest, SetExceptionRethrowsFromAwaitResume)
    EXPECT_THROW(..., std::runtime_error);
TEST_F(TaskCompletionSourceTest, SetExceptionRethrowsOnTheAlreadySetPath)   // set before the await
TEST_F(TaskCompletionSourceTest, IsSetReportsWhetherEitherChannelWasFilled)
TEST_F(TaskCompletionSourceTest, SettingFromAnotherThreadResumesTheWaiter)
TEST_F(TaskCompletionSourceTest, DestroyingASourceReleasesItsWaiterUnsatisfied)
    EXPECT_EQ(record.result.error(), std::make_error_code(std::errc::operation_canceled));
TEST_F(TaskCompletionSourceTest, ACancelledWaiterIsReleasedWithOperationCanceled)
TEST_F(TaskCompletionSourceTest, AWaiterResumedBySetValueMayDestroyTheSource)
    // the awaiting body destroys a heap-allocated source after reading the value
```

The set-twice and await-twice paths are debug asserts. If the suite already uses
death tests, each gets one; if not, say so in a comment in the test file rather
than leaving the gap silent — they are documented preconditions exercised by no
test, and the release-build no-op on a second set is not reachable from a `Debug`
preset at all.

Review Focus items 1 and 2, both needing a rendezvous rather than repetition:

```cpp
TEST_F(TaskCompletionSourceTest, ASetValueLandingAsTheWaiterLinksIsNeverLost)
// repeat race_repeats times: one thread rendezvous then set_value, the other
// rendezvous then awaits. The waiter always finishes satisfied with the value.

TEST_F(TaskCompletionSourceTest, AStopRacingSetValueResumesTheWaiterExactlyOnce)
// rendezvous, then request_stop on one thread and set_value on the other.
// Assert resumptions == 1, and that a waiter reporting success carries the value
// while one reporting operation_canceled carries no value.
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R TaskCompletionSource`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement `task_completion_source<T>` in `include/alt/task_completion_source.hpp`**

`ready()` reads an atomic flag, as the link condition too, so it takes no lock and resumes nothing — the same shape as `async_event::event_awaiter::ready()`.

**Superseded (2026-11-14, task 3 implementation).** This step first read: "`ready()` is `m_source->is_set()`". That is a data race. `detail::result_store::is_set()` reads a `std::variant` index (or, in the `void` specialization, a plain `bool`) non-atomically, and the setter writes it, so a waiter calling it concurrently with `set_value` is UB. The source therefore carries its own `std::atomic<bool>`, release-stored last inside the critical section and read by `ready()`, which is what `async_event` actually does — `is_set()` there is an atomic load, and this step paraphrased it wrongly.

The setters share one private helper that fills the store and detaches the waiter as **one critical section**, the way `async_event::set` does, so the publish has no order to argue about: `detach_one([&]() noexcept { /* fill m_result */ })`, then `resume_claimed(chain, true)` as the last statement. `detach_one` rather than `detach_all`, because a source is awaited once; a second parked waiter is not reachable through the documented API.

Set-twice is a debug assert and a release no-op, checked inside that same critical section so two threads cannot both pass it: `assert(!m_result.is_set() && "setting an alt::task_completion_source twice")`.

Await-twice is a debug assert in `operator co_await` — `await_resume` moves out of the store, so a second await would read a moved-from value.

`await_resume` takes the verdict from the base and only then decides what success is worth, hiding rather than extending it, exactly as `async_mutex::mutex_awaiter::await_resume` does:

```cpp
const wait_result result = detail::wait_awaiter<completion_awaiter>::await_resume();
if(!result) { return completion_result<T>{std::unexpect, result.error()}; }
// otherwise read m_source->m_result: value() rethrows a stored exception, and
// std::move(...).value() moves a T out. T& wraps in std::reference_wrapper; void
// calls value() for its rethrow and returns {}.
```

The destructor releases parked waiters unsatisfied, `resume_claimed(m_waiters.detach_all(), false)`, citing `async_event`'s destructor for why it is done here rather than left to the member.

- [ ] **Step 4: Register the test and run it**

Add `test_task_completion_source.cpp` to `alt_tests`.
Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure -R TaskCompletionSource`
Expected: PASS.

- [ ] **Step 5: Mutation check**

For `ASetValueLandingAsTheWaiterLinksIsNeverLost`, replace the single critical section with a fill-then-detach pair (fill the store, drop the lock, take it again to detach) and record how many of `race_repeats` fail. For `AStopRacingSetValueResumesTheWaiterExactlyOnce`, make the stop callback resume unconditionally instead of only on `claim_result::detached`, and record the same. Write both measured numbers into the tests' comments; a zero goes in as a zero, with what the test does cover said plainly.

- [ ] **Step 6: Run under ASan and TSan**

Run: `ctest --preset debug-asan --output-on-failure -R TaskCompletionSource` and the same for `debug-tsan`.
Expected: PASS, no sanitizer reports.

- [ ] **Step 7: Document it**

`docs/task_completion_source.md`, following `docs/async_event.md`'s shape: the callback-bridge example, the interface block, set once and awaited once, the thread the waiter resumes on, and three sections carrying what a caller cannot guess —

- **The exception channel is deliberately not the error channel.** A caller's exception rethrows from the await; `operation_canceled` travels in the `expected`. This is a second convention alongside `as_expected`'s `std::expected<T, std::exception_ptr>`, and `as_expected` is not meant to be composed over this type. Say why: one channel for both would force an `exception_ptr` error type across every primitive in the phase, or swallow user exceptions into error codes.
- **`T&` yields a `std::reference_wrapper`**, because `std::expected<T&, E>` is ill-formed in C++23.
- **A source never set, never cancelled and never destroyed parks its waiter forever** — the hazard the spec names, with the three ways out.

Add the README row, the `docs/awaitable.md` cross-reference, and a revision note in the spec recording both departures from the plan's **Departures** section above.

- [ ] **Step 8: Commit**

```bash
git add include/alt/task_completion_source.hpp tests/test_task_completion_source.cpp tests/CMakeLists.txt docs/task_completion_source.md docs/awaitable.md README.md docs/superpowers/specs/2026-10-26-async-primitives-design.md
git commit -m "Add alt::task_completion_source, the bridge from a callback to an await"
```

---

### Task 4: `task_group`

**Files:**
- Create: `include/alt/task_group.hpp`, `src/task_group.cpp`
- Create: `tests/test_task_group.cpp`
- Create: `docs/task_group.md`
- Modify: `tests/CMakeLists.txt`, `README.md`, `docs/cancellable_task.md` (one cross-reference to the group as the teardown point)

**Interfaces:**
- Consumes: Task 1's `alt::completion_handler` and `start_options::on_completion`; Task 2's `detail::completion_counter` (`add`, `complete`, `outstanding`, `join`); `alt::start`, `alt::cancellable_task`, `alt::cancellation_handle`, `alt::wait_result`.
- Produces:
  ```cpp
  namespace alt {
      class task_group
      {
      public:
          task_group();
          ~task_group();
          task_group(const task_group&)            = delete;
          task_group(task_group&&)                 = delete;
          task_group& operator=(const task_group&) = delete;
          task_group& operator=(task_group&&)      = delete;

          cancellation_handle spawn(cancellable_task t, const start_options& options = {});
          bool request_stop() noexcept;
          [[nodiscard]] bool stop_requested() const noexcept;
          [[nodiscard]] std::size_t outstanding() const noexcept;
          [[nodiscard]] detail::completion_counter::join_awaiter join() noexcept;

      private:
          static void on_task_complete(void* state) noexcept;
          detail::completion_counter m_counter;
          std::stop_source           m_source;
      };
  }
  ```
  `spawn` is deliberately not `[[nodiscard]]`: the group is the handle an owner keeps, so dropping the per-task one is the common case. `join()` is inline in the header; the header includes `<alt/detail/completion_counter.hpp>` because the group holds one by value.

- [ ] **Step 1: Write the failing tests**

`tests/test_task_group.cpp`, fixture in the house style.

```cpp
static_assert(!std::is_copy_constructible_v<alt::task_group>);
static_assert(!std::is_move_constructible_v<alt::task_group>);
static_assert(std::is_same_v<alt::await_result_t<decltype(std::declval<alt::task_group&>().join())>, alt::wait_result>);

TEST_F(TaskGroupTest, SpawnStartsTheTaskImmediately)
TEST_F(TaskGroupTest, OutstandingCountsLiveTasks)                  // two parked tasks; == 2
TEST_F(TaskGroupTest, ATaskThatCompletesDecrementsTheCount)
TEST_F(TaskGroupTest, JoiningAnEmptyGroupIsReadyWithoutSuspending)
TEST_F(TaskGroupTest, JoinCompletesWhenTheLastTaskFinishes)
TEST_F(TaskGroupTest, RequestStopCancelsEveryTask)                  // both tasks tear down
TEST_F(TaskGroupTest, StopThenJoinIsTheTeardownSequence)
    // the spec's order: request_stop(), co_await join(), then destroy. outstanding() == 0
TEST_F(TaskGroupTest, ATaskParkedOnAPrimitiveIsReleasedByRequestStopAndJoinCompletes)
    // the circularity revision 1 of the spec created: an async_event nobody sets
TEST_F(TaskGroupTest, TheReturnedHandleCancelsOnlyThatTask)
TEST_F(TaskGroupTest, StopRequestedReportsTheGroupsOwnState)
TEST_F(TaskGroupTest, SpawnAfterAJoinHasBegunIsCounted)
TEST_F(TaskGroupTest, SeveralJoinersAreAllReleased)
TEST_F(TaskGroupTest, AnEscapedExceptionStillReachesTheCallersHandler)
    // on_exception is the caller's field; the group must not overwrite it
```

Review Focus items 4 and 5:

```cpp
TEST_F(TaskGroupTest, ATaskCompletingSynchronouslyInsideSpawnLeavesTheCountCorrect)
// a body with no suspension point: the hook fires before spawn returns.
// outstanding() == the value before the spawn, and a joiner parked on an earlier
// task is not released.

TEST_F(TaskGroupTest, ACancelledJoinerIsReleasedWithOperationCanceledWhileTasksAreLive)
// joiner inside a cancellable_task; stop the joiner's own chain, not the group.
// The join reports operation_canceled and outstanding() is still non-zero --
// which is the state the destructor's assert exists to catch.
```

And the concurrent-spawn race from Review Focus item 3's neighborhood:

```cpp
TEST_F(TaskGroupTest, ASpawnRacingACompletionNeverUndercountsOrOvercounts)
// rendezvous, then spawn on one thread while a task completes on another;
// repeat race_repeats times and assert outstanding() settles at the expected value
```

The destructor's assert gets a death test if the suite already uses one; otherwise state in the test file that it is a documented debug precondition exercised by no test, rather than leaving the omission silent.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R TaskGroup`
Expected: FAIL — no such header.

- [ ] **Step 3: Implement `task_group` in `include/alt/task_group.hpp` and `src/task_group.cpp`**

`spawn` copies the caller's options, fills in the two fields the group owns, raises the count, and starts the task:

- `assert(!options.token.stop_possible() && "alt::task_group supplies its own stop token")` — the group's token goes in `token`. A caller with an external signal registers their own `std::stop_callback` on it calling `request_stop()`.
- `assert(options.on_completion.invoke == nullptr && "alt::task_group owns start_options::on_completion")` — the group's hook goes in `on_completion`. A caller wanting their own uses an `alt::scope_exit` local in the body.
- `home` and `on_exception` pass through untouched.
- `m_counter.add()` happens **before** `start`, because a body with no suspension point completes inside `start` and fires the hook there.
- The `start` call is the **last statement**: `return start(std::move(t), options_copy);`. Nothing may read `*this` afterwards — a hook firing inside `start` can resume a joiner whose body destroys this group. A `DELICATE` block says so, citing `async_event::set`'s last-statement rule as the family's existing form of it.

`on_task_complete` is the static trampoline: `static_cast<task_group*>(state)->m_counter.complete()`.

`~task_group()` asserts `outstanding() == 0` with `"destroying an alt::task_group with live tasks; request_stop() and join() it first"`. It does not stop or wait: the group does not own the frames, and blocking would deadlock whenever a task needs the calling thread.

- [ ] **Step 4: Register the test and run it**

Add `test_task_group.cpp` to `alt_tests`. `src/task_group.cpp` is picked up by `GLOB_RECURSE` after a configure.
Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure -R TaskGroup`
Expected: PASS.

- [ ] **Step 5: Mutation check**

Move `m_counter.add()` to after the `start` call and record how many runs of `ATaskCompletingSynchronouslyInsideSpawnLeavesTheCountCorrect` and `ASpawnRacingACompletionNeverUndercountsOrOvercounts` fail. Record the measured numbers in the tests' comments.

- [ ] **Step 6: Run under ASan and TSan**

Run: `ctest --preset debug-asan --output-on-failure -R TaskGroup` and the same for `debug-tsan`.
Expected: PASS, no sanitizer reports.

- [ ] **Step 7: Document it**

`docs/task_group.md`, carrying what the spec's **Teardown order** and **Hazards** sections say, in a caller's terms:

- The three-step teardown — `request_stop()`, `co_await group.join()`, then destroy the primitives and the group — and *why* it is finite: releasing on stop is what removes the circularity, because a parked waiter is released by its own stop callback rather than by the primitive's destruction.
- **Declare the primitives before the group**, so reverse-order destruction destroys the group first. Note that neither sanitizer catches getting this wrong.
- The non-coroutine owner's route: `sync_wait(group.join(), loop)`, reentrant on the loop, and it **must catch**, because a `sync_wait` inside a work item that throws terminates. Give the sequence explicitly.
- The group supplies `token` and `on_completion`; `home` and `on_exception` are the caller's.
- Joining is required before destruction; a cancelled joiner leaves the group unjoined, which is the one way to reach the destructor's assert without a mistake, and the owner's answer is to join without being cancellable or to stop and join from a scope the cancellation does not reach.
- A spawn racing a join that completes may land after it; an owner tearing down stops spawning first.

Add the README row and a one-line cross-reference from `docs/cancellable_task.md` naming the group as the teardown point for a set of tasks.

- [ ] **Step 8: Commit**

```bash
git add include/alt/task_group.hpp src/task_group.cpp tests/test_task_group.cpp tests/CMakeLists.txt docs/task_group.md docs/cancellable_task.md README.md
git commit -m "Add alt::task_group, the owner's teardown point for spawned tasks"
```

---

### Task 5: Version bump to 1.9.0

A minor bump: stage B adds components and extends `start_options` additively, breaking nothing. Its own commit, on the branch, so CI verifies the new version before it reaches `main`.

**Files:** all six, and nothing else.
- Modify: `CMakeLists.txt:2` — `project(Alt VERSION 1.9.0 LANGUAGES CXX)`
- Modify: `include/alt/version.hpp` — `ALT_VERSION_MINOR` macro **and** the constexpr variables
- Modify: `src/version.cpp` — the returned string
- Modify: `tests/test_version.cpp` — the expected values **and the `static_assert`s**, which a grep for the version string does not find
- Modify: `README.md` — every occurrence of the version string
- Modify: `CLAUDE.md` — every occurrence of the version string

- [ ] **Step 1: Update `tests/test_version.cpp` first**

Both the `EXPECT_EQ`s and the `static_assert`s. Run `grep -n "1\.8\.0\|MINOR\|minor" tests/test_version.cpp` and change every hit; the `static_assert`s fail the *build*, so a missed one is a build error, not a test failure.

- [ ] **Step 2: Run the version test to verify it fails**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: a `static_assert` failure, or a `VersionTest` failure naming 1.8.0.

- [ ] **Step 3: Update the other five files**

Verify with `grep -rn "1\.8\.0" CMakeLists.txt include/ src/ tests/ README.md CLAUDE.md` returning nothing.

- [ ] **Step 4: Run the full suite**

Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: PASS, 0 failures.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.9.0"
```

---

## Before the pull request

All six gates, with clang-tidy on **CI's** toolchain:

```bash
for p in debug debug-asan debug-tsan release-tests; do
    cmake --preset "$p" && cmake --build --preset "$p" && ctest --preset "$p" --output-on-failure
done
cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_C_COMPILER=clang-19 \
    -DCMAKE_CXX_COMPILER=clang++-19 -DCLANG_TIDY_PATH=/usr/bin/clang-tidy-19
cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
clang-format --dry-run --Werror <every changed .hpp and .cpp>
```

`cppcheck` covers `src/*.cpp` only, so `src/task_group.cpp` is in its scope and the three new headers are not.

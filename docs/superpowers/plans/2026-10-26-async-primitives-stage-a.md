# Async Primitives Stage A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the shared cancellable waiter mechanism and the four waiting primitives on top of it: `async_event`, `async_latch`, `async_semaphore`, `async_mutex`.

**Architecture:** One intrusive doubly-linked waiter list with an atomic per-node claim word decides the single resumer for each waiter, so a satisfier and a stop callback can never both resume one frame. A separate awaiter base reaches the awaiting promise's `cancellation_context` by templating `await_suspend` on `Promise`, registers a `std::stop_callback`, and links the node — so cancellation support is written once and the four primitives are thin.

**Tech Stack:** C++23, GCC 13+ / Clang 19+. GoogleTest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-10-26-async-primitives-design.md`

## Global Constraints

- **Branch:** `feature/async-primitives`, rebased on `main` at or after PR #8. Never push to `main`; work reaches it through a pull request.
- **Language:** C++23, within the GCC 13+ / Clang 19+ intersection.
- **Namespace:** public symbols in `namespace alt`, internals in `namespace alt::detail`.
- **Naming:** `snake_case` for variables, functions, types and files. Template typenames `PascalCase`. Private and protected members prefixed `m_`.
- **Const by default:** declare variables `const` unless mutation is required.
- **Doxygen:** `/** */` on every public function, class, struct and concept, including those in `detail`. Delicate functions carry a `DELICATE. Read this before changing it.` block, following `cancellation.hpp:50`.
- **Error handling:** every wait reports satisfied-or-released as `std::expected<…, std::error_code>`, using `std::make_error_code(std::errc::operation_canceled)` for released. `await_resume` is `[[nodiscard]]` wherever ignoring the result would proceed as though the wait succeeded.
- **Spelling:** American English. The only exception is the `cancelled`, `cancelling`, `cancellable`, `cancellation` family, which is correct here.
- **Formatting:** run `clang-format -i` on every changed file before committing. Never hand-format.
- **Lint:** `tests/.clang-tidy` is appended to, never rewritten. Fix findings at the source, or add a narrowly scoped `NOLINTNEXTLINE(check-name)` whose comment states the real argument, not the check's name.
- **Waiters resume on the thread that satisfied them.** No primitive captures a scheduler; a caller hops with `resume_on`.
- **No primitive ever calls `destroy()` on a handle it was given.** Only `abandon_chain` destroys a chain, and only through the root.
- **Tests:** register every new test file in the `alt_tests` sources list in `tests/CMakeLists.txt`.
- **Commit messages** end with: `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`

## Verification commands

Run the first two after every task. Run all of them before the pull request.

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure

cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure

cmake --preset release-tests && cmake --build --preset release-tests
ctest --preset release-tests --output-on-failure

cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
```

**ThreadSanitizer is a gate for this stage, not a nicety.** The claim word is the whole correctness argument and it is concurrent; AddressSanitizer does not find data races.

Baseline on `main`: 553 tests, 0 failures on all four presets, one skip in the Debug presets (`Task.DeepChainRunsInConstantStack`, by design) and zero under `release-tests`, clang-tidy 0 findings, cppcheck exit 0.

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `include/alt/detail/waiter_list.hpp` | The node, the claim protocol, the intrusive list, and the lock-free resumption walk. No cancellation knowledge, no coroutine policy. | 1 |
| `include/alt/detail/async_wait.hpp` | The awaiter base: reaches the awaiting promise's cancellation context, registers the stop callback, links the node. Written once, used by all four primitives. | 2 |
| `include/alt/async_event.hpp` + `src/async_event.cpp` | Manual-reset event. First user of the awaiter base. | 2 |
| `include/alt/async_latch.hpp` + `src/async_latch.cpp` | Single-use count-down latch. | 3 |
| `include/alt/async_semaphore.hpp` + `src/async_semaphore.cpp` | Counting semaphore. | 4 |
| `include/alt/async_mutex.hpp` + `src/async_mutex.cpp` | Mutex and its RAII guard. | 5 |
| `tests/test_waiter_list.cpp` | The mechanism, directly, including the claim protocol under threads. | 1 |
| `tests/test_async_event.cpp` | The event, plus the cancellation path every primitive shares. | 2 |
| `tests/test_async_latch.cpp` | The latch. | 3 |
| `tests/test_async_semaphore.cpp` | The semaphore, including that a released waiter holds no permit. | 4 |
| `tests/test_async_mutex.cpp` | The mutex, including that a released waiter cannot enter the critical section. | 5 |
| `docs/async_event.md`, `docs/async_latch.md`, `docs/async_semaphore.md`, `docs/async_mutex.md` | Component pages. | 6 |

The four primitives are compiled components, following phase 3's precedent: none has a template in its interface, so `<mutex>` and `<atomic>` stay out of consumers and the code lands in cppcheck's scope (`src/*.cpp` only). `waiter_list.hpp` and `async_wait.hpp` are header-only because `async_wait.hpp` is a template.

## Resolving what the design left open

- **Node layout and the claim word's states.** Three states, `linked` / `claimed` / `done`, as a `std::atomic<waiter_state>`. A single `compare_exchange_strong` from `linked` to `claimed` elects the one resumer, which is what makes a satisfier and a stop callback mutually exclusive rather than racing.
- **The resumption walk is lock-free, which is stronger than the spec claimed.** `detach_all` removes nodes from the list and marks them `claimed` under the lock; the walk then touches only node memory and never re-locks. A `claimed` or `done` node's destructor never locks either. So a resumed waiter destroying the primitive mid-walk is safe by construction, not merely by precondition. Task 1 pins this with a test.
- **`async_mutex` is standalone, not built on `async_semaphore`.** The guard needs ownership tracking the semaphore has no concept of, so sharing would be a false economy.
- **Cancellation lives in `async_wait.hpp`, not in `waiter_list.hpp`.** Keeping the list free of cancellation knowledge avoids an include edge to `detail/cancellation.hpp` from the lowest layer, and mirrors phase 3's reason for templating `work_queue`.

## Review Focus

Five conditions the spec implies that no primitive's own happy path exercises. Each has a test attached to the task that owns the code.

1. **A stop requested while a waiter is parked, with no satisfier ever arriving.** The waiter must be released with `operation_canceled` and its chain must tear down. Task 2.
2. **A stop and a satisfier racing for the same waiter.** Exactly one must resume it; the frame must not be resumed twice nor left parked. Task 1 and Task 2.
3. **A resumed waiter destroying the primitive that resumed it, while other waiters are still in the walk.** Must not touch freed memory. Task 1.
4. **A waiter released without the thing it waited for.** A released `async_mutex` waiter must not hold the lock; a released `async_semaphore` waiter must not hold a permit. Tasks 4 and 5.
5. **A primitive destroyed with waiters still parked.** Every waiter released, no frame leaked. Tasks 2 through 5.

---

### Task 1: The shared waiter list and claim protocol

**Files:**
- Create: `include/alt/detail/waiter_list.hpp`
- Test: `tests/test_waiter_list.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_waiter_list.cpp` to the `alt_tests` sources list)

**Interfaces:**
- Consumes: nothing from this plan. `<atomic>`, `<coroutine>`, `<cstddef>`, `<mutex>`.
- Produces:
  ```cpp
  namespace alt::detail
  {
      enum class waiter_state : int { linked, claimed, done };

      struct waiter_node
      {
          waiter_node*              prev{nullptr};
          waiter_node*              next{nullptr};
          std::coroutine_handle<>   handle{};
          std::atomic<waiter_state> state{waiter_state::linked};
          bool                      satisfied{false};
      };

      class waiter_list
      {
      public:
          waiter_list() = default;
          ~waiter_list();
          waiter_list(const waiter_list&) = delete;
          waiter_list& operator=(const waiter_list&) = delete;
          waiter_list(waiter_list&&) = delete;
          waiter_list& operator=(waiter_list&&) = delete;

          void link(waiter_node& node);
          bool claim(waiter_node& node);
          waiter_node* detach_all() noexcept;
          waiter_node* detach_one() noexcept;
          [[nodiscard]] std::size_t size() const;
          [[nodiscard]] bool empty() const;
      };

      void resume_claimed(waiter_node* chain, bool satisfied) noexcept;
  }
  ```
  `claim` returns true when this caller won the election and now owns the resumption; it unlinks the node before returning true. `detach_all` and `detach_one` return a chain linked through `next` with every node already `claimed` and unlinked. `resume_claimed` walks that chain, writing `satisfied`, storing `done`, and resuming each handle, taking no lock.

- [ ] **Step 1: Write the failing tests**

In `tests/test_waiter_list.cpp`, namespace-less GoogleTest cases named `WaiterList.*`. Use `std::noop_coroutine()` for handles where a test only needs identity, and a small eager test coroutine where a test needs a real resumption. Cases and their assertions:

- `LinkingThenDetachingReturnsTheNodesInFifoOrder` — link three nodes, `detach_all`, assert the chain order is the link order and `size() == 0`.
- `ClaimElectsExactlyOneWinner` — two threads both call `claim` on one node; assert exactly one returns true. Repeat 100 times. **This is the test that fails if `claim` uses a load-then-store instead of a compare-exchange.**
- `ADetachedNodeIsAlreadyClaimedSoClaimLoses` — `detach_all`, then `claim` the detached node; assert false.
- `ClaimUnlinksSoSizeDrops` — link two, claim one, assert `size() == 1`.
- `ResumeClaimedWritesSatisfiedAndMarksDone` — assert `satisfied` and `state == done` after `resume_claimed(chain, true)`.
- `ResumeClaimedResumesEveryHandleOnce` — three real coroutines counting their resumptions; assert each ran exactly once. **Fails against a walk that stops at the first node.**
- `TheWalkTakesNoLockSoAWaiterMayDestroyTheListMidWalk` — Review Focus 3. A chain of three; the first node's resumption destroys a heap-allocated `waiter_list`; assert the remaining two still resume and nothing faults. Run under ASan and TSan.
- `DestroyingTheListWithLinkedNodesLeavesNoneLinked` — Review Focus 5 at this layer; assert every node's state is `done` after the list dies.
- `AConcurrentSatisfierAndStopDoNotBothResume` — Review Focus 2 at this layer; one thread `detach_all`s while another `claim`s the same node, 100 repeats; assert total resumptions equal node count exactly.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R WaiterList`
Expected: build failure, `'waiter_list.hpp' file not found`.

- [ ] **Step 3: Write `include/alt/detail/waiter_list.hpp`**

Implement the interface above. The mutex guards only the list links and `size`. `claim` is a `compare_exchange_strong` from `linked` to `claimed`, and only the winner takes the lock to unlink. `resume_claimed` must read each node's `next` into a local **before** resuming that node, because resuming destroys the frame the node lives in. Carry a `DELICATE` block on `resume_claimed` stating that it takes no lock, that this is what makes re-entrant destruction safe, and that the `next` read must precede the resume.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R WaiterList`
Expected: all pass, output pristine.

- [ ] **Step 5: Mutation-check the two load-bearing tests**

Replace `claim`'s compare-exchange with a load-then-store; confirm `ClaimElectsExactlyOneWinner` fails. Revert. Move the `next` read in `resume_claimed` to after the resume; confirm `ResumeClaimedResumesEveryHandleOnce` fails under ASan. Revert. Record both in the task report.

- [ ] **Step 6: Run the full gates**

Run the debug and debug-tsan presets in full, then clang-tidy and cppcheck. Expected: 553 + new tests, 0 failures; 0 tidy findings; cppcheck exit 0.

- [ ] **Step 7: Commit**

```bash
git add include/alt/detail/waiter_list.hpp tests/test_waiter_list.cpp tests/CMakeLists.txt
git commit -m "Add the shared waiter list and its claim protocol"
```

---

### Task 2: The cancellable awaiter base, and `async_event`

**Files:**
- Create: `include/alt/detail/async_wait.hpp`, `include/alt/async_event.hpp`, `src/async_event.cpp`
- Test: `tests/test_async_event.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `waiter_node`, `waiter_list`, `claim`, `detach_all`, `resume_claimed`. `alt::detail::cancellation_context` from `include/alt/detail/cancellation.hpp`.
- Produces:
  ```cpp
  namespace alt::detail
  {
      /** A promise that relays an alt cancellation context to its children. */
      template<typename Promise>
      concept relays_cancellation = requires(Promise& p) {
          { p.cancellation_context_pointer() } -> std::same_as<cancellation_context*>;
      };

      /**
       * Awaiter base for a primitive that parks a waiter in a waiter_list.
       * Derived supplies: bool ready() const; and waiter_list& list();
       */
      template<typename Derived>
      class wait_awaiter;
  }

  namespace alt
  {
      class async_event
      {
      public:
          explicit async_event(bool set = false) noexcept;
          ~async_event();
          // non-copyable, non-movable

          void set();
          void reset() noexcept;
          [[nodiscard]] bool is_set() const noexcept;
          [[nodiscard]] auto operator co_await() noexcept;   // awaits to std::expected<void, std::error_code>
      };
  }
  ```
  `wait_awaiter::await_suspend` is `template<typename Promise>` taking `std::coroutine_handle<Promise>`, so that `Promise = void` still satisfies `alt::awaitable`. When `relays_cancellation<Promise>` holds, it reads the context, registers a `std::stop_callback` whose callback `claim`s the node and, on winning, resumes it with `satisfied = false`; otherwise it links without one. The stop callback is declared **after** the node in the awaiter, so it is destroyed first — the same ordering discipline `cancellable_task`'s promise documents, and it carries that reasoning in a comment.

- [ ] **Step 1: Write the failing tests**

`tests/test_async_event.cpp`, cases `AsyncEvent.*`. Reuse `tests/coroutine_test_support.hpp` (`frame_tracker`, `live_frames`, `destruction_order`, `reset_coroutine_test_state`) and `manual_scheduler` with `drain()` as the deterministic pump, asserting the drain count rather than only that something ran.

- `AwaitingASetEventIsReadyWithoutSuspending`
- `AwaitingAnUnsetEventSuspendsUntilSet`
- `SetReleasesEveryWaiter` — three waiters, one `set()`, assert all three resumed. **Fails against a walk that releases only the first.**
- `SetLeavesTheEventSetSoALaterAwaitIsReady`
- `ResetClearsIt`
- `ASatisfiedWaitReportsSuccess` — assert the `std::expected` has a value.
- `CancellingAParkedWaiterReleasesItWithOperationCanceled` — Review Focus 1. A real `cancellable_task` awaits the event; request the stop while parked; assert the wait reports `std::errc::operation_canceled` and that `live_frames` reaches 0 without the event ever being set. **This is the test that fails if the stop callback is not registered: the frame would stay parked and `live_frames` would never drop.**
- `ACancelledWaiterTearsDownTheWholeChain` — three-frame chain, assert `destruction_order == {3, 2, 1}`, which is the signature of the cascade actually recursing.
- `AStopAndASetDoNotBothResume` — Review Focus 2, end to end. 100 repeats with a stopper thread; assert the body ran exactly once.
- `DestroyingTheEventReleasesParkedWaiters` — Review Focus 5; assert `live_frames` reaches 0.
- `AStopRequestedBeforeTheAwaitStillReleasesTheWaiter` — the window the registration order closes: request the stop, then await the event, and assert the wait reports `operation_canceled` rather than parking forever. **Fails against registering the stop callback before linking**, where the immediate fire finds an unlinked node.
- `AwaitingFromACoroutineWithNoCancellationContextStillWorks` — a plain `task<T>` under `sync_wait`, proving the `relays_cancellation` branch is optional.
- `static_assert(alt::awaitable<alt::async_event&>)`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --preset debug --output-on-failure -R AsyncEvent`
Expected: build failure, `'alt/async_event.hpp' file not found`.

- [ ] **Step 3: Write `include/alt/detail/async_wait.hpp`**

Implement `relays_cancellation` and `wait_awaiter` per the interface block. `await_ready` forwards to `Derived::ready()`.

`await_suspend` does three things **in this exact order**: store the handle in the node, link the node, then register the stop callback. The order is load-bearing in both directions and a `DELICATE` block must say why:

- **Registration must come after linking.** `std::stop_callback` invokes its callback immediately on construction if the token has already had a stop requested. Register first and that callback claims a node that is not in any list yet, so `claim`'s unlink has nothing to unlink. Linking first means the immediate-fire case claims a properly linked node and takes the ordinary path.
- **Nothing may touch `*this` after registration.** Once the callback is live, another thread's stop can claim and resume the node, destroying the frame this awaiter lives in. Registration is therefore the last statement, the same last-statement discipline `resume_on` and `checked_resume_on` carry.

The window this ordering closes is real: the `cancellation_checked` wrapper already abandons before reaching an inner `await_suspend` when a stop is pending, but a stop landing between that check and this registration still has to be caught, and the immediate-fire behavior of `std::stop_callback` is what catches it.

The stop callback is also declared **after** the node in the awaiter, so it is destroyed first — a callback mid-flight must not outlive the node it claims. That is the same ordering discipline `cancellable_task`'s promise documented before PR #8 removed its own instance, and the comment should cite it so the reasoning is not lost twice.

- [ ] **Step 4: Write `include/alt/async_event.hpp` and `src/async_event.cpp`**

A `bool m_set` and a `waiter_list m_waiters` under the list's own lock. `set()` marks set, `detach_all()`, then `resume_claimed(chain, true)` — and reads nothing from `*this` afterwards, because a resumed body may have destroyed the event. The destructor detaches and resumes with `satisfied = false`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R AsyncEvent`
Expected: all pass.

- [ ] **Step 6: Mutation-check the cancellation test**

Remove the stop-callback registration from `wait_awaiter::await_suspend`; confirm `CancellingAParkedWaiterReleasesItWithOperationCanceled` fails rather than hangs. If it hangs instead of failing, the test is wrong — rewrite it so the failure is reported, not timed out. Revert and record in the report.

- [ ] **Step 7: Run the full gates, then commit**

```bash
git add include/alt/detail/async_wait.hpp include/alt/async_event.hpp src/async_event.cpp tests/test_async_event.cpp tests/CMakeLists.txt
git commit -m "Add async_event and the cancellable awaiter base"
```

---

### Task 3: `async_latch`

**Files:**
- Create: `include/alt/async_latch.hpp`, `src/async_latch.cpp`
- Test: `tests/test_async_latch.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's list, Task 2's `wait_awaiter`.
- Produces:
  ```cpp
  namespace alt
  {
      class async_latch
      {
      public:
          explicit async_latch(std::ptrdiff_t count);
          ~async_latch();
          // non-copyable, non-movable

          void count_down(std::ptrdiff_t n = 1);
          [[nodiscard]] bool try_wait() const noexcept;
          [[nodiscard]] auto operator co_await() noexcept;   // std::expected<void, std::error_code>
      };
  }
  ```

- [ ] **Step 1: Write the failing tests**

Cases `AsyncLatch.*`: `AwaitingAZeroLatchIsReady`; `CountingDownToZeroReleasesEveryWaiter` (three waiters, assert all resumed — fails against releasing only the first); `CountingDownPartiallyDoesNotRelease`; `CountDownByMoreThanOneReachesZero`; `TryWaitReflectsTheCount`; `CountingDownPastZeroIsHarmless`; `CancellingAParkedWaiterReleasesIt` (asserting `operation_canceled` and `live_frames == 0`); `DestroyingTheLatchReleasesParkedWaiters`; `ConstructingWithZeroIsImmediatelyReady`; `static_assert(alt::awaitable<alt::async_latch&>)`.

- [ ] **Step 2: Run to verify they fail** — `ctest --preset debug --output-on-failure -R AsyncLatch`, expected build failure.

- [ ] **Step 3: Implement the header and TU.** A `std::ptrdiff_t m_count` guarded by the list's lock, and the same detach-then-resume-outside-the-lock shape as `async_event`. Single use: it does not reset.

- [ ] **Step 4: Run to verify they pass.**

- [ ] **Step 5: Run the full gates, then commit**

```bash
git add include/alt/async_latch.hpp src/async_latch.cpp tests/test_async_latch.cpp tests/CMakeLists.txt
git commit -m "Add async_latch"
```

---

### Task 4: `async_semaphore`

**Files:**
- Create: `include/alt/async_semaphore.hpp`, `src/async_semaphore.cpp`
- Test: `tests/test_async_semaphore.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's list, Task 2's `wait_awaiter`.
- Produces:
  ```cpp
  namespace alt
  {
      class async_semaphore
      {
      public:
          explicit async_semaphore(std::ptrdiff_t initial);
          ~async_semaphore();
          // non-copyable, non-movable

          void release(std::ptrdiff_t n = 1);
          [[nodiscard]] bool try_acquire() noexcept;
          [[nodiscard]] std::ptrdiff_t available() const noexcept;
          [[nodiscard]] auto operator co_await() noexcept;   // std::expected<void, std::error_code>
      };
  }
  ```

- [ ] **Step 1: Write the failing tests**

Cases `AsyncSemaphore.*`: `AcquiringAnAvailablePermitIsReady`; `AcquiringWithNoPermitsSuspends`; `ReleaseResumesOneWaiterPerPermit` (three waiters, `release(2)`, assert exactly two resumed and one stays parked — **fails against a release that wakes every waiter**); `ReleaseResumesInFifoOrder`; `TryAcquireConsumesAPermit`; `TryAcquireFailsWithNone`; `AvailableReflectsReleases`; `AReleasedWaiterHoldsNoPermit` — Review Focus 4: cancel a parked waiter, assert its result is `operation_canceled` **and** that `available()` is unchanged, so the cancelled waiter consumed nothing; `CancellingAParkedWaiterReleasesIt`; `DestroyingTheSemaphoreReleasesParkedWaiters`; `static_assert(alt::awaitable<alt::async_semaphore&>)`.

- [ ] **Step 2: Run to verify they fail** — `ctest --preset debug --output-on-failure -R AsyncSemaphore`.

- [ ] **Step 3: Implement the header and TU.** A `std::ptrdiff_t m_permits` under the list's lock. `release(n)` claims up to `n` waiters with `detach_one` in a loop and adds any remainder to `m_permits`; a waiter claimed this way is handed its permit, so the count is not incremented and then decremented. A cancelled waiter is claimed by the stop callback instead and takes no permit.

- [ ] **Step 4: Run to verify they pass.**

- [ ] **Step 5: Mutation-check Review Focus 4.** Make the stop-callback path decrement `m_permits`; confirm `AReleasedWaiterHoldsNoPermit` fails. Revert and record.

- [ ] **Step 6: Run the full gates, then commit**

```bash
git add include/alt/async_semaphore.hpp src/async_semaphore.cpp tests/test_async_semaphore.cpp tests/CMakeLists.txt
git commit -m "Add async_semaphore"
```

---

### Task 5: `async_mutex`

**Files:**
- Create: `include/alt/async_mutex.hpp`, `src/async_mutex.cpp`
- Test: `tests/test_async_mutex.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's list, Task 2's `wait_awaiter`.
- Produces:
  ```cpp
  namespace alt
  {
      class async_mutex;

      /** Holds the lock. Only ever constructed holding it. */
      class async_mutex_lock
      {
      public:
          async_mutex_lock(async_mutex_lock&&) noexcept;
          async_mutex_lock& operator=(async_mutex_lock&&) noexcept;
          ~async_mutex_lock();
          async_mutex_lock(const async_mutex_lock&) = delete;
          async_mutex_lock& operator=(const async_mutex_lock&) = delete;

          void unlock() noexcept;
          [[nodiscard]] bool owns_lock() const noexcept;
      };

      class async_mutex
      {
      public:
          async_mutex() noexcept;
          ~async_mutex();
          // non-copyable, non-movable

          [[nodiscard]] std::expected<async_mutex_lock, std::error_code> try_lock();
          [[nodiscard]] auto operator co_await() noexcept;
          // awaits to std::expected<async_mutex_lock, std::error_code>
      };
  }
  ```
  The guard has no public constructor that does not hold the lock, so the fails-open state is unrepresentable rather than documented. A cancelled waiter yields `std::unexpected`, never an empty guard.

- [ ] **Step 1: Write the failing tests**

Cases `AsyncMutex.*`: `LockingAnUncontendedMutexIsReady`; `TheGuardUnlocksOnDestruction`; `ASecondLockerSuspendsUntilTheFirstUnlocks`; `UnlockHandsOwnershipToTheNextWaiterInFifoOrder`; `MovingTheGuardTransfersOwnership` (assert the moved-from guard's `owns_lock()` is false and does not double-unlock); `ExplicitUnlockIsIdempotent`; `TryLockSucceedsWhenFreeAndFailsWhenHeld`; `AReleasedWaiterCannotEnterTheCriticalSection` — Review Focus 4: cancel a parked locker, assert the result is `std::unexpected(operation_canceled)` and that a separate `try_lock` from the test still fails because the original holder still holds it, proving the cancelled waiter did not take ownership; `CancellingAParkedWaiterReleasesIt`; `DestroyingTheMutexReleasesParkedWaiters`; `AHandOffChainOfManyWaitersDoesNotRecurse` — queue 10,000 waiters, unlock once, assert all run and the stack does not grow, since phase 3 rejected an `inline_scheduler` on exactly this ground; `static_assert(alt::awaitable<alt::async_mutex&>)`.

- [ ] **Step 2: Run to verify they fail** — `ctest --preset debug --output-on-failure -R AsyncMutex`.

- [ ] **Step 3: Implement the header and TU.** A `bool m_held` under the list's lock. Unlock claims the next waiter with `detach_one` and hands ownership directly, leaving `m_held` true — it does not clear and re-contend. The hand-off **drains iteratively**: a guard destructor that resumes the next waiter must not nest, so unlocking loops over successive claims rather than resuming from inside a resumption. Carry a comment giving the stack-depth reason and citing the `inline_scheduler` precedent.

- [ ] **Step 4: Run to verify they pass.**

- [ ] **Step 5: Mutation-check the recursion test.** Make the hand-off resume inline from the guard destructor; confirm `AHandOffChainOfManyWaitersDoesNotRecurse` fails or crashes rather than passing. Revert and record.

- [ ] **Step 6: Run the full gates, then commit**

```bash
git add include/alt/async_mutex.hpp src/async_mutex.cpp tests/test_async_mutex.cpp tests/CMakeLists.txt
git commit -m "Add async_mutex"
```

---

### Task 6: Documentation

**Files:**
- Create: `docs/async_event.md`, `docs/async_latch.md`, `docs/async_semaphore.md`, `docs/async_mutex.md`
- Modify: `README.md` (component table), `docs/scheduler.md` (one cross-reference to the new primitives from the resumption-thread discussion)

Match `docs/run_loop.md` and `docs/thread_pool.md` for structure, voice and depth. Every code example must compile against the real headers; check each signature against the declaration.

Each page must carry these hazards, with the consequence named:

- [ ] **Step 1:** Waiters resume on the thread that satisfied them. Hop with `resume_on` if the continuation is thread-sensitive.
- [ ] **Step 2:** Ignoring a wait result means proceeding without the lock, the permit or the value. Say that `async_mutex` makes the bad state unrepresentable and the others rely on `[[nodiscard]]`.
- [ ] **Step 3:** Destroying a primitive while another thread is satisfying it is undefined. Destroying one with waiters releases them, running coroutine bodies during the destructor, so anything those bodies touch must outlive the primitive — the same declaration-ordering rule the schedulers carry, and equally invisible to both sanitizers.
- [ ] **Step 4:** Cancellation releases a parked waiter with `operation_canceled`; it does not acquire anything. A chain then tears down at its next suspension point.
- [ ] **Step 5:** `async_latch` is single use. `async_semaphore` acquire/release pairing is the caller's to get right; there is no RAII permit.
- [ ] **Step 6:** Run `grep -nEi 'behaviour|signalling|optimis|serialis|initialis|colour|licence' docs/async_*.md` and fix anything outside the approved `cancell*` family.
- [ ] **Step 7: Commit**

```bash
git add docs/async_event.md docs/async_latch.md docs/async_semaphore.md docs/async_mutex.md README.md docs/scheduler.md
git commit -m "Document the async synchronization primitives"
```

---

### Task 7: Version bump

**Files:**
- Modify: `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

A standalone commit, never folded into a feature or merge commit.

- [ ] **Step 1: Confirm the part to increase**

Four new public components that break nothing points at a minor bump, from 1.7.1 to 1.8.0. Confirm with the author before committing; the plan does not get to decide this.

- [ ] **Step 2: Change all six files.** `include/alt/version.hpp` holds the numbers as separate macros, so it does **not** appear in a text search for the old version — it is the easy one to miss, and the build still succeeds without it. Only the test catches it. Note the patch component returns to 0.

- [ ] **Step 3: Verify nothing was missed**

```bash
grep -rn "1\.7\.1" --exclude-dir=build --exclude-dir=.git --exclude-dir=.superpowers . | grep -v "docs/superpowers"
```
Must produce no output. Documents under `docs/superpowers/` recorded the version when written and must not change.

- [ ] **Step 4: Build, test, commit**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R Version
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.8.0"
```

---

## Final verification before the pull request

Run all six verification commands. Expected: 553 plus this stage's new tests, 0 failures on every preset; one skip in the Debug presets and zero under `release-tests`; clang-tidy 0 findings; cppcheck exit 0; `clang-format --dry-run -Werror` clean on every changed file.

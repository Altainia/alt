# File I/O phase 7a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Awaitable file operations in `alt`, with the thread-pool backend, that survive
a cancelled chain's teardown without a use-after-free.

**Architecture:** Three layers. `io_backend` is a virtual seam whose completion callback
runs on an I/O thread. `detail::io_operation` is a refcounted control block, never a
coroutine-frame local, that co-owns the buffer, the descriptor and the service's shared
state so all three outlive the frame cancellation destroys. `file_io`/`async_file`
return `alt::task<std::expected<T, std::error_code>>` and post every completion to a
caller-named scheduler.

**Tech Stack:** C++23, GoogleTest, CMake presets. No new external dependencies.

**Spec:** `docs/superpowers/specs/2026-11-20-awaitable-file-io-design.md`

## Global Constraints

- C++23. GCC 13+, Clang 19+, MSVC 19.50+.
- `snake_case` for everything except template typenames (`PascalCase`). Private and
  protected members prefixed `m_`.
- Declare variables `const` unless mutation is required.
- `std::expected` for recoverable errors, not exceptions or error codes.
- Doxygen `/** */` on every public function, class, struct, enum and union.
- **No new external dependency.** 7a adds no third-party library and no
  `find_dependency` to the package config.
- **No platform headers in public headers.** `windows.h`, `unistd.h`, `fcntl.h` and
  friends appear only in `src/*.cpp`. `native_handle` is spelled so that
  `include/alt/async_file.hpp` needs none of them.
- Every new test file is added to the `alt_tests` sources list in
  `tests/CMakeLists.txt`.
- `tests/.clang-tidy` is **append-only**; never rewrite it.
- The clang-tidy gate is `clang-tidy-19` with `clang++-19`, per CLAUDE.md's invocation.
- No version bump until Task 10. 7a is 1.15.0.

## Review Focus

Five conditions the spec implies and leaves unstated. Each line's test is added to the
task that owns the code.

1. **`max_in_flight == 0`** would acquire a permit that never exists and hang on the
   first operation forever. `create` must reject it. → Task 5.
2. **`size` greater than `buffer.size()`** in `read_at`/`write_at` would let the backend
   write past the buffer — the worst outcome in the component. Must be rejected before
   submission, not clamped silently. → Task 7.
3. **A zero-length read or write** must succeed with 0 bytes rather than hang, error, or
   submit a degenerate request. → Task 7.
4. **An empty `scheduler_ref` as `completion`.** `scheduler_ref::post` asserts on an
   empty reference, so the failure would surface at the first completion, on an I/O
   thread, far from the mistake. `create` must reject it. → Task 5.
5. **The home scheduler's `post` throwing** (a queue allocation failure) leaves a block
   that can never be resumed. The chain must be leaked deliberately, not crash and not
   resume on the wrong thread — `cpwf`'s `Handoff::post_home_locked` settles this the
   same way. → Task 4.

---

### Task 1: `io_buffer`

**Files:**
- Create: `include/alt/io_buffer.hpp`, `src/io_buffer.cpp`
- Create: `tests/test_io_buffer.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_io_buffer.cpp` to `alt_tests`)

**Interfaces:**
- Consumes: nothing.
- Produces: `alt::io_buffer` with
  `static std::expected<io_buffer, std::error_code> allocate(std::size_t)`,
  `static io_buffer borrow(std::span<std::byte>, std::shared_ptr<void>)`,
  `static io_buffer borrow_unsafe(std::span<std::byte>)`,
  `std::span<std::byte> bytes() const noexcept`, `std::size_t size() const noexcept`,
  `explicit operator bool() const noexcept` (false when default-constructed).
  Copyable and movable; copies share storage.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoBufferTest, AnAllocatedBufferHasTheRequestedSize)
TEST(IoBufferTest, ADefaultConstructedBufferIsEmptyAndFalsey)
TEST(IoBufferTest, AZeroSizeAllocationSucceedsAndIsEmpty)
TEST(IoBufferTest, CopiesShareStorageSoAWriteThroughOneIsSeenByTheOther)
TEST(IoBufferTest, AllocatedStorageOutlivesEveryCopyButTheLast)
TEST(IoBufferTest, BorrowHoldsItsKeepaliveForAsLongAsTheBufferLives)
TEST(IoBufferTest, BorrowUnsafeHoldsNothingAndStillExposesTheBytes)
```

`AllocatedStorageOutlivesEveryCopyButTheLast`: write a sentinel pattern through one
copy, destroy that copy, read it back through the other, assert the pattern.
`BorrowHoldsItsKeepaliveForAsLongAsTheBufferLives`: pass a
`std::shared_ptr<int>`, assert `use_count() == 2` while the buffer lives and `1` after
it is destroyed.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug`
Expected: FAIL — `alt/io_buffer.hpp` not found.

- [ ] **Step 3: Implement `io_buffer`**

A single `std::shared_ptr<std::byte[]>` plus a `std::span<std::byte>`, which covers all
three factories: `allocate` makes the `shared_ptr` own new storage, `borrow` puts the
caller's keepalive in it with a no-op deleter aliasing, `borrow_unsafe` leaves it null.
`allocate` returns `std::unexpected(std::make_error_code(std::errc::not_enough_memory))`
on a failed allocation rather than throwing.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R IoBufferTest`
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/io_buffer.hpp src/io_buffer.cpp tests/test_io_buffer.cpp tests/CMakeLists.txt
git commit -m "Add alt::io_buffer, the refcounted I/O buffer"
```

---

### Task 2: `io_backend` and `manual_io_backend`

**Files:**
- Create: `include/alt/io_backend.hpp`, `src/io_backend.cpp`
- Create: `tests/test_io_backend.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `io_buffer`.
- Produces:
  - `enum class alt::io_backend_kind : std::uint8_t { automatic, thread_pool, io_uring, iocp, manual }`
  - `enum class alt::io_op : std::uint8_t { read, write, sync_full, sync_data, stat_handle, stat_path, open, rename, unlink, list_directory, invoke }`
  - `struct alt::io_request { io_op op; native_handle handle; io_buffer buffer; std::uint64_t offset; std::size_t size; const void* in; void* out; }`
    — `in` points at operation-specific input the request does not model (a path pair
    for `rename`, an `io_invocable*` for `invoke`); `out` points at storage for a result
    that is not a byte count (a `file_status` for the two stat ops, a
    `std::vector<directory_entry>` for `list_directory`). **Both point into the
    operation block, never into a coroutine frame**, which is what keeps them valid when
    a completion lands after the frame is gone.
  - `struct alt::io_completion { std::size_t bytes; std::error_code error; }`
    — `bytes` is meaningful for `read`/`write` only; every other operation reports
    through `error` and, where it has one, the request's `out`.
  - `using alt::io_completion_fn = void (*)(void* state, io_completion result) noexcept`
  - `class alt::io_token` — opaque, default-constructible-as-empty, copyable.
  - `class alt::io_backend` with the three pure virtuals from the spec's contract.
  - `class alt::manual_io_backend : public io_backend` adding
    `std::size_t pending() const`, `void complete(std::size_t index, io_completion)`,
    `void complete_inline_next(io_completion)`, `bool was_cancelled(std::size_t index) const`,
    `void fail_next_submission(std::error_code)`, and
    `const io_request& request(std::size_t index) const` — the last so a test can reach
    the submitted request's `buffer` and write into it, which is how the
    buffer-survives-teardown test stands in for a backend's late write.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoBackendTest, ASubmittedOperationIsPendingUntilCompleted)
TEST(IoBackendTest, CompletingRunsTheCallbackExactlyOnceWithTheGivenResult)
TEST(IoBackendTest, CompleteInlineNextRunsTheCallbackBeforeSubmitReturns)
TEST(IoBackendTest, CancelIsRecordedButTheCallbackStillRunsExactlyOnce)
TEST(IoBackendTest, CancelThenCompleteWithSuccessStillDeliversSuccess)
TEST(IoBackendTest, ManualBackendReportsItsKindAsManual)
TEST(IoBackendTest, ASubmissionConfiguredToFailReturnsAnErrorAndNeverRunsTheCallback)
```

`CancelThenCompleteWithSuccessStillDeliversSuccess` is the one that pins the spec's
"cancel is a hint": cancel, then complete with `io_completion{.bytes = 64}`, and assert
the callback saw 64 bytes and no error.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `alt/io_backend.hpp` not found.

- [ ] **Step 3: Implement the interface and the manual backend**

`manual_io_backend` stores submissions in a `std::vector` under a mutex, each with its
callback, state, and a cancelled flag. `complete` invokes the callback outside the lock.
`complete_inline_next(result)` arms the backend so the *next* `submit` invokes the
callback before returning, which is the inline-completion path the spec says backends
may take. A `fail_next_submission(std::error_code)` arms the error return.

Declare `native_handle` in `include/alt/async_file.hpp`? No — declare it here, in
`io_backend.hpp`, since `io_request` needs it and Task 7's header includes this one:
`using native_handle = std::intptr_t` on every platform, with `-1` as the invalid value,
so no platform header is required to spell it. Document that it holds an `int` fd on
POSIX and a `HANDLE` on Windows.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R IoBackendTest`
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/io_backend.hpp src/io_backend.cpp tests/test_io_backend.cpp tests/CMakeLists.txt
git commit -m "Add the alt::io_backend seam and a manual backend for tests"
```

---

### Task 3: `detail::io_handle`, the refcounted descriptor

**Files:**
- Create: `include/alt/detail/io_handle.hpp`, `src/io_handle.cpp`
- Create: `tests/test_io_handle.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2's `native_handle`.
- Produces: `alt::detail::io_handle` with
  `static std::shared_ptr<io_handle> adopt(native_handle)`,
  `native_handle native() const noexcept`,
  `bool is_open() const noexcept`,
  `void request_close() noexcept` (marks closed-by-request; does not close),
  `bool close_requested() const noexcept`,
  and a destructor that closes the descriptor if it is still open.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoHandleTest, TheDescriptorIsClosedWhenTheLastReferenceGoes)
TEST(IoHandleTest, TheDescriptorStaysOpenWhileASecondReferenceIsHeld)
TEST(IoHandleTest, RequestCloseMarksTheHandleWithoutClosingTheDescriptor)
TEST(IoHandleTest, ADescriptorIsNotReusedUnderAHeldReference)
```

`TheDescriptorStaysOpenWhileASecondReferenceIsHeld`: `adopt` a real fd from a temp
file, take a second `shared_ptr`, drop the first, then assert `::fcntl(fd, F_GETFD)`
still succeeds (POSIX) / `GetHandleInformation` still succeeds (Windows).

`ADescriptorIsNotReusedUnderAHeldReference` is Review-Focus-adjacent and the one that
earns this task. Per the spec: a merely-closed fd gives `EBADF`, which is benign; the
dangerous case is the *number* being reassigned so a read silently succeeds against the
wrong file. Hold a reference, open a second temp file, and assert the second file's
descriptor is a **different** number from the held one. Gate the whole test on POSIX,
since descriptor numbering is not a Windows concept.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `alt/detail/io_handle.hpp` not found.

- [ ] **Step 3: Implement `io_handle`**

`src/io_handle.cpp` holds the only `#include <unistd.h>` / `#include <windows.h>` in this
task, behind `#ifdef _WIN32` as `<alt/memory.hpp>` already does.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R IoHandleTest`
Expected: PASS, 4 tests (3 on Windows).

- [ ] **Step 5: Commit**

```bash
git add include/alt/detail/io_handle.hpp src/io_handle.cpp tests/test_io_handle.cpp tests/CMakeLists.txt
git commit -m "Add detail::io_handle so a cancelled frame cannot close a live descriptor"
```

---

### Task 4: The operation block and its awaiter

The centerpiece. Everything the spec calls hard is in this task.

**Files:**
- Create: `include/alt/detail/io_shared.hpp`, `include/alt/detail/io_operation.hpp`
- Create: `src/io_operation.cpp`
- Create: `tests/test_io_operation.cpp`
- Modify: `tests/coroutine_test_support.hpp` (see Step 1), `tests/CMakeLists.txt`,
  `tests/.clang-tidy` (append only, if the gate asks)

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `alt::detail::io_shared` holding `async_semaphore slots`, `io_backend* backend`,
    `scheduler_ref completion`, `thread_pool* workers`, plus
    `std::atomic<std::size_t> at_backend`, `std::atomic<bool> accepting`, and a
    `std::mutex`/`std::condition_variable` pair for `wait_until_quiet()`.
  - `alt::detail::io_operation` exactly as the spec declares it, with
    `enum class phase : std::uint8_t { waiting, done, detached }` and
    `static constexpr std::uint32_t alive_pattern`/`dead_pattern`.
  - `alt::detail::io_awaiter`, constructed from
    `(std::shared_ptr<io_shared>, io_request)`. It allocates the block and submits to
    the backend inside `await_suspend`, which is declared
    `template<typename Promise> bool await_suspend(std::coroutine_handle<Promise>)` so
    the `cancellation_ref` it builds is non-empty — an erased handle yields an empty
    view, which is the trap `docs/cancellation_ref.md` names. `io_completion
    await_resume()` yields the block's result; the calling coroutine turns that into the
    `std::expected` its own signature promises, so the backend's result shape stops at
    this boundary.

- [ ] **Step 1: Add a mutex-guarded destruction log to the test support header**

`tests/coroutine_test_support.hpp`'s `destruction_order` documents its own limit: *"A
test whose teardown can run on two threads at once — an awaiter that registers a
std::stop_callback on the chain's token, say, so that one branch tears down on the
requesting thread while the other tears down on the completing one — would race here,
and needs a mutex added before it can use this."* That is precisely this awaiter, so the
header must grow the facility before the tests can use it.

Add `alt_test::record_destruction(int tag)` and
`std::vector<int> alt_test::take_destruction_order()`, both guarded by a file-local
`std::mutex`, and have `reset_coroutine_test_state()` clear them. Leave the existing
unsynchronized `destruction_order` and its comment alone — other suites rely on it —
and extend the comment to point at the new pair as the answer for two-thread teardowns.

- [ ] **Step 2: Write the failing tests**

The spec's ordering table, one test per row, each driven by `manual_io_backend` so the
ordering is written rather than hoped for:

```cpp
TEST(IoOperationTest, ACompletionBeforeAnyStopRunsTheAwaitingBody)
TEST(IoOperationTest, AStopBeforeTheCompletionAbandonsTheChainAndTheBodyNeverRuns)
TEST(IoOperationTest, ACompletionAfterAStopFindsTheBlockDetachedAndTouchesNoFrame)
TEST(IoOperationTest, AStopAfterTheCompletionFindsTheBlockDoneAndDoesNothing)
TEST(IoOperationTest, AnInlineCompletionInsideSubmitIsStillPostedNotRunInline)
TEST(IoOperationTest, ACancelWhileParkedOnASlotNeverReachesTheBackend)
TEST(IoOperationTest, TheBufferSurvivesTheChainsTeardownAndTheBackendsLateWrite)
TEST(IoOperationTest, TheDescriptorSurvivesTheChainsTeardown)
TEST(IoOperationTest, ThePermitIsReleasedOnAHomeThreadSoTheNextOperationSubmitsThere)
TEST(IoOperationTest, AThrowingHomePostLeavesTheChainLeakedRatherThanResumedElsewhere)
```

Two of these carry the whole design and must assert more than liveness:

`TheBufferSurvivesTheChainsTeardownAndTheBackendsLateWrite` — an
`io_buffer::allocate` buffer; submit; request stop so the chain is abandoned and the
frame destroyed; *then* have `manual_io_backend` write a known pattern into
`request.buffer.bytes()` and complete; then assert the pattern is readable through a
copy of the `io_buffer` the test still holds. The assertion is on the *bytes*, not on
the absence of a crash.

`ThePermitIsReleasedOnAHomeThreadSoTheNextOperationSubmitsThere` — `max_in_flight = 1`,
two operations, a `manual_scheduler` as the completion scheduler pumped by the test
thread. Record `std::this_thread::get_id()` inside `manual_io_backend::submit` for each.
Complete the first from a `std::jthread`, pump the scheduler on the test thread, and
assert the second submission's recorded thread id is the **test thread's**, not the
jthread's. Per `feedback_no_predictions_in_measurement`, the assertion is the test; do
not write the expected thread into a comment as a prediction.

`AThrowingHomePostLeavesTheChainLeakedRatherThanResumedElsewhere` — Review Focus 5. A
completion scheduler whose `post` throws `std::bad_alloc`. Assert the test survives, the
awaiting body never ran, and `alt_test::live_frames` shows the frame still alive (leaked
deliberately, not resumed on the wrong thread).

- [ ] **Step 3: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `alt/detail/io_operation.hpp` not found.

- [ ] **Step 4: Implement the block, the awaiter and `submit_operation`**

The protocol is fully specified in the design's "Operation state lives outside the
frame" and "Backpressure, and where the permit is released"; implement it as written.
Four points the signatures do not determine:

- `await_suspend` constructs the `std::stop_callback` into a
  `std::unique_ptr` member, **after** the awaiter has been moved, because a
  `stop_callback` is not movable and alt moves a foreign awaiter before it suspends.
  `cpwf`'s `CommitOnWorker::stop_` is the precedent.
- The posted completion does three things in this order: read under the mutex, release
  the slot, then `chain.resume_or_abandon(handle)` as the last statement. Nothing may be
  read through the block, the awaiter or the chain afterwards.
- `post_home` moves the block's own `shared_ptr` into `keep` before posting and the
  resumption takes it back out, so the block cannot die between the post and the run.
  A throwing `post` resets `keep` and leaves the chain leaked.
- The sentinel: the member exists in every configuration, and only the checks are
  `assert`-guarded. `dead_pattern` is stored **before** the block's teardown, not after,
  so a concurrent touch is caught. Follow
  `docs/superpowers/plans/2026-11-15-waiter-liveness-guard.md` Task 1 exactly, including
  its comment that the guard is absent under `NDEBUG`.

- [ ] **Step 5: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R IoOperationTest`
Expected: PASS, 10 tests.

- [ ] **Step 6: Measure that the two lifetime tests can actually fail**

Not optional, and not satisfied by reasoning. Per
`feedback_coverage_is_assertions_not_arrangements` and
`feedback_measure_before_claiming`: build the mutation and count.

Mutation A — remove the block's co-ownership of the `io_buffer` (hold a raw span
instead). Mutation B — remove the block's co-ownership of the `io_handle`.

For each, run `ctest --preset debug-asan --output-on-failure -R IoOperationTest` and
`ctest --preset debug --output-on-failure -R IoOperationTest --repeat until-fail:40`.

Record the counts in the commit message. A mutation that produces 40 passes is a finding
about this task's premise, not a reason to move on: it means the test does not reach the
hazard, and it must be surfaced rather than worked around.

- [ ] **Step 7: Add the second-callback teardown test**

CLAUDE.md records that libstdc++'s `request_stop` skips relocking the callback list
after the last callback, which kept a use-after-free in `detail::stop_forwarder`
invisible on every Linux preset including ASan, and that
`CancellationRefTest.ATeardownWithASecondCallbackOnTheChainsTokenIsSafe` is built with a
second registration on the same token on purpose — which made it fail every Linux preset
(ASan `heap-use-after-free`; 0 passes in 40 `debug` runs). This awaiter registers a
`std::stop_callback` on the chain's token, so it is in that blind spot.

Add `TEST(IoOperationTest, ATeardownWithASecondCallbackOnTheChainsTokenIsSafe)`,
modelled on the named test in `tests/test_cancellation_ref.cpp`: a second
`std::stop_callback` on the same token, registered so the walk has somewhere to go after
the abandoning one. Verify it fails with the keepalive removed, by the same
`--repeat until-fail:40` method as Step 6.

- [ ] **Step 8: Run the sanitizers**

Run:
```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R "IoOperationTest|IoBufferTest|IoHandleTest|IoBackendTest"
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R "IoOperationTest|IoBufferTest|IoHandleTest|IoBackendTest"
```
Expected: PASS, both, with no sanitizer diagnostics.

- [ ] **Step 9: Commit**

```bash
git add include/alt/detail/io_shared.hpp include/alt/detail/io_operation.hpp \
        src/io_operation.cpp tests/test_io_operation.cpp \
        tests/coroutine_test_support.hpp tests/CMakeLists.txt
git commit -m "Add the file I/O operation block and its cancellation-safe awaiter"
```

---

### Task 5: `file_io` lifecycle and shutdown

**Files:**
- Create: `include/alt/file_io.hpp`, `src/file_io.cpp`
- Create: `tests/test_file_io.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - `struct alt::file_io_options { io_backend_kind backend = io_backend_kind::automatic; std::size_t max_in_flight = 64; std::size_t queue_depth = 0; }`
  - `class alt::file_io`, immovable and uncopyable, with both `create` overloads from
    the spec, `~file_io()`, and `io_backend_kind backend() const noexcept`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(FileIoTest, CreateSucceedsAndReportsTheThreadPoolBackend)
TEST(FileIoTest, CreateWithAnExplicitBackendReportsThatBackendsKind)
TEST(FileIoTest, CreateRejectsAZeroMaxInFlight)
TEST(FileIoTest, CreateRejectsAnEmptyCompletionScheduler)
TEST(FileIoTest, TheDestructorReturnsOnceTheBackendIsQuietEvenWithResumptionsQueued)
TEST(FileIoTest, AQueuedResumptionRunSafelyAfterTheDestructorReturned)
```

`CreateRejectsAZeroMaxInFlight` is Review Focus 1: assert
`std::errc::invalid_argument`, because a zero bound would park the first operation on a
permit that can never exist.

`CreateRejectsAnEmptyCompletionScheduler` is Review Focus 4: pass a default-constructed
`scheduler_ref` and assert `std::errc::invalid_argument`, so the mistake is reported at
`create` rather than as an assert inside `scheduler_ref::post` on an I/O thread.

`AQueuedResumptionRunSafelyAfterTheDestructorReturned` is the shutdown design's whole
claim: a `manual_scheduler` as completion, an operation completed at the backend so its
resumption is queued but unpumped, `file_io` destroyed, and *then* the scheduler drained.
Assert the resumption ran and nothing tripped the sentinel — this is what the refcounted
`io_shared` exists for.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `alt/file_io.hpp` not found.

- [ ] **Step 3: Implement `file_io`**

Construction through a private `passkey` and `std::in_place` into the `expected`,
exactly as `thread_pool::create` does and for the same reason — the type is immovable
and the caller binds a reference whose address must be stable. In 7a the `automatic`
probe always selects `thread_pool`; the probe's structure goes in now so 7b adds a
branch rather than a mechanism.

`~file_io`: clear `accepting`, then wait on `io_shared::at_backend` reaching zero. It
does **not** wait for posted resumptions. Add the Doxygen for both preconditions the
spec states — `file_io` outlives its operations, and the destructor must not run on a
borrowed `thread_pool` worker.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R FileIoTest`
Expected: PASS, 6 tests.

- [ ] **Step 5: Add death tests for the two preconditions**

In `tests/test_file_io.cpp`, including `death_test_support.hpp` and following its
`asserts_are_compiled_in` and `thread_sanitizer_is_active` skip guards as the existing
death-test files do:

```cpp
TEST(FileIoDeathTest, DestroyingAFileIoWithAnOperationParkedOnASlotAborts)
```

- [ ] **Step 6: Commit**

```bash
git add include/alt/file_io.hpp src/file_io.cpp tests/test_file_io.cpp tests/CMakeLists.txt
git commit -m "Add alt::file_io, its create overloads and its shutdown contract"
```

---

### Task 6: The thread-pool backend

**Files:**
- Create: `src/io_backend_thread_pool.cpp`
- Modify: `include/alt/io_backend.hpp` (declare `make_thread_pool_backend`)
- Modify: `tests/test_io_backend.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: `std::unique_ptr<io_backend> alt::make_thread_pool_backend(thread_pool&)`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ThreadPoolBackendTest, AReadAtReturnsTheFilesBytes)
TEST(ThreadPoolBackendTest, AWriteAtPutsBytesAtTheOffset)
TEST(ThreadPoolBackendTest, AReadPastTheEndReturnsAShortCount)
TEST(ThreadPoolBackendTest, AReadOfAMissingPathReportsNoSuchFileOrDirectory)
TEST(ThreadPoolBackendTest, AJobCancelledBeforeItStartedNeverTouchesTheFile)
TEST(ThreadPoolBackendTest, AJobCancelledAfterItStartedStillCompletesAndReportsItsResult)
```

A temp-directory fixture: `SetUp` makes a unique directory under
`std::filesystem::temp_directory_path()`, `TearDown` removes it recursively.

`AJobCancelledBeforeItStartedNeverTouchesTheFile` needs the pool occupied so the job
stays queued: a one-thread `thread_pool` with a blocking job posted ahead of it, released
after the cancel.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `make_thread_pool_backend` not declared.

- [ ] **Step 3: Implement the backend**

One `src/io_backend_thread_pool.cpp` with the POSIX and Windows syscalls behind
`#ifdef _WIN32`. `submit` posts a job holding a `shared_ptr` to its own submission
record, so a cancelled job's record and buffer die on the worker. `pread`/`pwrite`
retry on `EINTR`, as `cpwf`'s `FileBody::pull` does. errno maps through
`std::system_category()`.

**Implement the whole `io_op` dispatch table except `io_op::invoke`** — `read`, `write`,
`sync_full`, `sync_data`, `stat_handle`, `stat_path`, `open`, `rename`, `unlink`,
`list_directory` — even though this task's tests cover only the read and write paths.
The rest are exercised end-to-end by Tasks 7 and 8, which is why those tasks touch no
backend file. `invoke` is left for Task 9, because the type it dispatches on does not
exist yet.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R ThreadPoolBackendTest`
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
git add src/io_backend_thread_pool.cpp include/alt/io_backend.hpp tests/test_io_backend.cpp
git commit -m "Add the thread-pool I/O backend"
```

---

### Task 7: `async_file` and its handle operations

**Files:**
- Create: `include/alt/async_file.hpp`, `src/async_file.cpp`
- Create: `tests/test_async_file.cpp`
- Modify: `include/alt/file_io.hpp`, `src/file_io.cpp` (this task adds `file_io::open`,
  which returns an `async_file` and so could not be declared before this header existed)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–6.
- Produces:
  - `enum class alt::file_kind : std::uint8_t { regular, directory, symlink, other, unknown }`
  - `enum class alt::sync_mode : std::uint8_t { full, data }`
  - `struct alt::file_identity { std::uint64_t device; std::uint64_t serial; }`
  - `struct alt::file_status { std::uint64_t size; std::int64_t mtime_ns; file_kind kind; file_identity identity; }`
  - `enum class alt::open_flag : std::uint32_t { read, write, read_write, create, exclusive, truncate, append, close_on_exec }`,
    used through `alt::flags<open_flag>` from `<alt/flags.hpp>` and therefore requiring
    an `alt::flag_enum` specialization for it.
  - `struct alt::file_mode { std::uint32_t permissions = 0644; }` — the creation mode for
    an `open` that carries `open_flag::create`, ignored otherwise and on Windows. A
    struct rather than a bare integer so a call site cannot pass it where the flags go.
  - `class alt::async_file` with `adopt`, `read_at`, `write_at`, `sync`, `stat`,
    `close`, `handle`, a closing destructor, and move operations.
  - `file_io::open(const std::filesystem::path&, flags<open_flag>, file_mode = {})`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(AsyncFileTest, ReadAtYieldsTheBytesAtTheOffset)
TEST(AsyncFileTest, WriteAtThenReadAtRoundTrips)
TEST(AsyncFileTest, AZeroLengthReadSucceedsWithZeroBytes)
TEST(AsyncFileTest, AZeroLengthWriteSucceedsWithZeroBytes)
TEST(AsyncFileTest, ASizeLargerThanTheBufferIsRejectedBeforeSubmission)
TEST(AsyncFileTest, ReadAtOnAClosedFileReportsBadFileDescriptor)
TEST(AsyncFileTest, StatReportsSizeKindAndAStableIdentity)
TEST(AsyncFileTest, SyncDataSucceedsOnEveryPlatform)
TEST(AsyncFileTest, TwoConcurrentPositionalReadsEachGetTheirOwnOffsetsBytes)
TEST(AsyncFileTest, TheDestructorWithAnOutstandingOperationClosesNothing)
TEST(AsyncFileTest, OpenOfAMissingPathReportsNoSuchFileOrDirectory)
```

Review Focus 2 and 3 are the third, fourth and fifth of these.
`ASizeLargerThanTheBufferIsRejectedBeforeSubmission` asserts
`std::errc::invalid_argument` **and** that `manual_io_backend::pending()` stayed 0 — a
clamp would also pass an outcome-only assertion, and the spec requires rejection.

`StatReportsSizeKindAndAStableIdentity`: stat the same file twice through different
descriptors and assert the two `file_identity` values compare equal, which is the
property `cpwf`'s ETag depends on.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `alt/async_file.hpp` not found.

- [ ] **Step 3: Implement `async_file` and `file_io::open`**

Each operation is a short `alt::task` coroutine: acquire a slot, then `co_await` the
Task 4 awaiter, exactly as the spec's `read_at` sketch shows. `m_file` and `m_owner` are
the two `shared_ptr` members; every `io_request` the file builds copies `m_file` into its
block.

`sync_mode::data` maps to `fdatasync` on Linux, `fsync` elsewhere on POSIX, and
`FlushFileBuffers` on Windows. Document the degradation on the enumerator, as the spec
requires.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R AsyncFileTest`
Expected: PASS, 11 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/async_file.hpp src/async_file.cpp tests/test_async_file.cpp tests/CMakeLists.txt
git commit -m "Add alt::async_file and its handle operations"
```

---

### Task 8: `file_io` path operations

**Files:**
- Modify: `include/alt/file_io.hpp`, `src/file_io.cpp`, `tests/test_file_io.cpp`

**Interfaces:**
- Consumes: Tasks 1–7.
- Produces: `struct alt::directory_entry { std::string name; file_kind kind; }`, and
  `file_io::stat`, `rename`, `unlink`, `list_directory` with the spec's signatures.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(FileIoPathTest, StatOfAPathReportsItsSizeAndKind)
TEST(FileIoPathTest, StatOfADirectoryReportsKindDirectory)
TEST(FileIoPathTest, RenameMovesAFileAndTheOldPathIsGone)
TEST(FileIoPathTest, RenameOverAnExistingFileReplacesIt)
TEST(FileIoPathTest, RenameAcrossDevicesReportsCrossDeviceLink)
TEST(FileIoPathTest, UnlinkRemovesAFile)
TEST(FileIoPathTest, UnlinkOfAMissingPathReportsNoSuchFileOrDirectory)
TEST(FileIoPathTest, ListDirectoryReturnsEveryEntryWithoutDotOrDotDot)
TEST(FileIoPathTest, ListDirectoryOfAnEmptyDirectoryReturnsNoEntries)
TEST(FileIoPathTest, ListDirectoryOfAFileReportsNotADirectory)
```

`RenameAcrossDevicesReportsCrossDeviceLink` needs a second filesystem to be meaningful;
`GTEST_SKIP()` with a stated reason when `/dev/shm` (POSIX) is unavailable, rather than
asserting something weaker.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `file_io::stat` not declared.

- [ ] **Step 3: Implement the four operations**

All four go to the pool arm on every platform. `rename` uses `::rename` on POSIX and
`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` on Windows, so replace-an-existing-file
behaves the same on both. `list_directory` omits `.` and `..`, and leaves
`file_kind::unknown` where the OS does not say — document that, rather than hiding a
stat per entry behind it.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R FileIoPathTest`
Expected: PASS, 10 tests (9 where the cross-device case skips).

- [ ] **Step 5: Commit**

```bash
git add include/alt/file_io.hpp src/file_io.cpp tests/test_file_io.cpp
git commit -m "Add file_io's stat, rename, unlink and list_directory"
```

---

### Task 9: The `run()` escape hatch

**Files:**
- Modify: `include/alt/file_io.hpp`, `src/file_io.cpp`, `tests/test_file_io.cpp`
- Modify: `include/alt/detail/io_operation.hpp` (the erased-callable block)
- Modify: `src/io_backend_thread_pool.cpp` (the `io_op::invoke` arm Task 6 left out)

**Interfaces:**
- Consumes: Tasks 1–8.
- Produces: `file_io::run` with the spec's signature, and
  `alt::detail::io_invocable` — the abstract `virtual void invoke() noexcept = 0` the
  callable is erased behind, so `io_operation` stays one type.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(FileIoRunTest, RunReturnsTheCallablesValue)
TEST(FileIoRunTest, RunOfAVoidCallableYieldsAnExpectedVoid)
TEST(FileIoRunTest, RunExecutesTheCallableOnAWorkerNotTheCallersThread)
TEST(FileIoRunTest, AnExceptionFromTheCallableIsRethrownFromTheCoAwait)
TEST(FileIoRunTest, ACallableCancelledBeforeItRanIsNeverInvoked)
TEST(FileIoRunTest, ACallablesCapturesAreDestroyedOnTheWorkerWhenCancelled)
TEST(FileIoRunTest, ACallableCancelledWhileRunningCompletesAndItsResultIsDiscarded)
TEST(FileIoRunTest, RunConsumesAnInFlightSlot)
```

`ACallablesCapturesAreDestroyedOnTheWorkerWhenCancelled` is the test that proves the
escape hatch does not reintroduce the bug the component exists to prevent, and the
behaviour `cpwf`'s `CommitOnWorker` relies on to delete its temporary file. Capture a
move-only sentinel that records the thread id in its destructor, cancel before the job
runs, and assert the recorded id is a **worker's**, not the test thread's.

`RunConsumesAnInFlightSlot`: `max_in_flight = 1`, one `run` in flight, assert a second
operation parks until the first completes.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug`
Expected: FAIL — `file_io::run` not declared.

- [ ] **Step 3: Implement `run`**

`run` moves `f` into a heap `io_invocable` held by the operation block, so neither `f`
nor its captures are ever in the coroutine frame. The result and any
`std::exception_ptr` live in the block; the `co_await` rethrows the exception if the
chain is still there, and both die with the block if it is not — the convention
`task_completion_source` already set. `std::invoke_result_t<F>` of `void` yields
`std::expected<void, std::error_code>`.

- [ ] **Step 4: Run to verify they pass**

Run: `ctest --preset debug --output-on-failure -R FileIoRunTest`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/file_io.hpp include/alt/detail/io_operation.hpp \
        src/file_io.cpp src/io_backend_thread_pool.cpp tests/test_file_io.cpp
git commit -m "Add file_io::run, the escape hatch, with its callable out of the frame"
```

---

### Task 10: Documentation, the full gate sweep, and the version bump

**Files:**
- Create: `docs/io_buffer.md`, `docs/io_backend.md`, `docs/file_io.md`,
  `docs/async_file.md`
- Modify: `README.md`, `CLAUDE.md`, `CMakeLists.txt` (version and any new cppcheck
  suppression), `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`

- [ ] **Step 1: Write the four component pages**

One per component, matching the shape of `docs/task_completion_source.md` and
`docs/cancellation_ref.md`. Each carries the contract its header's Doxygen states, and
`docs/file_io.md` additionally carries the three things a reader cannot infer: that
cancellation detaches rather than aborts, the two shutdown preconditions, and that slot
handoff is serialized through the home scheduler.

- [ ] **Step 2: Add the component list entries**

`README.md`'s Components section gains the four. No claim about io_uring or IOCP: 7a
ships neither, and `backend()` reports `thread_pool`.

- [ ] **Step 3: Run the complete gate sweep**

Every check in CLAUDE.md, because a sweep that skips one is not a green branch:

```bash
cmake --preset debug           && cmake --build --preset debug           && ctest --preset debug --output-on-failure
cmake --preset debug-asan      && cmake --build --preset debug-asan      && ctest --preset debug-asan --output-on-failure
cmake --preset debug-tsan      && cmake --build --preset debug-tsan      && ctest --preset debug-tsan --output-on-failure
cmake --preset release-tests   && cmake --build --preset release-tests   && ctest --preset release-tests --output-on-failure
cmake --preset debug-coverage  && cmake --build --preset debug-coverage  && cmake --build --preset debug-coverage --target coverage
cmake -B build/tidy -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCLANG_TIDY_PATH=/usr/bin/clang-tidy-19 -DCMAKE_BUILD_TYPE=Debug \
    -DALT_CLANG_TIDY=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
clang-format -i <every file this branch changed>
```

The coverage step runs the suite itself; do not add a `ctest` before it. Expect
`libgcov profiling error: ... overwriting an existing profile data` lines against a
reused build directory — harmless, and issue #32 carries the measurements.

`cppcheck` reaches `src/*.cpp` only, and this branch adds the library's first
substantial compiled subsystem, so new findings are expected. Each one is either fixed
or suppressed **header by header with a written argument** in the root `CMakeLists.txt`,
in the style the existing suppressions use. A blanket `*/alt/*.hpp` suppression is not
acceptable.

- [ ] **Step 4: Bump the version to 1.15.0**

A standalone commit, message `Bump version to 1.15.0`. All six files from CLAUDE.md's
table, and `tests/test_version.cpp` in **three** places — the `EXPECT_EQ`s, the
`static_assert`s (which spell the number bare, so a grep for the old string misses
them), and the version string. `README.md` and `CLAUDE.md` also carry the
`find_package` compatibility example, whose operands illustrate the rule rather than
this release; adjust them so the prose still agrees with the snippet above it.

- [ ] **Step 5: Verify the bump**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure -R Version`
Expected: PASS. A stale `static_assert` fails the **build**, not the test, which is why
this step builds first.

- [ ] **Step 6: Commit and open the pull request**

```bash
git add docs/ README.md CLAUDE.md CMakeLists.txt
git commit -m "Document the file I/O components and settle the static-analysis gates"
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.15.0"
gh pr create --base main --title "Awaitable file I/O, phase 7a: interface and thread-pool backend" --body "<summary, Closes #49 only when 7b and 7c are also done -- reference it instead>"
```

Issue #49 covers all three phases, so the PR **references** it rather than closing it.
File 7b and 7c as their own issues with `gh issue create` before the PR merges, so the
remaining phases have a durable home rather than living only in this plan — the plan
workspace is deleted when the branch finishes, and PR #19's deferred list went with it.

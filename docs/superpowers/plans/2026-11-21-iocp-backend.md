# Phase 7c, the IOCP backend: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Windows, perform `read_at`/`write_at` as overlapped I/O completed through an I/O completion port, with adopted files routed to the thread pool and reported per file.

**Architecture:** A new `iocp_backend` (`src/io_backend_iocp.cpp`) owns a completion port and one completion thread, and forwards every operation except read and write to an internal thread-pool backend whose `open` is customized to open overlapped and associate with the port. The file layer gains a second backend pointer for adopted files, and `async_file::backend()`. The seam is unchanged.

**Tech Stack:** C++23, MSVC 19.5x (the only compiler that builds this backend), Win32 (`CreateIoCompletionPort`, `GetQueuedCompletionStatusEx`, `CancelIoEx`, `SetFileCompletionNotificationModes`), GoogleTest, CMake presets.

**Spec:** `docs/superpowers/specs/2026-11-21-iocp-backend-design.md`, whose parent is `docs/superpowers/specs/2026-11-20-awaitable-file-io-design.md`. Read both.

## Global Constraints

- Branch `feature/iocp-backend`, on 1.16.0. The version bump is its own commit, `Bump version to 1.17.0`, touching the six files CLAUDE.md lists.
- The seam (`io_backend`, `io_request`, `io_completion`, `io_token`, `io_op`) is not changed.
- No `windows.h` in any public header. Windows-only code lives in `src/` behind `#ifdef _WIN32`.
- Every Windows test and every mutation check runs on all three MSVC presets with `-DALT_WARNINGS_AS_ERRORS=ON`: `debug`, `release-tests`, `debug-asan` (CLAUDE.md, "MSVC").
- Each mutation check is built and measured, then reverted; the source is restored with a fresh timestamp (`(Get-Item f).LastWriteTime = Get-Date`) so the build cannot reuse the mutant's object.
- `snake_case` throughout, `m_` members, Doxygen on every public symbol, `const` by default, `std::expected` for recoverable errors.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A zero-length transfer** (`size == 0` or an empty buffer) on IOCP must complete with 0 bytes and no error, never hang waiting for a packet. Test in Task 2.
2. **A requested size larger than the buffer, or than a `DWORD`,** must be clamped exactly as the pool arm does (`transfer_size`, then the `DWORD` limit). Test in Task 2 (`size = SIZE_MAX`, 4-byte buffer).
3. **An offset above 4 GiB** must reach `OffsetHigh`. Test in Task 2 on a sparse file.
4. **More completions than one `GetQueuedCompletionStatusEx` batch** must all be delivered. Test in Task 6 (200 concurrent reads).
5. **A consumer's own overlapped call on `handle()`, made with the low-bit tag as documented,** must leave the port untouched. Test in Task 2, beside the untagged foreign-packet test.

---

### Task 1: Share the Windows primitives, and let a pool backend's open be customized

**Files:**
- Create: `src/io_windows.hpp`, `src/io_windows.cpp` (both wholly `#ifdef _WIN32`)
- Create: `src/io_backend_internal.hpp`
- Modify: `src/io_backend_thread_pool.cpp` (the `#ifdef _WIN32` primitives at lines ~63-279 move out; `perform`'s `io_op::open` case uses the policy)
- Modify: `tests/CMakeLists.txt` (add `${PROJECT_SOURCE_DIR}/src` to `alt_tests`' PRIVATE include directories)
- Test: `tests/test_io_backend.cpp`

**Interfaces:**
- Produces, in `src/io_windows.hpp`, namespace `alt::detail::windows_io`: `std::error_code normalize_error(std::error_code)`, `std::error_code last_error()`, `HANDLE to_handle(native_handle)`, `bool is_directory_handle(native_handle)`, `std::error_code open_file(const io_open_args&, native_handle& out, DWORD extra_flags)`. The bodies are today's `normalize_windows_error`, `last_error`, `to_handle`, `is_directory_handle` and `do_open`, with `extra_flags` OR'd into `CreateFileW`'s flags.
- Produces, in `src/io_backend_internal.hpp`, namespace `alt::detail`:
  ```cpp
  struct pool_open_policy
  {
      std::uint32_t extra_flags = 0;   // OR'd into CreateFileW's flags; unused off Windows
      std::error_code (*prepare)(void* context, native_handle handle) noexcept = nullptr;
      void* context = nullptr;
  };
  std::unique_ptr<io_backend> make_thread_pool_backend(thread_pool& pool, const pool_open_policy& policy);
  ```
  `alt::make_thread_pool_backend(pool)` becomes `detail::make_thread_pool_backend(pool, {})`. After a successful open, a non-null `prepare` is called with the new handle; if it returns an error, the handle is closed, `out` is left `invalid_native_handle`, and the open reports that error.

- [ ] **Step 1: Write the failing tests** in `tests/test_io_backend.cpp`, Windows-only, fixture `ThreadPoolBackendTest` extended with a way to build its backend from a policy:
  - `AnOpenPolicysPrepareHookSeesTheNewHandleOnce`: policy with `prepare` recording its argument; open a file for read; expect one call, with the handle the completion reports.
  - `AnOpenWhosePrepareHookFailsReportsThatErrorAndLeavesNoHandle`: `prepare` returns `std::errc::io_error`; expect the completion's error is `io_error`, the out handle is `invalid_native_handle`, and `GetProcessHandleCount` is the same before and after the call (the handle was closed, not leaked).
  - `AnOpenPolicysExtraFlagsReachCreateFile`: the test serves a pipe (`CreateNamedPipeW`, `PIPE_ACCESS_OUTBOUND`) and opens `\\.\pipe\<name>` for read through a policy with `extra_flags = FILE_FLAG_OVERLAPPED`. With nothing written, a `ReadFile` on the returned handle with an `OVERLAPPED` and a fresh event returns `FALSE` with `ERROR_IO_PENDING` at once; a synchronous handle would block instead. Afterwards the test writes, waits for the event, and closes everything.
- [ ] **Step 2: Run them to verify they fail to compile** (`make_thread_pool_backend` has no policy overload).
  Run: `cmake --build --preset debug --parallel`
- [ ] **Step 3: Move the primitives** into `src/io_windows.hpp/.cpp`, add `pool_open_policy` and the factory overload, and route `perform`'s open through `windows_io::open_file(args, out, policy.extra_flags)` then `prepare`. The thread-pool backend stores the policy by value.
- [ ] **Step 4: Run the whole suite on MSVC `debug`.**
  Run: `ctest --preset debug -j 8 --output-on-failure`
  Expected: all pass, including the three new tests.
- [ ] **Step 5: Commit** — `Share the Windows I/O primitives and let a pool backend's open be customized`.

### Task 2: The IOCP backend: port, completion thread, transfers, shutdown

**Files:**
- Create: `src/io_backend_iocp.hpp` (declarations; includes `<windows.h>`; tests may include it, consumers never see it), `src/io_backend_iocp.cpp` (wholly `#ifdef _WIN32`)
- Create: `tests/test_io_backend_iocp.cpp` (wholly `#ifdef _WIN32`), registered in `tests/CMakeLists.txt`'s `alt_tests` sources
- Modify: `docs/io_backend.md` (the IOCP backend: what runs where, the completion thread, the slow-close note from the spec's "What runs on the completion thread")

**Interfaces:**
- Consumes: Task 1's `pool_open_policy`, `detail::make_thread_pool_backend(pool, policy)`, `windows_io::*`.
- Produces, in `src/io_backend_iocp.hpp`, namespace `alt::detail`:
  ```cpp
  std::expected<std::unique_ptr<io_backend>, std::error_code> make_iocp_backend(thread_pool& pool);
  HANDLE iocp_port_for_testing(io_backend& iocp);                   // precondition: built by make_iocp_backend
  std::size_t iocp_ignored_packets_for_testing(io_backend& iocp);   // packets the completion thread found no record for
  ```
  The ignored-packet count is an atomic the completion thread increments; it exists so the tests can see the defense act, since an ignored packet otherwise leaves no trace.
  The returned backend's `kind()` is `io_backend_kind::iocp`. Its tokens for IOCP transfers have bit 63 set (`iocp_token_tag = std::uint64_t{1} << 63`); tokens without it came from the internal pool backend and `cancel` forwards them there.

Behaviour, all from the spec's "Shape of the backend", "Open", "Transfers", "The handle a consumer sees" and "Shutdown": the policy's `extra_flags` is `FILE_FLAG_OVERLAPPED`; its `prepare` associates the handle with the port (`CreateIoCompletionPort(handle, port, 0, 0)`) and calls `SetFileCompletionNotificationModes(handle, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)`. `read`/`write` build a heap transfer record whose first member is the `OVERLAPPED`; the in-flight registry is indexed by token and by record address under one mutex; synchronous success and any immediate failure other than `ERROR_IO_PENDING` complete inline inside `submit`; the completion thread uses `GetQueuedCompletionStatusEx` and completes only records it finds by address, ignoring all others; `ERROR_HANDLE_EOF` (immediate or in the packet) is a zero-byte success; error normalization and the directory check are `windows_io`'s. "Complete" is always: remove from the registry, call `on_done`, free the record. The destructor posts a packet with the reserved key `shutdown_key = ~ULONG_PTR{0}`, joins, closes the port; a non-empty registry asserts in debug and calls `std::terminate()` in release. Cancellation is Task 3: in this task `cancel` forwards untagged tokens to the pool backend and ignores tagged ones.

- [ ] **Step 1: Write the failing tests** in `tests/test_io_backend_iocp.cpp`, fixture `IocpBackendTest` (a 1-thread pool, `make_iocp_backend`, a scratch directory, `open_file` through the backend as `ThreadPoolBackendTest` does):
  - `ReportsIocpAsItsKind`.
  - `AReadAtReturnsTheFilesBytes`, `AWriteAtPutsBytesAtTheOffset`, `AReadPastTheEndReturnsAShortCount` (mirror the pool tests' values: `"hello, backend"` read 5 at 7 → `"backe"`; `"0123456789"` etc.).
  - `AReadAtOrPastTheEndIsAZeroByteSuccess` at offsets 11 and 4096 of an 11-byte file.
  - `AZeroLengthTransferCompletesWithNoBytesAndNoError` (Review Focus 1): read and write with `size = 0`.
  - `ARequestedSizeIsClampedToTheBuffer` (Review Focus 2): read with `size = SIZE_MAX` and a 4-byte buffer on a 10-byte file → 4 bytes.
  - `AnOffsetAboveFourGibReachesTheFile` (Review Focus 3): mark the file sparse (`FSCTL_SET_SPARSE`), write `"far"` at `5 * 2^30`, read it back at the same offset.
  - `ASynchronousSuccessQueuesNoPacket`: 50 reads of a freshly written small file, each awaited; afterwards, with a short settle (`wait_until` the count is stable for 100 ms), `iocp_ignored_packets_for_testing` is 0 and every callback ran exactly once. A cached read is not guaranteed to complete synchronously, so also count how many callbacks ran before `submit` returned and `RecordProperty` it; the assertion is on the ignored count, which is what a missing `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` would raise.
  - `AReadOnAPipeIsPendingUntilDataArrives`: the test creates a pipe server (`CreateNamedPipeW`, `PIPE_ACCESS_OUTBOUND`), opens `\\.\pipe\<name>` for read **through the backend**, submits a 7-byte read, checks the callback has not run after 100 ms, writes `"pending"` from the server, and expects 7 bytes, no error, the data in the buffer.
  - `AnImmediateFailureCompletesInline`: a write on a handle opened read-only → error, callback ran before `submit` returned.
  - `ATransferOnADirectoryReportsIsADirectory`, `ATransferOnAClosedHandleFails` (mirror the pool tests).
  - `NonTransferOperationsRunOnThePool`: stat of a handle opened by this backend, both sync modes, rename, unlink and list_directory all succeed.
  - `AForeignPacketIsIgnored`: on a handle this backend opened, the test issues its own `ReadFile` with an `OVERLAPPED` whose `hEvent` is an untagged event, waits on that event, then runs one of the backend's own reads to completion; expect `iocp_ignored_packets_for_testing` rose by exactly 1 and the backend's read completed with the right data (under `debug-asan`, no report).
  - `ATaggedOverlappedCallQueuesNothing` (Review Focus 5): same, with the low bit set on `hEvent`; afterwards `GetQueuedCompletionStatus(iocp_port_for_testing(backend), ..., 0)` returns `FALSE` with a null `OVERLAPPED*` — this is the documented path for a consumer using `handle()`.
  - `APoolTransferOnAPortAssociatedHandleQueuesNothing`: a pool-backend read (`make_thread_pool_backend(pool)`) on a handle this backend opened; then the port holds nothing.
  - `DestroyingTheBackendWithNothingInFlightReturns`; death test `DestroyingTheBackendWithATransferInFlightAborts` (pending pipe read, then destroy) using `death_test_support.hpp`, skipped in release builds as the existing death tests are.
- [ ] **Step 2: Run them to verify they fail to compile** (no `make_iocp_backend`).
- [ ] **Step 3: Implement `src/io_backend_iocp.cpp`** to the behaviour above.
- [ ] **Step 4: Run `ctest --preset debug -R IocpBackend --output-on-failure --repeat until-fail:20`**, then all three MSVC presets in full.
  Expected: all pass; the death test passes in `debug` and `debug-asan` and is skipped in `release-tests`.
- [ ] **Step 5: Mutation checks**, each measured then reverted:
  - complete a packet by casting `OVERLAPPED*` without the registry lookup → `AForeignPacketIsIgnored` fails under `debug-asan`;
  - remove `SetFileCompletionNotificationModes` but keep inline completion → `ASynchronousSuccessQueuesNoPacket` sees ignored packets (if no read in that run completed synchronously, the mutant is undetectable on this machine: say so in the commit message rather than claim it);
  - free the record before calling `on_done` → an ASan report in the pending-pipe test.
- [ ] **Step 6: Update `docs/io_backend.md`**, then commit — `Add the IOCP backend for overlapped reads and writes`.

### Task 3: Cancellation that cannot reach the wrong operation

**Files:**
- Modify: `src/io_backend_iocp.cpp`
- Test: `tests/test_io_backend_iocp.cpp`

**Interfaces:**
- Consumes: Task 2's registry and token tag.
- Produces: `cancel(token)` for a tagged token takes the registry mutex and, only if the token is still registered, calls `CancelIoEx(handle, &record->overlapped)` while holding it. A packet with `ERROR_OPERATION_ABORTED` is reported as `std::errc::operation_canceled`.

- [ ] **Step 1: Write the failing tests:**
  - `CancellingAPendingReadCompletesItOnceAsCancelled`: pending pipe read (Task 2's setup), `cancel`, expect one callback with `operation_canceled` and 0 bytes.
  - `CancellingAfterCompletionIsANoOp`: complete a read, then `cancel` its token; nothing happens, nothing crashes (`debug-asan`).
  - `ACancelForAFinishedTokenDoesNotCancelANewTransferOnTheSameHandle`: read A on a pipe completes; read B is submitted on the same handle and is pending; `cancel(A's token)`; write data; expect B completes with its data, not `operation_canceled`. Repeat 50 times in the test body, so a reused record address has the chance to collide.
  - `CancelRacingCompletionDeliversExactlyOnce`: 200 iterations of a pending pipe read with `cancel` on one thread and the server write on another, started together; each read's callback runs exactly once, with either the data or `operation_canceled`.
  - `CancellingAPoolTokenReachesThePool`: occupy the pool's only thread (the pool tests' `blocker`), submit a stat, cancel its (untagged) token, release; expect `operation_canceled`.
- [ ] **Step 2: Run them to verify the cancel tests fail** (Task 2's `cancel` ignores tagged tokens, so the pending read never completes; give the test a 5-second guard that fails rather than hangs).
- [ ] **Step 3: Implement** the cancel path.
- [ ] **Step 4: Run all three MSVC presets in full**, and `-R Cancel --repeat until-fail:50` under `debug-asan`.
- [ ] **Step 5: Mutation check**: move `CancelIoEx` outside the lock (look up under the lock, release, then cancel). Measure whether `ACancelForAFinishedTokenDoesNotCancelANewTransferOnTheSameHandle` fails. If it cannot be made to fail deterministically, say so in a comment on that test and in the commit message rather than claim it guards the lock.
- [ ] **Step 6: Commit** — `Cancel IOCP transfers under the registry lock`.

### Task 4: Append on an overlapped handle

**Files:**
- Modify: `src/io_backend_iocp.cpp` (only if the measurement requires it)
- Test: `tests/test_io_backend_iocp.cpp`

**Interfaces:**
- Consumes: Task 2's transfers.
- Produces: an `open_flag::append` write on an IOCP-opened handle always lands at the end of the file.

- [ ] **Step 1: Write the test** `AnAppendWriteLandsAtTheEnd`: open `"start"` with `write | append`, write `"-one"` at offset 0 and `"-two"` at offset 0, read the whole file; expect `"start-one-two"`.
- [ ] **Step 2: Run it.** If it passes as Task 2 left it, record in the commit message that an ordinary offset appends on a `FILE_APPEND_DATA`-only overlapped handle on this machine. If it fails, go to Step 3.
- [ ] **Step 3 (only if Step 2 failed): stop and report to the maintainer.** The documented fix (`Offset = OffsetHigh = 0xFFFFFFFF` for writes on an append handle) needs the backend to know at transfer time that a handle was opened for append, and the seam carries no such fact. Every way of supplying it (a per-handle record, which a backend can never retire; a request field, which widens the seam) is a design decision, not an implementation one. Keep the failing test, marked `GTEST_SKIP` with the measured result, and leave the decision open.
- [ ] **Step 4: Run all three MSVC presets in full.**
- [ ] **Step 5: Commit** — `Pin that an append write on an IOCP handle lands at the end`.

### Task 5: Select IOCP, route adopted files to the pool, report the backend per file

**Files:**
- Modify: `include/alt/detail/io_handle.hpp`, `src/io_handle.cpp` (origin)
- Modify: `include/alt/detail/io_shared.hpp` (adopted-file backend, the two kinds)
- Modify: `include/alt/detail/io_operation.hpp`, `src/io_operation.cpp` (the block keeps the backend it submitted to)
- Modify: `include/alt/file_io.hpp`, `src/file_io.cpp` (probe, second owned backend)
- Modify: `include/alt/async_file.hpp`, `src/async_file.cpp` (`backend()`, `handle()` and `adopt()` docs)
- Modify: `docs/async_file.md`, `docs/file_io.md`, the parent design's staging row for 7c
- Test: `tests/test_file_io.cpp`, `tests/test_async_file.cpp`, `tests/test_io_handle.cpp`

**Interfaces:**
- Consumes: Task 2's `make_iocp_backend`, Task 1's `detail::make_thread_pool_backend`.
- Produces:
  - `enum class handle_origin : std::uint8_t { opened, adopted };` in `io_handle.hpp`; `io_handle::adopt(native_handle handle, handle_origin origin = handle_origin::adopted)`; `[[nodiscard]] handle_origin io_handle::origin() const noexcept`. `open_file` in `src/async_file.cpp` passes `handle_origin::opened`; every other caller keeps the default, which is the safe one.
  - `io_shared(io_backend& seam, io_backend& adopted_seam, scheduler_ref home, std::ptrdiff_t max_in_flight)`; members `io_backend* const adopted_backend`, `const io_backend_kind primary_kind`, `const io_backend_kind adopted_kind` (read from the backends at construction, so `async_file::backend()` never dereferences a backend that may be gone); `[[nodiscard]] io_backend& backend_for(const io_handle* file) const noexcept` returns `*adopted_backend` when `file` is non-null and `file->origin() == handle_origin::adopted`, else `*backend`. For every service but IOCP, `adopted_seam` is the same object as `seam`.
  - The operation block gains `io_backend* submitted_to`, set from `backend_for(file.get())` before `submit`; both cancel sites (`src/io_operation.cpp` ~79 and ~422) use it instead of `owner->backend`.
  - `file_io` gains `std::unique_ptr<io_backend> m_owned_adopted_backend`, built (`make_thread_pool_backend(workers)`) only when the probe chose IOCP.
  - Probe: on Windows, `automatic` tries `make_iocp_backend` and falls back to the thread pool on error; `iocp` builds it or fails `create` with its error; off Windows `iocp` stays `not_supported`.
  - `/** ... */ [[nodiscard]] io_backend_kind async_file::backend() const noexcept;` — `m_owner ? (m_file && m_file->origin() == handle_origin::adopted ? m_owner->adopted_kind : m_owner->primary_kind) : io_backend_kind::thread_pool`.

- [ ] **Step 1: Write the failing tests:**
  - `test_io_handle.cpp`: `AdoptDefaultsToTheAdoptedOrigin`, `AnOpenedOriginIsKept`.
  - `test_file_io.cpp`, Windows: `AutomaticSelectsIocpOnWindows`, `AnExplicitIocpRequestSucceedsOnWindows`; elsewhere `AnExplicitIocpRequestIsNotSupported` (already exists for io_uring; mirror it). Update the existing `backend() == thread_pool` expectation for `automatic` to be `iocp` on Windows.
  - `test_async_file.cpp`, Windows, on an IOCP `file_io`:
    - `AnOpenedFileReportsIocp`;
    - `AnAdoptedSynchronousHandleReportsThreadPoolAndWorks` (handle from `CreateFileW` without the overlapped flag; write then read back);
    - `AnAdoptedOverlappedHandleReportsThreadPoolAndWorks` (with `FILE_FLAG_OVERLAPPED`; same);
    - `CancellingAnAdoptedFilesQueuedReadReachesThePool` (occupy the pool's thread, start a read, cancel the chain, release; the read reports cancellation and the file is untouched);
    - `AMovedFromFileReportsThreadPool`.
  - On a thread-pool `file_io`, any platform: `AnAdoptedFileReportsTheServicesBackend`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement**, in the order handle origin → `io_shared` → block → `file_io` → `async_file`.
- [ ] **Step 4: Mutation check**: make `backend_for` always return the primary → `AnAdoptedOverlappedHandleReportsThreadPoolAndWorks` must fail: an overlapped transfer on a handle not associated with the port goes pending and its packet never comes, which the fixture's 10-second guard reports. (The synchronous-handle test is expected to pass under this mutant, since a transfer on a synchronous handle completes inline; it guards the report, not the routing.)
- [ ] **Step 5: Docs**: `async_file::backend()` Doxygen; `handle()` and `adopt()` Doxygen and `docs/async_file.md` per the spec's "The handle a consumer sees" (overlapped and port-associated on an IOCP service; synchronous calls invalid; use overlapped calls with the low-bit tag); `docs/file_io.md` selection rules and the IOCP backend; the parent design's staging row for 7c notes `async_file::backend()` as the one public addition.
- [ ] **Step 6: Run all three MSVC presets in full**, then commit — `Select IOCP on Windows and route adopted files to the thread pool`.

### Task 6: Run the file-layer suite against IOCP

**Files:**
- Create: `tests/file_io_test_support.hpp`
- Modify: `tests/test_file_io.cpp`, `tests/test_async_file.cpp` (fixtures that build a real `file_io`: `FileIoPathTest`, `FileIoRunTest`, `run_rig`, `AsyncFileTest::m_real`)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `inline alt::file_io_options file_io_options_under_test(alt::file_io_options base = {})` in `alt_test` namespace: returns `base` with `.backend` replaced by `iocp` when the environment variable `ALT_TEST_IO_BACKEND` is `iocp`, by `thread_pool` when it is `thread_pool`, unchanged when unset; any other value is a test-setup failure (`ADD_FAILURE` plus `base`).
- CMake, on `WIN32` only: `add_test(NAME file_layer_on_iocp COMMAND alt_tests "--gtest_filter=FileIo*:AsyncFile*")` with `ENVIRONMENT "ALT_TEST_IO_BACKEND=iocp"` and `TIMEOUT 300`, and the discovered tests get `ENVIRONMENT "ALT_TEST_IO_BACKEND=thread_pool"` so the two runs are explicit. A plain `add_test`, not a second `gtest_discover_tests`, so nothing new depends on discovery (issue #68).

- [ ] **Step 1: Add the helper and switch the four fixtures to it.**
- [ ] **Step 2: Add the Review Focus 4 test** `ManyConcurrentReadsAllComplete` to `test_async_file.cpp`: on the fixture's `file_io` with `max_in_flight = 64`, start 200 `read_at`s of distinct 1-byte offsets of a 200-byte file and await them all; every one returns its byte.
- [ ] **Step 3: Run `ctest --preset debug -R file_layer_on_iocp --output-on-failure`.** Fix what fails in the backend, not in the tests, unless a test asserts thread-pool-only behaviour; such a test is changed to state which backend it requires, with the reason.
- [ ] **Step 4: Run all three MSVC presets in full.**
- [ ] **Step 5: Commit** — `Run the file-layer suite against the IOCP backend`.

### Task 7: Version, verification, pull request

**Files:** the six CLAUDE.md lists for a version bump.

- [ ] **Step 1: Bump to 1.17.0** in all six files, including the three places in `tests/test_version.cpp` and the `find_package` compatibility prose in `README.md` and `CLAUDE.md` (requesting `1.17.0` accepts later `1.17.x`, rejects `1.18.0` and `1.16.x`, and `1.17.1` against `1.17.0` fails). Commit alone: `Bump version to 1.17.0`.
- [ ] **Step 2: Run all three MSVC presets in full** with `-DALT_WARNINGS_AS_ERRORS=ON`. Expected: everything passes, `file_layer_on_iocp` included.
- [ ] **Step 3: Push and open the pull request** against `main` with `Closes #53.` and the mutation-check results, and wait for the eleven CI jobs. The Linux jobs never compile the backend, so a Linux failure means a portable change in Task 5 or 6.

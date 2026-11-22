# io_uring Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Perform `read`, `write`, `sync_full` and `sync_data` through an io_uring ring on Linux, behind the existing `io_backend` seam, falling back to the thread pool where a ring cannot be created or the kernel lacks the opcodes.

**Architecture:** A backend owning a ring and one reaper thread, composing an internal thread-pool backend for the seven operations it does not handle, with tagged tokens routing `cancel`. Structurally the mirror of the IOCP backend from phase 7c, minus its handle-preparation machinery, which io_uring does not need.

**Tech Stack:** C++23, liburing (floor 2.1, package `liburing2`, Linux only), GoogleTest, CMake presets.

**Spec:** `docs/superpowers/specs/2026-11-21-io-uring-backend-design.md`

## Global Constraints

- **liburing is a hard dependency on Linux only.** Floor **2.1** (Ubuntu 22.04 / Debian 12, package `liburing2`). `io_uring_prep_cancel64` is liburing **2.2** and MUST NOT be used. `io_uring_queue_init_params`, `IORING_SETUP_CQSIZE` and `io_uring_get_probe_ring` are all present at 2.1.
- **No gate exercises the floor.** This machine has liburing 2.14 and CI's runner 2.5. A post-2.1 helper compiles and passes everywhere here and breaks only for a consumer on 22.04.
- **No public header may name `liburing.h`.** `src/io_backend_io_uring.cpp` is the only translation unit that includes it. `src/io_backend_io_uring.hpp` is library-private and is not installed.
- **Linux-only code is guarded by `#ifdef __linux__` wrapping the whole file's contents**, exactly as `src/io_backend_iocp.hpp` guards on `_WIN32`. The three MSVC CI jobs must be unaffected by every task.
- `snake_case` throughout; template typenames `PascalCase`; `m_` on private members; `const` by default; Doxygen on all public functions, classes, structs and enums; American spelling, with the double-l `cancelled`/`cancellation`/`cancellable` forms approved.
- **Completions must never run user code on the reaper thread**, and no callback may be invoked while holding any of the backend's locks.
- **`std::expected` for recoverable errors.** The backend seam's callback is a plain `noexcept` function pointer that must not allocate or throw.
- **Eleven CI jobs, eight on Linux**, all must pass: `Build & Test (GCC)`, `(Clang)`, `(optimized)`, `AddressSanitizer + UBSan`, `ThreadSanitizer`, `clang-tidy`, `cppcheck`, `Coverage`, plus `Build & Test (MSVC, debug)`, `(MSVC, release-tests)`, `(MSVC, debug-asan)`.
- **`clang-tidy` is pinned to `clang-tidy-19` with `clang-19`/`clang++-19`.** `CLAUDE.md`'s "Quality checks" has the exact invocation. `tests/.clang-tidy` is appended to, never rewritten.
- **Every test carrying a guarantee gets a mutation of record**: break what it guards, confirm it fails, revert, record the count. A guard test with no mutation on file is a finding, not coverage.
- **No line numbers in durable files** (code comments, docs, commit messages) — they go stale by construction.
- Suite baseline on this branch: **965 tests, all passing**, on `debug`.

## Review Focus

Input classes the spec implies that no task's tests would otherwise exercise. Each is assigned to the task that owns the code.

1. **A transfer whose `size` exceeds `buffer.size()`** — must be clamped, not passed to the kernel. The pool arm clamps via `transfer_size`; the ring arm must use the same helper. → Task 4.
2. **Destruction with a *forwarded* operation still in flight** — the ring half of the precondition is obvious, the pool half is the subtle one, and it is exactly what the IOCP backend had to handle. → Task 3.
3. **A zero-size read or write submitted directly to the backend** — the file layer short-circuits `size == 0` before submitting, so only a direct backend caller reaches this. → Task 4.
4. **An operation on a closed or invalid descriptor** — `EBADF` must arrive as `std::errc::bad_file_descriptor` through the shared mapping, identically to the pool arm. → Task 4.
5. **`queue_depth` of 1 with `max_in_flight` of 64** — the completion ring must still be sized to cover `max_in_flight` plus reserved internal completions, or completions are lost at depth 1. → Task 3.

---

### Task 1: Wire liburing into the build and prove a consumer still links

**Files:**
- Modify: `CMakeLists.txt` (dependency discovery and linking)
- Modify: `cmake/AltConfig.cmake.in` (consumer-side rediscovery)
- Modify: `packaging/` control template (`Depends:`)
- Modify: `.github/workflows/ci.yml` (all eight Linux jobs)
- Modify: `CLAUDE.md`, `README.md` (build prerequisite)

**Interfaces:**
- Consumes: nothing.
- Produces: a linkable `alt` target on Linux with liburing available to `src/*.cpp`; the imported target name that `AltTargets.cmake` will reference in `$<LINK_ONLY:...>`.

- [ ] **Step 1: Discover and link liburing on Linux in `CMakeLists.txt`**

Use `find_package(PkgConfig REQUIRED)` and `pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.1)`, guarded to Linux only, and link the resulting imported target `PRIVATE` to `alt`. Configure must fail with a message naming `liburing-dev` when it is absent, because the dependency is hard by decision.

- [ ] **Step 2: Verify configure succeeds and reports the version found**

Run: `cmake --preset debug`
Expected: configures without error. Confirm the detected liburing version is at least 2.1.

- [ ] **Step 3: Rediscover liburing in `cmake/AltConfig.cmake.in`**

`alt` is declared by `add_library(alt ${ALT_SOURCES})` with no explicit type, so it is **static** unless `BUILD_SHARED_LIBS` is set — and a static library propagates even `PRIVATE` link dependencies to consumers as `$<LINK_ONLY:...>`. The package config must therefore define a target with **exactly the imported name the export references**, beside the existing `find_dependency(Threads)`, guarded to Linux. A bare `find_dependency(PkgConfig)` does not do this.

- [ ] **Step 4: Prove a downstream `find_package(Alt)` still configures and links**

`tests/package/consumer` already exists as a `find_package(Alt)` harness driven by `tests/package/run_version_gate_test.cmake`. Install to a staging prefix and run that harness.
Expected: the consumer configures and links. A failure here is the whole point of Steps 3 and 4 — it is invisible to an in-tree build.

- [ ] **Step 5: Add `liburing-dev` to every Linux CI job**

All eight: `Build & Test (GCC)`, `(Clang)`, `(optimized)`, `AddressSanitizer + UBSan`, `ThreadSanitizer`, `clang-tidy`, `cppcheck`, `Coverage`. The last three configure the project and will fail without it. Do not touch the three MSVC jobs.

- [ ] **Step 6: Record the prerequisite in `CLAUDE.md` and `README.md`**

Beside the existing compiler requirements: liburing (>= 2.1, `liburing-dev`) is required for Linux builds. Note it is Linux-only.

- [ ] **Step 7: Full suite still green**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: `965 tests, 0 failed`.

- [ ] **Step 8: Commit**

---

### Task 2: One definition of the transfer clamp and the errno mapping

**Files:**
- Modify: `src/io_backend_internal.hpp` (add the two shared helpers)
- Modify: `src/io_backend_thread_pool.cpp` (use them instead of its own)

**Interfaces:**
- Consumes: Task 1's build.
- Produces: in `namespace alt::detail` —
  - `std::size_t transfer_size(const io_request& request) noexcept` — returns `std::min(request.size, request.buffer.size())`.
  - an errno-to-`std::error_code` mapping function, keeping the thread-pool backend's current name and exact behavior.

This is a pure refactor. It exists so that Task 4's ring arm shares one definition with the pool arm rather than agreeing with it by coincidence — the documented contract names `std::errc` conditions and exactly one place must decide them.

- [ ] **Step 1: Move both helpers to `src/io_backend_internal.hpp`**

Move `transfer_size` out of `io_backend_thread_pool.cpp`'s anonymous namespace, and the errno mapping with it, into `alt::detail`. Behavior must be byte-for-byte identical; this task changes no observable behavior.

- [ ] **Step 2: Point the thread-pool backend at them and confirm nothing moved**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: `965 tests, 0 failed` — the same count as before, since nothing was added.

- [ ] **Step 3: Confirm the Windows arm still compiles**

The mapping has a Windows counterpart in the same file. Do not merge the two; this task touches only the POSIX errno path and the shared clamp. Confirm by inspection that no `_WIN32` branch changed, and say so in the report — the MSVC jobs are the only verification and they run in CI.

- [ ] **Step 4: Commit**

---

### Task 3: The backend's skeleton — ring, capability check, reaper, destruction

**Files:**
- Create: `src/io_backend_io_uring.hpp`
- Create: `src/io_backend_io_uring.cpp`
- Create: `tests/test_io_backend_io_uring.cpp`
- Modify: `tests/CMakeLists.txt` (add the test to the `alt_tests` sources list)

**Interfaces:**
- Consumes: Task 2's `alt::detail::transfer_size` and errno mapping.
- Produces:
  - `inline constexpr std::uint64_t alt::detail::io_uring_token_tag = std::uint64_t{1} << 63U;`
  - `std::expected<std::unique_ptr<io_backend>, std::error_code> alt::detail::make_io_uring_backend(thread_pool& pool, std::size_t queue_depth, std::size_t max_in_flight);`
  - test accessors, following the IOCP backend's `*_for_testing` convention:
    - `std::size_t io_uring_submission_depth_for_testing(const io_backend&) noexcept`
    - `std::size_t io_uring_completion_depth_for_testing(const io_backend&) noexcept`
    - `std::size_t io_uring_ignored_completions_for_testing(const io_backend&) noexcept`

At the end of this task the backend creates a ring and forwards **all eleven** operations to its internal pool backend. Ring operations arrive in Task 4. This is deliberately a working, reviewable deliverable on its own.

Build the internal pool backend with the **plain** `make_thread_pool_backend(pool)` — **no `pool_open_policy`.** io_uring needs no specially-opened descriptor, unlike IOCP, so there is no `prepare` hook and no extra open flags. Say this in the backend's own documentation, because a reader arriving from the IOCP backend will look for that machinery.

- [ ] **Step 1: Write the failing tests**

In `tests/test_io_backend_io_uring.cpp`, guarded by `#ifdef __linux__`:

```cpp
TEST(IoUringBackendTest, ReportsItsOwnKind)                                  // kind() == io_backend_kind::io_uring
TEST(IoUringBackendTest, DerivesBothRingSizesFromMaxInFlightWhenDepthIsZero) // submission >= max_in_flight, completion >= max_in_flight + reserved
TEST(IoUringBackendTest, SizesTheCompletionRingForMaxInFlightEvenAtDepthOne) // queue_depth 1, max_in_flight 64: completion_depth >= 64
TEST(IoUringBackendTest, HonoursAnExplicitQueueDepth)                        // submission_depth reflects the requested depth, rounded up to a power of two
TEST(IoUringBackendTest, ForwardsAnOperationItDoesNotHandleToThePool)        // a stat_path completes, and its token carries no io_uring_token_tag
```

The third test is Review Focus item 5.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --preset debug -R IoUringBackendTest --output-on-failure`
Expected: FAIL — `make_io_uring_backend` does not exist.

- [ ] **Step 3: Implement the factory and the backend class**

Create the ring with `io_uring_queue_init_params` and `IORING_SETUP_CQSIZE`. The submission ring is `queue_depth` when non-zero, else derived from `max_in_flight`; round up to a power of two and clamp, so the accessor reports the value actually in effect. The completion ring is sized to at least `max_in_flight` plus the reserved internal completions — one wake-up NOP plus one cancel per in-flight operation — so completion-queue overflow is unreachable by construction at any `queue_depth`.

Then run the capability check (Step 5) before returning. `kind()` returns `io_backend_kind::io_uring`. `submit` forwards every operation to the internal pool backend through a trampoline that counts it while in flight, mirroring the IOCP backend's forwarding; the pool's token is returned unchanged.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --preset debug -R IoUringBackendTest --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Add the capability check and its test**

Rings have existed since kernel 5.1, but `IORING_OP_READ` and `IORING_OP_WRITE` arrived in **5.6** and `IORING_OP_ASYNC_CANCEL` in **5.5**. On a 5.1–5.5 kernel `io_uring_queue_init` *succeeds* and every transfer then fails `EINVAL`, so ring creation is not evidence the backend can work.

After creating the ring, `io_uring_get_probe_ring` must report `READ`, `WRITE`, `FSYNC` and `ASYNC_CANCEL` all supported; if any is missing the factory fails with `std::errc::not_supported`. One all-or-nothing check, not a per-operation matrix.

```cpp
TEST(IoUringBackendTest, TheRunnerSupportsEveryOpcodeThisBackendNeeds)  // READ, WRITE, FSYNC, ASYNC_CANCEL all probe as supported
```

If the `not_supported` path cannot be reached without adding a production hook, leave it untested and say so in the report rather than writing a test that cannot fail.

- [ ] **Step 6: Add the destruction precondition death tests**

Destroying the backend with **any** operation in flight — a ring operation or a forwarded one whose callback has not run — is a precondition violation: it asserts, and under `NDEBUG` terminates, rather than free a record the kernel may still own. Reuse the IOCP backend's wording. Follow `docs/superpowers/specs/2026-11-17-precondition-death-tests-design.md`.

```cpp
TEST(IoUringBackendDeathTest, DestroyingItWithAForwardedOperationInFlightTerminates)
```

That is Review Focus item 2, and the subtler half of the precondition.

Shutdown order: verify quiescence, wake the reaper with the reserved NOP, join it, then `io_uring_queue_exit`.

- [ ] **Step 7: Mutation-check every test added**

For each, break what it guards, confirm failure, revert, record. Report counts.

- [ ] **Step 8: Gates and commit**

Run the full suite plus `clang-tidy-19` and `clang-format`. Expected: `965 + <added> tests, 0 failed`.

---

### Task 4: Reads and writes through the ring

**Files:**
- Modify: `src/io_backend_io_uring.cpp`
- Modify: `tests/test_io_backend_io_uring.cpp`

**Interfaces:**
- Consumes: Task 3's backend and accessors; Task 2's `transfer_size` and errno mapping.
- Produces: `io_op::read` and `io_op::write` served by the ring. Tokens for ring operations carry `io_uring_token_tag`; forwarded ones do not.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoUringBackendTest, ReadsAtAnOffset)                                 // bytes and contents match
TEST(IoUringBackendTest, WritesAtAnOffset)                               // the file holds the bytes afterward
TEST(IoUringBackendTest, AnAppendWriteLandsAtTheEndWhateverOffsetIsAsked) // O_APPEND descriptor; phase 7c pinned the same property for IOCP
TEST(IoUringBackendTest, AReadAtEndOfFileReportsZeroBytesAndNoError)
TEST(IoUringBackendTest, AReadPastEndOfFileReportsZeroBytesAndNoError)
TEST(IoUringBackendTest, AShortReadReportsTheShortCountAndNoError)
TEST(IoUringBackendTest, ARequestLargerThanItsBufferIsClampedToTheBuffer) // Review Focus 1
TEST(IoUringBackendTest, AZeroSizeTransferCompletesWithZeroBytes)         // Review Focus 3
TEST(IoUringBackendTest, AnOperationOnAClosedDescriptorReportsBadFileDescriptor) // Review Focus 4
TEST(IoUringBackendTest, ARingOperationsTokenCarriesTheTag)
TEST(IoUringBackendTest, ManyConcurrentTransfersAllComplete)             // enough to exceed the submission ring
```

`ARequestLargerThanItsBufferIsClampedToTheBuffer` must assert the clamp is applied, not merely that the call succeeds: a request with `size` greater than `buffer.size()` must report at most `buffer.size()` bytes.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --preset debug -R IoUringBackendTest --output-on-failure`
Expected: FAIL — reads and writes are still forwarded to the pool, so the tag assertion and the clamp fail.

- [ ] **Step 3: Implement records, the registry, and the submission path**

Each ring operation gets a record holding the `io_buffer` copy that keeps the bytes alive, the completion function pointer and its state, and the operation kind. Records live in a registry keyed by token.

**The SQE's `user_data` carries the token value, not a record pointer**, so a stale completion is looked up and found absent instead of dereferencing a freed record. Reserve two `user_data` values that belong to no operation: one for the wake-up NOP, one for cancel SQEs.

Serialize `io_uring_get_sqe` and `io_uring_submit` under a mutex — liburing is not safe to call concurrently on a shared ring, and `submit` may be called from any thread. A failure from either is reported as its errno and never waited on, because `submit` must not block, and both are **documented as untested rather than given a test that cannot fail**. **The two failures are not interchangeable**: a null `io_uring_get_sqe` published nothing and its record is freed, while a refused `io_uring_submit` has already published its entry and its record must be quarantined rather than freed. This sentence originally said only "a failure from either", and that conflation produced a memory-safety defect caught in Task 4's review; the spec's "The ring, its depth, and submission" section now carries the full rule.

- [ ] **Step 4: Implement the reaper's completion mapping**

For each completion: look up the token, remove the record, release the lock, **then** invoke the callback — no callback runs under a lock, because it is the phase-7a operation block's entry point and takes its own.

- `res >= 0` → `bytes = res`, no error. Short counts are returned as-is.
- `res < 0` → `-res` is an errno, mapped through Task 2's shared helper.
- `EINTR` belongs to the reaper's wait, not to an operation: retry the wait. There is no per-operation `EINTR` retry, unlike the pool arm's `pread`.
- The kernel caps a single transfer at about 2 GiB (`0x7ffff000`); a larger request returns a **short count, as-is** — the backend does not loop. Not tested, because a buffer that large is not reasonable in CI; record that in the report and in a code comment.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `ctest --preset debug -R IoUringBackendTest --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Mutation-check every test added, then run every Linux gate**

Including `debug-asan`, `debug-tsan`, `release-tests` and `Coverage`. **ThreadSanitizer is the one to watch**: the ring is memory shared with the kernel and liburing's barriers sit in an uninstrumented prebuilt library, so a false race is plausible and this project has no suppression infrastructure at all. If TSan reports, see Task 4a.

- [ ] **Step 7: Commit**

---

### Task 4a: ThreadSanitizer suppressions — only if Task 4 reports

**Files:**
- Create: `tests/tsan.supp`
- Modify: `CMakePresets.json` (`TSAN_OPTIONS` on the `debug-tsan` test preset)

**Skip this task entirely if `debug-tsan` is clean in Task 4.** It exists so that the response is planned rather than improvised.

- [ ] **Step 1: Capture the exact report**

Record the full TSan output in the task report, including the stack, before changing anything.

- [ ] **Step 2: Establish it is a false positive, not a defect**

Explain *why* the reported ordering is in fact enforced — which liburing barrier, on which ring — and why TSan cannot see it. If that argument cannot be made, it is a real race: stop and report, do not suppress.

- [ ] **Step 3: Add the narrowest suppression that silences it**

One entry, with a comment naming what it suppresses and why that is not a real race. A blanket suppression would silently cost the coverage `debug-tsan` exists for.

- [ ] **Step 4: Prove the suppression is load-bearing and narrow**

Confirm the suite is clean with it and reports without it. Then confirm a *deliberately introduced* real race in the backend is still caught with the suppression active — otherwise the suppression is too broad.

- [ ] **Step 5: Commit**

---

### Task 5: The two fsync forms through the ring

**Files:**
- Modify: `src/io_backend_io_uring.cpp`
- Modify: `tests/test_io_backend_io_uring.cpp`

**Interfaces:**
- Consumes: Task 4's record, registry and completion path, unchanged.
- Produces: `io_op::sync_full` and `io_op::sync_data` served by the ring.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoUringBackendTest, SyncFullReachesTheRingAndSucceeds)   // token carries the tag
TEST(IoUringBackendTest, SyncDataReachesTheRingAndSucceeds)   // token carries the tag
```

- [ ] **Step 2: Run them to verify they fail**

Expected: FAIL — both are still forwarded, so the tag assertion fails.

- [ ] **Step 3: Route both to `IORING_OP_FSYNC`**

`sync_full` with no flags; `sync_data` with `IORING_FSYNC_DATASYNC`.

- [ ] **Step 4: Run the tests, mutation-check, commit**

Expected: PASS. The mutation that matters is swapping the two flags: confirm a test fails, so the flag is actually pinned and not merely present.

---

### Task 6: Cancellation

**Files:**
- Modify: `src/io_backend_io_uring.cpp`
- Modify: `tests/test_io_backend_io_uring.cpp`

**Interfaces:**
- Consumes: Task 4's registry and reserved `user_data` values.
- Produces: `cancel(io_token) noexcept` routing by tag; `io_uring_ignored_completions_for_testing` becoming meaningful.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(IoUringBackendTest, ACancelOfAForwardedOperationReachesThePoolBackend)
TEST(IoUringBackendTest, AMidFlightCancelStillDeliversExactlyOneCompletion)
TEST(IoUringBackendTest, ACancelAfterCompletionIsHarmless)
TEST(IoUringBackendTest, ACompletionForAnUnregisteredTokenIsIgnoredAndCounted)
```

**`AMidFlightCancelStillDeliversExactlyOneCompletion` asserts invariants only, never the error code.** For a regular file the operation is usually already running or finished, so `ASYNC_CANCEL` returns `-EALREADY` or `-ENOENT` and the operation completes *normally*, with a byte count and no error. That is not a defect, and the code must not synthesize a cancellation: on the cancelled path the awaiting frame is already gone, and `io_operation.cpp`'s `phase::detached` branch tears the block down **without reading the completion's error or byte count at all**. A test asserting `operation_canceled` would be asserting the outcome of a race the caller cannot observe, and would flake. Put that reasoning in the test's comment.

- [ ] **Step 2: Run them to verify they fail**

- [ ] **Step 3: Implement `cancel`**

Tag says pool → forward to the internal pool backend's `cancel`. Otherwise, **under the registry lock**, if the token is still pending, submit `IORING_OP_ASYNC_CANCEL` targeting that `user_data`. Taking the lock is what stops a cancel racing a completion that is already removing the record. Use `io_uring_prep_cancel` — **not** `io_uring_prep_cancel64`, which is liburing 2.2 and above the floor.

The original operation's completion is what drives the callback. The cancel SQE's own completion carries the reserved `user_data` and is ignored. A completion whose token is not in the registry is ignored and counted.

- [ ] **Step 4: Run the tests, mutation-check, run `debug-tsan`, commit**

Cancellation is where a lock-ordering mistake surfaces, so TSan matters here specifically.

---

### Task 7: Selection, fallback, and a testable fallback path

**Files:**
- Modify: `src/file_io.cpp` (`probe`, and a new `automatic_on_linux`)
- Modify: `src/io_backend_internal.hpp` (declare `automatic_on_linux` and its factory parameter)
- Modify: `tests/test_file_io.cpp`

**Interfaces:**
- Consumes: Task 3's `make_io_uring_backend`.
- Produces: `io_backend_kind::automatic` resolving to io_uring on Linux; `io_backend_kind::io_uring` resolving to a real backend.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(FileIoTest, AutomaticSelectsIoUringOnLinux)                  // backend() == io_backend_kind::io_uring
TEST_F(FileIoTest, AutomaticFallsBackToTheThreadPoolWhenNoRingCanBeMade)  // injected failing factory; backend() == thread_pool
TEST_F(FileIoTest, AnExplicitIoUringRequestSurfacesTheFactorysError)      // injected failing factory; create() fails with that error
```

- [ ] **Step 2: Run them to verify they fail**

Expected: FAIL — `io_uring` currently returns `std::errc::not_supported` from `probe`.

- [ ] **Step 3: Add the Linux branch to `probe`, with the factory injectable**

`probe` currently takes `(thread_pool&, io_backend_kind)` and must also take the queue depth and `max_in_flight`, with `create()` passing `options.queue_depth` and `options.max_in_flight`. It is a private function in an anonymous namespace, so no public signature changes.

`automatic_on_linux(thread_pool&, std::size_t queue_depth, std::size_t max_in_flight, factory)` takes the factory as a parameter defaulted to the real one, declared in `src/io_backend_internal.hpp` so a test can pass one that fails. That header exists for exactly this — its own comment says the IOCP backend builds on it "and tests include this header to exercise the seam". **An environment variable read from `src/` was considered and rejected**: it would be a permanent production branch serving only a test.

`automatic` falls back to the thread pool; an explicit `io_uring` request fails with the factory's own error. This mirrors `automatic_on_windows` exactly.

- [ ] **Step 4: Run the tests, mutation-check, commit**

---

### Task 8: Run the file layer on io_uring, and keep the thread pool covered

**Files:**
- Modify: `tests/file_io_test_support.hpp` (accept `io_uring`)
- Modify: `tests/CMakeLists.txt` (Linux environment and a new ctest entry)
- Modify: `tests/test_io_backend_io_uring.cpp` (the availability canary)

**Interfaces:**
- Consumes: Task 7's selection.
- Produces: the whole `FileIo*`/`AsyncFile*` suite running against io_uring.

- [ ] **Step 1: Teach the harness the new backend name**

`file_io_options_under_test` in `tests/file_io_test_support.hpp` accepts `"iocp"`, `"thread_pool"` or unset, and **reports any other value as a test failure**. Add `"io_uring"`. Without this, `ALT_TEST_IO_BACKEND=io_uring` fails every test it touches.

- [ ] **Step 2: Fix the coverage shift this phase causes, and the comment that asserts it**

On Linux the discovered tests currently leave `ALT_TEST_IO_BACKEND` unset, so the fixtures take `automatic` — and `tests/CMakeLists.txt` carries a comment stating that this "is the thread pool there". **Task 7 makes that false.** Left alone, the whole file layer would silently switch to io_uring on Linux and stop covering the thread pool.

Set `ALT_TEST_IO_BACKEND=thread_pool` for the discovered tests on Linux as well as Windows, and correct the comment to say why.

- [ ] **Step 3: Add the io_uring file-layer ctest entry**

Mirror the IOCP entry: a plain `add_test` named `file_layer_on_io_uring` running `--gtest_filter=FileIo*:AsyncFile*` with `ALT_TEST_IO_BACKEND=io_uring` and `TIMEOUT 300`. A plain `add_test` rather than a second `gtest_discover_tests`, so nothing new depends on discovery — see issue #68.

- [ ] **Step 4: Add the availability canary**

```cpp
TEST(IoUringAvailability, TheRunnerProvidesIoUring)  // FAILS, does not skip, when no ring can be created
```

Per-test skipping elsewhere uses the same helper; **this one test fails outright**, so a runner that stops permitting io_uring turns CI red instead of leaving the entire backend unexercised behind a green sweep. Its name and comment must say that is its whole job, so nobody later "fixes" it into a skip.

- [ ] **Step 5: Run both file-layer runs and the whole suite**

Run: `ctest --preset debug --output-on-failure`
Expected: all pass, including `file_layer_on_io_uring`. Report the new total.

- [ ] **Step 6: Every Linux gate, then commit**

`debug`, `debug-asan`, `debug-tsan`, `release-tests`, `debug-coverage` plus its `coverage` target, pinned `clang-tidy-19`, `cppcheck`, `clang-format`.

---

### Task 9: Version bump to 1.18.0

**Files:** `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

- [ ] **Step 1: Change all six**

`CLAUDE.md`'s "Version bump" table is authoritative. `tests/test_version.cpp` has **three** places, not two: the `EXPECT_EQ` macros, the `static_assert`s on `alt::version_patch` and friends, and the version string. The `static_assert` spells its number bare, so a grep for the old string misses it and **the build, not `ctest`**, is what reports it. In `README.md` and `CLAUDE.md`, the `find_package` compatibility example illustrates the rule rather than this release: adjust the operands so the prose still agrees with the snippet above it.

- [ ] **Step 2: Build and test**

Run: `cmake --build --preset debug && ctest --preset debug --output-on-failure`
Expected: all pass. A failure to build is the `static_assert`.

- [ ] **Step 3: Commit as its own commit**

Message exactly: `Bump version to 1.18.0`. Never bundled into a feature or merge commit.

---

## Notes for the executor

- Branch `feature/io-uring-backend` already exists with the design committed. Do not create another.
- A bare branch push does not run CI. Open a draft PR, or `gh workflow run CI --ref feature/io-uring-backend`, to see the eleven jobs before the PR is ready.
- `gh issue view` and `gh pr edit` are broken in this environment (classic-projects GraphQL error). Use `gh api repos/Altainia/alt/issues/<n> --jq .body` to read an issue, and `gh api repos/Altainia/alt/pulls/<n> -X PATCH --input <json>` to edit a PR. **`gh pr edit` fails silently mid-pipeline** — read back anything you write.
- `gh pr checks` has no `--json` flag here. A CI watcher must resolve the run from `git rev-parse HEAD`, because right after a push `gh pr checks` still reports the *previous* commit's finished run.
- The PR references issue #49 and does not close it: #49 is the parent for all three phases and 7b is the last, so closing it is a judgement for the user once they are satisfied the whole feature is done.
- Deferred findings are filed as GitHub issues, never left only in a plan or review report: the plan workspace is git-ignored and deleted when the branch finishes.

# Design: phase 7b, the io_uring backend

**Date:** 2026-11-21
**Status:** Proposed
**Related:** `2026-11-20-awaitable-file-io-design.md` (phase 7a, the interface and the
thread-pool backend), `2026-11-21-iocp-backend-design.md` (phase 7c, the IOCP backend),
`2026-09-10-coroutines-roadmap.md` (phase 7's place in the family). Parent issue #49.

## Intent

Phase 7a built the awaitable file I/O interface and a thread-pool backend behind it.
Phase 7c added IOCP, so Windows performs reads and writes as overlapped I/O. This phase
does the same for Linux with io_uring, which is the last of the three backends issue #49
asks for and the last phase of the roadmap's phase 7.

The point is not throughput benchmarks. It is that the downstream consumer — a server
moving its file I/O off its loop thread — gets reads and writes that never occupy a
worker thread while the kernel is working, on the platform it actually runs on, through
the interface it already codes against. Everything above the backend seam is already
built and tested; this phase fills in one more implementation of it.

## Why now

Phase 7c is merged and phase 7b is the only phase of 7 left. The seam was built for it:
`io_backend_kind::io_uring` already exists in the public enum and currently resolves to
`std::errc::not_supported`, `file_io_options::queue_depth` is already public and
documented as "the submission ring size for a backend that has one" with **no code
reading it**, and the comment on `probe()` in `src/file_io.cpp` says in as many words that
"a later backend adds its branch ahead of the fallback in the same way". This phase is
the one that makes those three things true.

## Goals

- `read`, `write`, `sync_full` and `sync_data` submitted to an io_uring ring rather than
  occupying a thread-pool worker for their duration.
- Selected automatically on Linux when a ring can be created, with the thread-pool
  backend as the fallback when it cannot.
- Every guarantee the seam already makes, unchanged: completions posted to the
  caller-chosen scheduler and never run inline on an I/O thread, buffers kept alive until
  the kernel is genuinely finished with them, cancellation that does not resume the
  awaiting frame, bounded in-flight work.
- `file_io_options::queue_depth` becomes meaningful.
- Clean under AddressSanitizer and ThreadSanitizer, as the rest of the coroutine
  components are.

## Non-goals

| Excluded | Reason |
|----------|--------|
| Metadata operations through the ring (`open`, `stat_handle`, `stat_path`, `rename`, `unlink`) | `OPENAT`, `STATX`, `RENAMEAT` and `UNLINKAT` exist, but arrived across kernels 5.6 to 5.11, so each needs an `IORING_REGISTER_PROBE` check plus a tested fallback. That is a runtime matrix for operations that are rare and not latency-critical. Decided in conversation; see Approaches. |
| `list_directory` and `invoke` through the ring | No opcode exists. `getdents` is not exposed by io_uring, and `invoke` runs an arbitrary callable. These *must* stay on the pool whatever else is decided. |
| Registered buffers (`IORING_REGISTER_BUFFERS`) and registered files | A genuine throughput win, but pre-registration conflicts with `io_buffer`'s model, where a caller brings arbitrary memory through `borrow()` or `borrow_unsafe()`. Worth a follow-up issue once this lands. |
| `IORING_SETUP_SQPOLL` | Needs privileges and dedicates a core to polling. Wrong default for a general-purpose library. |
| `IORING_SETUP_IOPOLL`, multishot operations, linked SQEs (`IOSQE_IO_LINK`) | None is needed by the operation set, and each adds a failure mode to reason about. |
| Buffering, readahead, or any caching policy | The backend performs exactly the operation asked of it. |

## Anti-goals

These would make the phase a failure even if the code worked:

- **A backend whose completions can run on the ring's reaper thread.** The single
  guarantee the downstream consumer depends on is that its single-threaded loop keeps its
  threading rules. A completion that reaches user code on the reaper thread breaks the
  reason this whole component exists.
- **A buffer freed while the kernel still owns it.** io_uring completes operations *after*
  a cancel request, not at it. The 7a operation block exists precisely for this; this
  backend must not find a way around it.
- **A new mandatory dependency that is not confined to Linux.** The dependency decision
  below is deliberate, but it must not leak into the Windows or macOS builds, or into any
  public header.
- **A backend that silently is not tested.** If a CI runner stops permitting io_uring and
  the suite goes green having exercised none of this, the phase has delivered nothing it
  can prove. See Testing.

## Constraints

- **C++23**, GCC 13+ / Clang 19+ / MSVC 19.50+, `snake_case`, `m_` on private members,
  `const` by default, Doxygen on public symbols, American spelling (with the double-l
  `cancelled` / `cancellation` forms approved). `CLAUDE.md` governs.
- **The backend seam is fixed and public.** `io_backend::submit(const io_request&,
  io_completion_fn, void* state) -> std::expected<io_token, std::error_code>`,
  `cancel(io_token) noexcept`, `kind() const noexcept`. The completion callback is a plain
  function pointer, `noexcept`, may fire inline inside `submit`, and runs exactly once. A
  backend must copy whatever it needs from the request before `submit` returns.
- **`io_backend` is library-private at this layer.** `src/io_backend_iocp.hpp` is not
  installed and never names `windows.h` from `include/alt/`; the io_uring header follows
  that rule and never names `liburing.h` from a public header.
- **Eleven CI jobs**, eight of them on Linux. All must pass. The three MSVC jobs must be
  unaffected by this work, which is a real constraint on where the dependency is wired.
- **liburing 2.1 is the floor** — the version in Ubuntu 22.04 and Debian 12, whose package
  name is `liburing2`. Decided in conversation. Consequences the builder must respect:
  - `io_uring_prep_cancel64` arrived in liburing 2.2 and is **not available**; use
    `io_uring_prep_cancel`, or prepare the cancel SQE directly.
  - `io_uring_queue_init_params`, `IORING_SETUP_CQSIZE` and `io_uring_get_probe_ring` are
    all present at 2.1, so the design above needs nothing newer.
  - The development machine has 2.14 and CI's runner has 2.5, both supersets — so **the
    floor is not exercised by any gate.** Using a post-2.1 helper would compile and pass
    everywhere here and break only for a consumer on 22.04. The plan should state how it
    avoids that, even if the answer is care rather than a check.
  - Kernel floor follows for free: anything passing the capability check has
    `ASYNC_CANCEL`, hence kernel 5.5+, hence `IORING_SETUP_CQSIZE`.

## Approaches

Three decisions were taken in conversation before this document. Each had real
alternatives, so each is recorded with what it trades.

### 1. How to talk to the kernel

| Option | Trade |
|--------|-------|
| **liburing as a hard dependency on Linux** — **chosen** | Simplest, least error-prone code; liburing owns the ring's memory barriers, which are the part most likely to harbor a bug no test would find. Costs every Linux consumer a dependency: `find_dependency` in the package config, `Depends:` in the `.deb`, and `liburing-dev` in CI. |
| Raw `io_uring_setup` / `io_uring_enter` syscalls | Keeps alt dependency-free, which it has been until now (`find_dependency(Threads)` is its only entry). But we would own the submission and completion memory barriers against the kernel, and a barrier bug there is invisible to ThreadSanitizer because the ordering is with the kernel rather than another thread. |
| liburing as an optional, auto-detected dependency | No new hard requirement, and the backend simply is not compiled where liburing is absent — which the existing `not_supported` path already models. Costs a build-configuration axis that needs its own CI job, or the without-liburing path rots. |

**Decision:** hard dependency, Linux only. The user's reason: not wanting to fracture the
project on a build-configuration axis. The barrier-correctness argument points the same
way.

### 2. Which operations go through the ring

| Option | Trade |
|--------|-------|
| **The four fd operations** — `read`, `write`, `sync_full`, `sync_data` — **chosen** | The hot path, and the reason the downstream consumer wants io_uring at all. Also collapses issue #49's "fall back when an operation is unsupported" into **one all-or-nothing capability check** at construction, rather than a per-operation runtime matrix. (An earlier draft claimed any kernel that can create a ring supports these. That is false — see "Capability check" below — but the fix is one check, not a matrix, so the decision stands.) |
| Add the metadata operations behind per-op probes | More of the API leaves the pool threads, at the cost of an `IORING_REGISTER_PROBE` check and a tested fallback per opcode, for operations that are rare and not latency-critical. |
| Reads and writes only | Strictest symmetry with IOCP, but the two `fsync` forms are one opcode away and genuinely block a worker, so leaving them behind gives up something nearly free. |

**Decision:** the four fd operations.

### 3. The backend's structure

There is one sensible structure, and it is the one phase 7c established. It is recorded
here with the alternatives so that a later reader knows they were considered.

- **Chosen: a ring plus one reaper thread, composing an internal thread-pool backend** for
  the seven operations it does not handle, with tagged tokens routing `cancel`.
- *A self-contained backend handling all eleven operations* — rejected: `list_directory`
  and `invoke` have no ring form at all, and the thread-pool backend already performs the
  rest correctly and portably. Composition is why `detail::pool_open_policy` exists.
- *Folding the ring into the thread-pool backend as a second mode* — rejected: it merges
  two unrelated lifetime models into one class and discards the `io_backend_kind`
  selection the public API already exposes.
- *An eventfd registered with the caller's event loop instead of a reaper thread* —
  rejected: issue #49 requires no dependency on a particular event loop, and completions
  must be posted to a caller-chosen scheduler regardless, so the thread is both simpler
  and consistent with IOCP.

## One asymmetry with IOCP that simplifies this phase

IOCP transfers are valid only on handles that backend opened itself, because they need
`FILE_FLAG_OVERLAPPED` and association with the port. That is why `detail::pool_open_policy`
exists, and why issue #53 had to route `async_file::adopt()` to the pool backend.

**io_uring has no equivalent requirement.** It operates on ordinary file descriptors. So:

- The internal pool backend is built with the **default** `make_thread_pool_backend(pool)`.
  No `pool_open_policy`, no `prepare` hook, no extra open flags.
- Descriptors that arrived through `async_file::adopt()` work with the ring exactly as
  descriptors this service opened. The whole class of problem behind #53 does not recur
  here, and the builder should not go looking for it.

This asymmetry should be stated in the backend's own documentation, because a reader
coming from the IOCP backend will expect the handle-preparation machinery and should learn
immediately why there is none.

## The design

### Files

| Path | Contents |
|------|----------|
| `src/io_backend_io_uring.hpp` | Factory and test accessors, entire contents guarded by `#ifdef __linux__`, exactly as `io_backend_iocp.hpp` guards on `_WIN32`. Declares nothing elsewhere. |
| `src/io_backend_io_uring.cpp` | The backend. The only translation unit that includes `<liburing.h>`. |
| `src/file_io.cpp` | A Linux `automatic` branch and the `io_uring` case in `probe()`; `probe()` gains the queue depth. |
| `tests/test_io_backend_io_uring.cpp` | The backend's own tests, registered in `tests/CMakeLists.txt`. |
| `CMakeLists.txt`, `cmake/AltConfig.cmake.in`, `packaging/`, `.github/workflows/ci.yml`, `CLAUDE.md`, `README.md` | Dependency wiring and documentation. |

`src/` is globbed with `GLOB_RECURSE`, so the new `.cpp` needs no source-list edit, only a
configure re-run.

### The factory

```cpp
namespace alt::detail
{
    /// Set on every token for a ring operation; a token without it came from the internal pool backend.
    inline constexpr std::uint64_t io_uring_token_tag = std::uint64_t{1} << 63U;

    [[nodiscard]] std::expected<std::unique_ptr<io_backend>, std::error_code>
      make_io_uring_backend(thread_pool& pool, std::size_t queue_depth, std::size_t max_in_flight);
}
```

Mirrors `make_iocp_backend` in shape, plus the two bounds. Returns the error that stopped
the backend being created — `ENOSYS`, `EPERM`, or `std::errc::not_supported` from the
capability check — so an explicit `io_backend_kind::io_uring` request reports a real reason
rather than a generic failure.

### Operation routing

`submit` switches on `request.op`:

- `read` → `IORING_OP_READ` at `request.offset` into `request.buffer`, for
  `min(request.size, request.buffer.size())`. That clamp is `transfer_size()` in
  `io_backend_thread_pool.cpp`'s anonymous namespace today; **move it, and the errno
  mapping, into `src/io_backend_internal.hpp`** so both backends share one definition
  rather than agreeing by coincidence.
- `write` → `IORING_OP_WRITE`, same shape.
- `sync_full` → `IORING_OP_FSYNC` with no flags.
- `sync_data` → `IORING_OP_FSYNC` with `IORING_FSYNC_DATASYNC`.
- everything else → forwarded to the internal pool backend through a trampoline that
  counts the operation while it is in flight, exactly as the IOCP backend's forwarding
  does. The pool's token is returned unchanged, and the tag bit distinguishes it.

`kind()` returns `io_backend_kind::io_uring`.

### The ring, its depth, and submission

**What `queue_depth` bounds.** Without `SQPOLL` the kernel consumes submission entries
synchronously inside `io_uring_submit`, and `submit` holds its mutex across both the SQE
acquisition and the submit, so the submission queue is drained again by the time the lock
is released. A full *submission* queue is therefore all but unreachable at any depth. What
actually bounds outstanding work is the **completion** queue, which defaults to twice the
submission queue and, when it overflows, makes `io_uring_submit` fail or forces the kernel
to hold completions in a backlog.

So the design is:

- `queue_depth` is the **submission** ring size: `queue_depth` when non-zero, otherwise
  derived from `max_in_flight`. The kernel requires a power of two and rounds up; the
  derivation rounds up and clamps explicitly, so the value the test accessor reports is the
  value in effect.
- **The completion ring is sized explicitly** with `IORING_SETUP_CQSIZE` via
  `io_uring_queue_init_params`, to at least `max_in_flight` plus the reserved internal
  completions (the wake-up NOP and one cancel per in-flight operation). Completion-queue
  overflow is then unreachable *by construction* rather than by assumption, whatever
  `queue_depth` the caller passes.
- Consequently **the factory takes `max_in_flight` as well as `queue_depth`**. The two
  bound different rings and only the caller knows both.
- **Submission is serialized by a mutex.** liburing's `io_uring_get_sqe` and
  `io_uring_submit` are not safe to call concurrently on a shared ring, and `submit` may be
  called from any thread.
- A failure from `io_uring_get_sqe` or `io_uring_submit` is reported as its errno and never
  waited on, because `submit` must not block. Given the sizing above this path is
  unreachable; it is implemented because an unreachable-by-construction argument is still
  an argument, and it is **documented as untested for that reason** rather than given a
  test that cannot fail. An earlier draft claimed a `queue_depth` below `max_in_flight`
  would reach it and prescribed a test to force it — that was wrong on both counts, since
  the submission queue drains on every call and the completion queue is now sized
  independently.

### Capability check

Rings have existed since kernel 5.1, but `IORING_OP_READ` and `IORING_OP_WRITE` arrived in
**5.6** and `IORING_OP_ASYNC_CANCEL` in **5.5**. On a 5.1–5.5 kernel
`io_uring_queue_init` therefore *succeeds* and every read or write then fails with
`EINVAL`. Creating a ring is not sufficient evidence that this backend can work.

The factory runs **one** check, after creating the ring and before returning it:
`io_uring_get_probe_ring` must report `READ`, `WRITE`, `FSYNC` and `ASYNC_CANCEL` all
supported. If any is missing the factory fails with `std::errc::not_supported`, so
`automatic` falls back to the thread pool and an explicit `io_uring` request reports why.

All-or-nothing is what keeps the decision above intact: one error path and one test, not a
per-operation matrix. The check costs one extra syscall once per service.

### Records and `user_data`

Each ring operation gets a record holding the copy of `io_buffer` that keeps the bytes
alive, the completion function pointer and its state, and the operation kind. Records live
in a registry keyed by token, under a mutex.

**The SQE's `user_data` carries the token value, not a record pointer.** A cancelled or
stale completion can therefore be looked up and found absent, instead of dereferencing a
pointer to a record that is gone. The IOCP backend has the same problem and solves it by
validating the `OVERLAPPED` address against its registry; carrying the token is the same
idea expressed so that the unsafe operation does not exist. Two `user_data` values are
reserved and belong to no operation: one for the wake-up NOP and one for cancel SQEs.

A completion whose token is not in the registry is **ignored and counted**, and the count
is exposed for testing, mirroring the IOCP backend's ignored-packet counter.

### The reaper thread

One thread, created by the factory and joined by the destructor. It blocks in
`io_uring_wait_cqe`, and for each completion: looks the token up, removes the record,
releases the registry lock, and invokes the callback. The callback must run with no lock
held, since it is the 7a operation block's entry point and will take its own.

Mapping a completion to `io_completion`:

- `cqe->res >= 0` → `bytes = res`, no error. A read at or past end of file yields `0`
  bytes and no error, which is what the pool backend already reports and what the file
  layer already treats as a real result. Short counts are returned as-is.
- `cqe->res < 0` → `-res` is an errno, mapped by the **shared** helper moved to
  `io_backend_internal.hpp`. The documented contract names `std::errc` conditions and there
  must be exactly one place that decides them. `EISDIR` and friends arrive from the kernel
  and map through it unchanged, so parity with the pool arm is structural rather than
  maintained by hand.
- The kernel caps a single read or write at about 2 GiB (`0x7ffff000`). A larger request
  comes back as a **short count, returned as-is** — the backend does not loop. The file
  layer already treats a short count as a real result, and looping here would hide a
  partial transfer the caller is entitled to see.
- `EINTR` belongs to the reaper's wait, not to an operation: `io_uring_enter` can return it
  while a submitted operation cannot. The reaper loop retries on it; there is no
  per-operation `EINTR` retry, unlike the pool arm's `pread`.
- A cancelled operation completes with `ECANCELED`, which maps to
  `std::errc::operation_canceled`. That completion is the one that drives the callback;
  the cancel SQE's own completion is reserved `user_data` and is ignored.
- **But a cancel usually loses the race.** For an ordinary file the operation is typically
  already running or finished, so `ASYNC_CANCEL` returns `-EALREADY` or `-ENOENT` and the
  operation completes *normally*, with a byte count and no error. This is not a defect and
  the code must not try to synthesize a cancellation: on the cancelled path the awaiting
  frame is already gone, and `src/io_operation.cpp`'s `phase::detached` branch tears the
  block down **without reading the completion's error or byte count at all**. The error
  code on a cancelled operation is therefore unobservable, which is what makes the race
  harmless.

**Shutdown** submits a NOP with the reserved wake-up `user_data` so the thread returns
from `wait_cqe` immediately, rather than polling with a timeout.

### Cancellation

`cancel(token)` is a hint, as the seam says.

- Tag says pool → forward to the internal pool backend's `cancel`.
- Otherwise, under the registry lock, if the token is still pending, submit
  `IORING_OP_ASYNC_CANCEL` targeting that `user_data`. Taking the lock is what stops a
  cancel racing a completion that is already removing the record.
- The original operation still completes, and that completion is what resumes the awaiting
  frame. Nothing about cancellation shortcuts the completion path.

### Destruction

Destroying the backend with **any** operation in flight — a ring operation or an operation
forwarded to the pool whose callback has not yet run — is a precondition violation. It
asserts, and under `NDEBUG` terminates, rather than free a record the kernel may still
own. This is the IOCP backend's rule and its wording should be reused; `~file_io` already
waits for backend quiescence, so `file_io` never does this.

Order: verify quiescence, wake the reaper with the NOP, join it, then
`io_uring_queue_exit`.

### Selection and fallback

`src/file_io.cpp` gains the mirror image of what Windows has:

```cpp
#ifdef __linux__
/** Linux' `automatic`: io_uring, or the thread pool if a ring cannot be created. */
built_backend automatic_on_linux(thread_pool& workers, std::size_t queue_depth);
#endif
```

and in `probe()`, a Linux branch for `automatic` and a real `io_uring` case. `automatic`
falls back to the pool; an explicit `io_uring` request fails with the ring's own error,
which is precisely how `iocp` behaves today.

`probe()` currently takes `(thread_pool&, io_backend_kind)` and must also take the queue
depth and `max_in_flight`, so `create()` passes `options.queue_depth` and
`options.max_in_flight` through. This is a private function in an anonymous namespace; no
public signature changes.

**The fallback must be testable on a machine that does have io_uring.** `automatic_on_linux`
therefore takes the factory as a parameter defaulted to the real one, declared in
`src/io_backend_internal.hpp` so a test can call it with a factory that fails. That header
already exists for exactly this — its own comment says the IOCP backend builds on it "and
tests include this header to exercise the seam" — so this follows precedent rather than
adding a test hook to production code. An environment variable read from `src/` was
considered and rejected: it would be a permanent production branch serving only a test.

Ring creation fails with `ENOSYS` where the kernel has no io_uring, and with `EPERM` where
`kernel.io_uring_disabled` is set or seccomp forbids the syscall — which is common in
containers. Both must reach the fallback, so the fallback is a normal operating condition
and not an error path.

### Build and packaging

- `CMakeLists.txt`: on Linux, locate liburing through `PkgConfig` (`liburing.pc` ships
  with it; CMake has no `FindLiburing`) and link it **`PRIVATE`** — no public header names
  it. Configure must fail with a clear message when it is missing, since it is a hard
  dependency.
- `cmake/AltConfig.cmake.in`: alt is built by `add_library(alt ${ALT_SOURCES})` with no
  explicit type, so it is **static** unless `BUILD_SHARED_LIBS` is on. A static library
  propagates even `PRIVATE` link dependencies to consumers at link time, so the package
  config must find liburing again on Linux, beside the existing `find_dependency(Threads)`.
  This is easy to get wrong and invisible in-tree; a consumer-side `find_package(Alt)`
  check belongs in the plan.
- `packaging/`: the `.deb` gains a runtime dependency on `liburing2` beside `libc6`, and a
  build dependency on `liburing-dev` if the control template carries one. `liburing2` is
  the package name from Ubuntu 22.04 and Debian 12 onward; older releases ship `liburing1`
  and are out of scope, which is moot anyway since they cannot provide GCC 13.
- `.github/workflows/ci.yml`: `liburing-dev` installed in each of the eight Linux jobs,
  including `clang-tidy`, `cppcheck` and `Coverage`, which all configure the project.
- `CLAUDE.md` and `README.md`: record liburing as a Linux build prerequisite.

### Version

**1.18.0**, as its own commit on the branch, per `CLAUDE.md`'s six-file table. A minor
bump: a new capability plus a packaging change. Decided in conversation.

## Testing

The suite is where this phase is most likely to go wrong, because the backend's
correctness is mostly about lifetime and ordering rather than results.

**Availability.** A helper reports whether a ring can be created. Per-test skipping uses
it, **and one test fails outright when io_uring is unavailable** — a deliberate canary, so
that a runner which stops permitting io_uring turns CI red instead of leaving the whole
backend unexercised behind a green sweep. That test's name and comment must say that is
its entire job, so nobody later "fixes" it into a skip.

**Reuse the file-layer harness.** Phase 7c added `tests/file_io_test_support.hpp` and runs
the file-layer suite against the IOCP backend. The io_uring backend should go through the
same harness rather than growing a parallel set of file-layer tests.

**What needs its own test**, beyond the shared harness:

- Short read, read at end of file, read past end of file.
- A write at an offset, and an append write landing at the end.
- Both `fsync` forms reaching the backend and succeeding.
- Transfers larger than one SQE's worth of work, and enough concurrent operations to fill
  the ring, which is where the submission mutex and the permit bound interact.
- The depth actually in effect, through the test accessor, for `queue_depth` zero and
  non-zero, and that the completion ring is sized at least to cover `max_in_flight` plus
  the reserved internal completions.
- The capability check: all four opcodes present on this machine, and — since it cannot be
  made to fail here — that the factory's `not_supported` path is reached when the check is
  made to report a missing opcode. If that cannot be injected without a production hook,
  the plan says so and the path goes untested rather than acquiring a test that cannot
  fail.
- A mid-flight cancel: **assert the invariants, not the error code.** Exactly one
  completion arrives, the frame does not resume, and the buffer outlives the kernel's use
  of it. The operation may complete normally or with `operation_canceled` depending on a
  race the caller cannot observe (see Cancellation), so a test asserting
  `operation_canceled` would be asserting the outcome of a race and would flake.
- A cancel after completion: harmless.
- A completion whose token is no longer registered: ignored, and the counter moves.
- Destruction preconditions, as death tests, following
  `2026-11-17-precondition-death-tests-design.md`.
- Operations forwarded to the pool, and that their tokens carry no tag.
- Fallback: with a deliberately failing factory injected through
  `io_backend_internal.hpp`, `automatic` yields a thread-pool backend and reports
  `io_backend_kind::thread_pool` from `file_io::backend()`, while an explicit `io_uring`
  request surfaces the factory's error. Both halves are testable on a machine that has
  io_uring, which is why the factory is a parameter.

**Every test carrying a guarantee gets a mutation of record** — break what it guards,
confirm it fails, revert, record the count. This project's dominant historical defect is a
test, name, or comment claiming more than the code delivers, and a guard test with no
mutation on file is treated as a finding rather than as coverage.

## Risks

**ThreadSanitizer and the shared ring.** The ring is memory shared with the kernel, and
liburing's acquire/release barriers live in a prebuilt library TSan has not instrumented.
TSan may therefore report races on a buffer the kernel wrote, or on the ring itself, with
no real defect behind them. **This project has no sanitizer suppression infrastructure at
all today** — no `.supp` files, nothing in the presets. If such a report appears, building
that infrastructure is part of this phase, and a suppression must be narrow and carry a
comment saying what it suppresses and why that is not a real race. A blanket suppression
would silently cost the coverage that `debug-tsan` exists for.

**Kernels between 5.1 and 5.5.** They create rings but lack `READ`/`WRITE`. The capability
check is the mitigation, and it is the one place where getting this wrong produces a
backend that is selected and then fails every transfer. It needs a test that the check is
actually consulted, not merely present.

**AddressSanitizer and kernel writes.** Lower risk: the kernel writes within an allocation
ASan knows about. Worth confirming early rather than assuming.

**CI runner support.** io_uring works on the development machine — a ring was created,
`READ`, `WRITE`, `FSYNC`, `ASYNC_CANCEL` and `NOP` all report supported, and
`kernel.io_uring_disabled` is `0`. GitHub's runners are not verified. The canary test
above is the mitigation; if it fails on first push, the decision about what CI should do
comes back to the user rather than being quietly softened into a skip.

**The static-library dependency propagation.** Getting `AltConfig.cmake.in` wrong produces
a package that configures in-tree and fails only for a downstream consumer. It needs a
deliberate check, not an assumption.

## What is left to the builder

- The exact liburing call sequence, and whether the reaper uses `io_uring_wait_cqe` or
  `io_uring_submit_and_wait`.
- The registry's data structure, and whether the submission mutex and the registry mutex
  are one lock or two. One is simpler; two reduce the window a completion holds against a
  submission. Either is acceptable if the lock order is documented and no callback runs
  under a lock.
- The depth derivation's exact rounding and clamping.
- Where the pool backend's errno mapping is reached from, which may need a small
  internal declaration rather than duplication.
- Test helper and accessor names, following the IOCP backend's `*_for_testing` convention.
- Whether the shared `transfer_size` and errno mapping move to `io_backend_internal.hpp` as
  inline functions or as declarations with one definition; either is fine if there is
  exactly one definition.

## Calls made after the builder check

A fresh engineer read this document as the person who would have to plan from it. These
are the things it raised that are settled here rather than left open, so that the reasoning
is on record:

| Raised | Settled as |
|--------|-----------|
| `queue_depth`'s real meaning, given the submission queue drains on every submit | It sizes the submission ring; the completion ring is sized independently with `IORING_SETUP_CQSIZE`. The factory takes both bounds. The earlier "full queue" test was wrong and is removed. |
| Whether opcode support needs probing | Yes — one all-or-nothing check. The earlier claim that ring creation implies support was false. |
| Whether a cancelled operation must report `operation_canceled` | No. The `detached` branch never reads the completion, so tests assert invariants only. |
| Parity with the pool arm's clamping, `EINTR` and error mapping | Share one `transfer_size` and one errno mapping via `io_backend_internal.hpp`; `EINTR` is the reaper's concern; the ~2 GiB cap is a short count returned as-is. |
| How to force the `automatic` fallback test | Inject the factory through `io_backend_internal.hpp`, following the precedent that header was created for. No environment variable in `src/`. |
| When to build TSan suppression infrastructure | Reactively, only if a report appears, wired through `TSAN_OPTIONS` on the `debug-tsan` test preset so CI inherits it. |
| Where the consumer-side `find_package` check lives | `tests/package/consumer`, which already exists as a `find_package(Alt)` harness. The package config needs a snippet defining the same imported target name the static-library export references, not a bare `find_dependency(PkgConfig)`. |
| Whether a Linux user without liburing can build at all | No, and that is intended: the dependency is hard, by decision, with no opt-out. |
| The liburing version floor | 2.1 (Ubuntu 22.04 / Debian 12, package `liburing2`). Decided in conversation. No gate exercises the floor, which is itself a risk the plan must address. |

# Design: phase 7c, the IOCP backend

Phase 7c of [awaitable file I/O](2026-11-20-awaitable-file-io-design.md) (issue #49).
That document is the parent: everything it settles (the seam contract, operation state
outside the frame, cancellation as detach, backpressure, shutdown, thread affinity)
holds here unchanged and is not repeated. This one covers what 7c adds.

## Intent and the why

alt's coroutine file I/O works on Windows today, but only through the thread-pool
backend: every read and write occupies a pool worker for its whole duration. 7c gives
Windows the platform backend the parent design always planned: overlapped reads and
writes completed through an I/O completion port, so an in-flight transfer costs no
thread at all.

The maintainer's framing: this "buttons up" alt's Windows support for the coroutine
features. There is no Windows consumer waiting on it (`cpwf`, the only known consumer,
is Linux-only), so the bar is correctness and completeness rather than a deadline, and
the design should leave nothing half-settled behind it. It also settles issue #53, whose
decision (recorded in the parent design) is implemented here.

## Goals

1. On Windows, `file_io::create` with `io_backend_kind::automatic` or `iocp` gives a
   backend that performs `read_at` and `write_at` as overlapped I/O completed through a
   completion port, holding no thread while a transfer is in flight.
2. Every other operation (open, stat, sync, rename, unlink, list_directory, `run()`) keeps
   running on the borrowed thread pool, as the parent design specifies for every
   platform.
3. The seam (`io_backend`, `io_request`, `io_completion`, `io_token`) is not widened.
4. Adopted files work on an IOCP `file_io`, are served by the thread pool (issue #53), and
   each `async_file` reports which backend serves it.
5. The whole existing file-layer suite passes against the IOCP backend, not only against
   the thread pool.

## Non-goals

- **Making adopted files use IOCP.** Decided against in #53 for now. It upgrades
  additively later, by giving the seam handle lifetimes.
- **IOCP for anything but read and write.** Windows has no asynchronous open, stat,
  rename, unlink or directory listing. `FlushFileBuffers` has no overlapped form, so sync
  stays on the pool too.
- **Sockets**, as in the parent design.
- **Exposing the completion port** or letting a consumer supply one.

## Anti-goals

The parent's anti-goals all apply. Two weigh most here:

- **A use-after-free on an `OVERLAPPED` or a buffer the kernel still owns.** Every exit
  path of a transfer must leave the kernel finished with both before either is released:
  synchronous success, pending then completed, pending then cancelled, immediate failure,
  and shutdown.
- **A silent backend fallback.** If `automatic` cannot build IOCP it falls back to the
  thread pool, and `file_io::backend()` says so; an explicit `iocp` request that cannot be
  met is an error. An adopted file's routing is reported per file.

New to 7c:

- **A cancel that reaches the wrong operation.** `CancelIoEx` matches a pending request by
  handle and `OVERLAPPED` address. If a cancel for a finished operation ran after its
  `OVERLAPPED` was freed and the address reused by a new transfer on the same handle, it
  would cancel the new one. Cancel and completion must be serialized so that cannot
  happen.

## Constraints

- **A handle can be associated with only one completion port, for its whole life**
  (documented on `CreateIoCompletionPort`), and the association ends only when the handle
  is closed. Overlapped I/O also requires `FILE_FLAG_OVERLAPPED` at open time. Both are
  why only handles this backend opened itself can use the port.
- **A backend never sees a close.** `io_handle`'s destructor calls `CloseHandle`
  directly. This is fine here, because association lives on the handle and ends with it,
  so the backend keeps no per-handle state. It is also why adopted handles cannot be
  handled inside the backend (see #53 in the parent).
- **Stat, sync and pool transfers will run on handles that are associated with our port.**
  A synchronous API (`FlushFileBuffers`, `GetFileInformationByHandle`) is unaffected. A
  pool-arm overlapped transfer would post a packet to our port, naming the worker's stack
  `OVERLAPPED`. Since #52 the pool arm sets the low-order bit of `hEvent`, which is
  Windows' documented way of suppressing that packet. 7c relies on it, and a test must pin
  it (see Testing).
- **MSVC's STL hides cancellation defects that libstdc++ would not**, and the reverse
  (CLAUDE.md). Every cancellation test here runs on the three MSVC CI legs, which are the
  only ones that compile this backend at all.
- **The backend is compiled only on Windows** (`#ifdef _WIN32`, the parent's file layout:
  `src/io_backend_iocp.cpp`), so `clang-tidy`, GCC, Clang, TSan and Coverage never see it.
  cppcheck does: the `cppcheck` target walks every preprocessor configuration of each
  `src/*.cpp`, `_WIN32` included, so it analyzes this file on Linux too, though without the
  Windows headers' declarations. MSVC `/W4 /WX` and MSVC ASan are its only gates that
  compile it.

## Approaches

**Who waits on the completion port** was the one structural choice. Chosen by the
maintainer: **option 1.**

1. **A thread the backend owns**, started when the backend is built and joined when it is
   destroyed. Simple and self-contained. It departs from the parent's "the thread pool is
   the caller's, keeping the component out of the thread-lifetime business", but the
   thread is private to the backend, starts and stops with it, and runs nothing but
   completion dispatch. **Chosen.**
2. **A permanent job on the borrowed `thread_pool`.** No new thread, but it takes one of
   the caller's workers for good, so a one-thread pool would starve open and stat, and
   `create` would need a "at least two threads" rule. Rejected.
3. **Win32's thread pool (`CreateThreadpoolIo`).** Windows owns the threads, but it needs
   a per-handle `TP_IO` object that must be closed with the handle, which is exactly the
   handle-lifetime problem #53 steered around, because the backend never sees a close.
   Rejected.

## The design

### Shape of the backend

`iocp_backend` implements `io_backend` and reports `io_backend_kind::iocp`. It holds:

- the completion port, created with `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)`;
- its completion thread (option 1);
- an internal thread-pool backend over the borrowed pool (`make_thread_pool_backend`), to
  which it forwards every operation except `open`, `read` and `write`;
- an **in-flight registry**: the transfers submitted to the kernel and not yet completed,
  keyed by `io_token`, under a mutex (see Cancellation).

Its factory is **not public**: only `file_io::create(thread_pool&, ...)` builds one. A
consumer cannot inject it through the `io_backend&` overload, which has no thread pool to
route adopted files to (see below). The factory is declared in a header the tests can
reach but consumers have no reason to include (builder's call where it lives).

### Open

`open` runs on a pool worker, as everywhere, through the same `do_open` the thread-pool
arm uses, with two differences:

1. `FILE_FLAG_OVERLAPPED` is added to the `CreateFileW` flags;
2. the new handle is associated with the port (`CreateIoCompletionPort(handle, port,
   ...)`) and set to `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` (see Transfers), before the
   result is reported. If either call fails, the handle is closed and the open fails with
   that error: a handle this backend returned is always fully prepared.

So the shared `do_open` gains a way to take extra flags and a post-open step; how
(parameter, callback, a small shared internal header in `src/`) is the builder's call.

### Transfers

Each `read`/`write` gets a heap **transfer record** owned by the backend from submission
until its completion is consumed: the `OVERLAPPED` (first member, so the packet's
`OVERLAPPED*` recovers the record), the request's `io_buffer` copy (which keeps the
memory alive), `on_done`, `state` and the token.

Submission:

1. Register the record in the in-flight registry.
2. Call `ReadFile`/`WriteFile` with the record's `OVERLAPPED` (offset in
   `Offset`/`OffsetHigh`, `hEvent` null).
3. Then, by outcome:
   - **Success** (completed synchronously). With `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`
     set, no packet will come, so the backend completes it **inline, inside `submit`**,
     which the seam allows. Cached reads complete this way often, and it saves a thread
     hop.
   - **`ERROR_IO_PENDING`.** The packet will come; the completion thread finishes it.
   - **Any other failure.** No packet will come, so it completes inline with the error.
     `ERROR_HANDLE_EOF` on a read is a zero-byte success, as on the pool arm.

"Completes" always means the same three steps, in order: remove the record from the
registry under its mutex; call `on_done` with the result; free the record.

The completion thread loops on `GetQueuedCompletionStatusEx` (batching several packets
per wake is the builder's call). For each packet it **looks the `OVERLAPPED` address up in
the registry, under its mutex**, and completes the record found, provided its
`OVERLAPPED` has completed (`HasOverlappedIoCompleted`). A packet whose address is not
registered is not ours and is ignored: never cast, never completed, never freed (see "The
handle a consumer sees"). So is a packet naming a registered record still pending: a
consumer's packet whose freed `OVERLAPPED` address a newer record now occupies. The kernel
writes the final status before queuing a packet, so a record's own packet always finds it
complete. The registry is therefore indexed both by token, for
`cancel`, and by record address, for completion; the shape is the builder's call. A packet whose status is `ERROR_HANDLE_EOF` is a zero-byte success; one whose status
is `ERROR_OPERATION_ABORTED` after a cancel is reported as `std::errc::operation_canceled`;
any other failure is reported through the same Windows error normalization as the pool
arm (`ERROR_DIRECTORY` → `not_a_directory`, and the `is_a_directory` check for a transfer
on a directory handle).

**Append.** A handle opened with `open_flag::append` has `FILE_APPEND_DATA` access
without `FILE_WRITE_DATA`. The documented append-under-overlapped form is an offset of
`0xFFFFFFFF`/`0xFFFFFFFF`. Whether an ordinary offset on an append-only handle appends
anyway is **not established here**, so the builder must test it on real hardware and
take the documented form if not. Whatever is chosen, a test must pin that an append
write lands at the end of the file.

### The handle a consumer sees

Decided by the maintainer after the builder check. On an IOCP `file_io`, a file from
`file_io::open` has a handle opened with `FILE_FLAG_OVERLAPPED` and associated with the
backend's private port, and `async_file::handle()` returns exactly that handle. Two
consequences, both handled:

- **A synchronous call on it is no longer valid.** A plain `ReadFile`/`WriteFile` with no
  `OVERLAPPED`, typically inside `file_io::run()`, is not allowed on an overlapped handle.
  This is **documented** on `handle()`, on `adopt()` (whose "Handles opened by
  `file_io::open` satisfy all of this" must say that, on Windows, such a handle is
  overlapped and port-associated), and in `docs/async_file.md`. A consumer who needs the
  handle for its own transfers must use overlapped calls, and should set the low-order
  bit of `hEvent` so no packet reaches alt's port. This is a behaviour change for any
  Windows user of `handle()` once `automatic` selects IOCP; no such user is known.
- **A consumer's own overlapped call cannot corrupt alt.** If it posts a packet to our
  port anyway, the completion thread finds no record at that `OVERLAPPED` address, or only
  a record still pending there, and ignores it (see Transfers). The consumer's own wait on its event is unaffected.

The alternative of keeping a second, ordinary handle per file for `handle()` to return
(`ReOpenFile`) was considered and rejected: it doubles the handles per file and grows
`io_handle`, for a use no consumer is known to make.

### Cancellation

`cancel(token)` takes the registry's mutex, and **if** the token is still registered,
calls `CancelIoEx(handle, &record->overlapped)` while holding it. Completion removes the
record under the same mutex before freeing it. So a cancel either finds a record that is
still owned by the kernel, and cancels exactly that request, or finds nothing and does
nothing. That serialization is what rules out the new anti-goal.

`CancelIoEx` is a hint, as the seam says. The packet still arrives, with
`ERROR_OPERATION_ABORTED` or with the real result if the transfer finished first, and the
callback runs exactly once with whichever it is. `cancel` never blocks.

### What runs on the completion thread

Every IOCP transfer's `on_done` runs on the one completion thread, and
`io_operation::on_backend_complete` drops the block's backend reference there. For a
cancelled operation, or a file whose `async_file` is already gone, that is often the last
reference, so the block's teardown runs on that thread: the `io_buffer` release and,
through `io_handle`, `CloseHandle`. A close that blocks (a network filesystem) therefore
delays **every** other transfer's completion on that `file_io` until it returns. The pool
arm has the same property, but spread across its workers.

Decided by the maintainer after the builder check: **accepted and documented**, not
engineered around. Local files close quickly, and `co_await file.close()` lets a caller
who cares choose where the close happens. It is stated on the backend's documentation in
`docs/io_backend.md` and in `docs/file_io.md`'s description of the IOCP backend. Moving
final releases off the thread was rejected because it would reopen the parent design's
lifetime protocol for a cost no consumer is known to pay.

### Shutdown

`~file_io` already waits until no operation is outstanding at the backend, so by the
time the IOCP backend is destroyed, its registry is empty and no packet is owed. Its
destructor then posts a wake-up packet (`PostQueuedCompletionStatus` with a reserved
key) that the completion thread treats as "exit", joins the thread, closes the port, and
destroys the internal pool backend. A non-empty registry at destruction is a
precondition violation and asserts.

### Selection

The probe in `src/file_io.cpp` gains its Windows branch:

- **`automatic`** on Windows tries to build the IOCP backend. If that fails (port
  creation or thread start), it falls back to the thread pool, and `file_io::backend()`
  reports `thread_pool`, which is how the parent design already makes a fallback visible.
- **`iocp`** on Windows builds it or fails `create` with the error.
- **`iocp`** elsewhere stays `not_supported`.

This changes the default for an existing Windows `file_io` from `thread_pool` to `iocp`.
That is the point of the phase, and no known consumer runs on Windows.

### Adopted files: issue #53 implemented

In the file layer, as decided:

- `io_shared` gains a second, internal backend pointer, the **adopted-file backend**. For
  an IOCP `file_io` it points at a thread-pool backend that `file_io` also owns; for every
  other backend it is the same pointer as the primary, so nothing changes there.
- `io_handle` records whether the handle was adopted (`async_file::adopt`) or produced by
  `file_io::open`. It is immutable after construction.
- When an operation block submits, it picks its backend from those two facts, and keeps
  the pointer, so its `cancel` goes to the same backend its `submit` did. Quiescence
  (`enter_backend`/`leave_backend`) is already counted in `io_shared` across all
  submissions, so `~file_io` waits for both backends with no change.

**The one public API addition:** `[[nodiscard]] io_backend_kind async_file::backend() const
noexcept`, the backend that serves this file's operations. For an adopted file on an IOCP
`file_io` it reports `thread_pool`; otherwise it equals `file_io::backend()`. It is
documented on the member, on `adopt()`, and in `docs/async_file.md`. An empty or
moved-from `async_file` reports what its service reported, or `thread_pool` if it has none
(builder's call, stated in the Doxygen).

### Version

A minor bump: a new backend, `async_file::backend()`, and grown internal types
(`io_shared`, `io_handle`, the operation block) compiled into `libalt`, which by the
parent's own precedent is a minor in this library. The exact number depends on what has
merged before 7c (PR #69 takes 1.16.0).

## Testing

**The whole file-layer suite runs against IOCP.** The `FileIo*` and `AsyncFile*` tests
are written against whatever `file_io::create(pool, ...)` returns. On Windows the suite is
registered with ctest a second time with an environment variable that makes the
fixtures request `iocp` explicitly; the first registration requests `thread_pool`, so both
backends run every test on the MSVC legs. Chosen over `TEST_P` or typed tests because
issue #68 is weighing source-scanned test discovery, which cannot see either (builder's
call how the fixtures read the variable).

**Backend-level tests**, `IocpBackendTest`, mirroring `ThreadPoolBackendTest`:

- read and write at offsets, short read at EOF, read past EOF, a transfer on a directory
  handle, a closed handle;
- **the pending path, forced**: the backend's own `open` on a `\\.\pipe\` path (the client
  end of a pipe the test serves), so a read is genuinely pending until the test writes.
  Regular files often complete synchronously and would leave the pending path untested;
- **the inline paths**: a synchronous success completes before `submit` returns, and an
  immediate failure too;
- **cancel while pending** (`operation_canceled`, callback exactly once), **cancel after
  completion** (a no-op that touches no freed memory), and **cancel racing completion**,
  repeated under `debug-asan`;
- **the wrong-operation hazard**: a cancel issued for a finished token while a new transfer
  on the same handle is pending must not cancel the new one;
- **append** lands at the end (see Transfers);
- **a foreign packet is ignored**: an overlapped transfer the test issues itself, without
  the low-bit tag, on a handle this backend opened, posts a packet the completion thread
  must ignore, while the backend's own transfers keep completing normally;
- **a stale packet at a pending record's address is ignored**: a packet posted naming a
  pending read's `OVERLAPPED` is counted as ignored, and the read later completes once with
  its real data;
- **pool transfers on a port-associated handle queue no packet**: issue a pool-arm
  transfer on a handle this backend opened, then check the port holds nothing for it.
  This pins the #52 low-bit dependency from the IOCP side;
- **shutdown**: destroying the backend with nothing in flight joins its thread and closes
  the port; with something in flight is a death test.

**Adopted routing**, on an IOCP `file_io`: an adopted synchronous handle and an adopted
overlapped handle both read and write correctly and report `thread_pool`; an opened file
reports `iocp`; cancelling an adopted file's operation goes to the pool backend (the
cancel-before-start test from `ThreadPoolBackendTest`, through the file layer).

**Selection:** `automatic` reports `iocp` on Windows; an explicit `iocp` request succeeds;
`iocp` on other platforms is still `not_supported`.

**Mutation checks**, per this project's practice, each built and measured, not inferred:

- free the record before the packet (or skip the registry removal): an ASan report or a
  failure in the pending tests;
- call `CancelIoEx` outside the registry lock: the wrong-operation test must fail. If it
  cannot be made to fail deterministically, say so in the test rather than claim it;
- drop `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` while still completing inline: a double
  completion the "callback exactly once" assertions must catch;
- route adopted files to the primary: the adopted overlapped-handle test must fail;
- cast every packet's `OVERLAPPED*` to a record without the registry lookup: the
  foreign-packet test must fail, under `debug-asan` with an ASan report.

## Calls made after the builder check

Settled by the designer, not the maintainer; listed so they can be checked:

- **Tokens are unique ids, not record addresses.** `io_backend.hpp`'s comment allows an
  address, but a reused address would defeat the wrong-operation guarantee, since a
  stale token could then name a new record.
- **Tokens carry a tag bit** that tells an IOCP transfer's token from one the internal
  pool backend issued, so `cancel` forwards a pool token to the pool backend.
- **A non-empty registry at destruction** asserts in debug builds, and in release builds
  terminates rather than freeing records the kernel may still own.
- **Existing tests that assume `automatic` means `thread_pool`** are updated to say which
  they mean.
- **The test-only way to inspect the port** (for the low-bit test) lives in the same
  internal header as the factory.

## What is left to the builder

- Where the shared Windows pieces live (`do_open`'s extra flags, error normalization)
  between `io_backend_thread_pool.cpp` and `io_backend_iocp.cpp`.
- Where the non-public factory is declared.
- Batch size for `GetQueuedCompletionStatusEx`, and the reserved shutdown key.
- How the fixtures pick a backend from the environment variable.
- What an empty `async_file`'s `backend()` reports, within the rule above.
- Documentation: `docs/io_backend.md` and `docs/file_io.md` gain the IOCP backend and the
  selection rules; `docs/async_file.md` gains `backend()` and the adopted-file routing; the
  parent design's staging table notes the one public addition.

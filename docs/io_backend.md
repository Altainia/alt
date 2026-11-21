# io_backend

**Header:** `<alt/io_backend.hpp>`

The seam between the awaitable file layer ([`alt::file_io`](file_io.md)) and a
platform's I/O engine. It is a small virtual interface: submit one operation, receive
one callback, and cancel as a hint. A backend implements it; application code rarely
touches it, except to inject a `manual_io_backend` in a test.

```cpp
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>

void drive_by_hand(alt::scheduler_ref loop)
{
    alt::manual_io_backend backend;
    auto io = alt::file_io::create(loop, backend);   // the overload that is handed a backend; it takes no pool

    // ... start an operation on *io, then decide the order things happen in:
    backend.cancel(alt::io_token{1});                      // the first operation's token
    backend.complete(0, alt::io_completion{.bytes = 5, .error = {}});   // the callback runs here, on this thread
}
```

Every platform has the thread-pool backend, which `file_io::backend()` reports as
`io_backend_kind::thread_pool`. Windows also has the [IOCP backend](#the-iocp-backend-windows),
which reports `io_backend_kind::iocp`. The enumerator `io_uring` names a backend this
release does not provide.

## Interface

```cpp
namespace alt
{
    using native_handle = std::intptr_t;
    inline constexpr native_handle invalid_native_handle = -1;

    enum class io_backend_kind : std::uint8_t { automatic, thread_pool, io_uring, iocp, manual };

    enum class io_op : std::uint8_t
    {
        read, write, sync_full, sync_data, stat_handle, stat_path,
        open, rename, unlink, list_directory, invoke,
    };

    struct io_request
    {
        io_op                 op     = io_op::read;
        native_handle         handle = invalid_native_handle;
        io_buffer             buffer;
        std::uint64_t         offset = 0;
        std::size_t           size   = 0;
        const void*           in     = nullptr;
        void*                 out    = nullptr;
        std::shared_ptr<void> state;
    };

    struct io_completion
    {
        std::size_t     bytes = 0;
        std::error_code error;
    };

    using io_completion_fn = void (*)(void* state, io_completion result) noexcept;

    class io_token;   // opaque 64-bit id; zero is "empty"; converts to false when empty

    class io_backend
    {
    public:
        virtual ~io_backend() = default;

        [[nodiscard]] virtual std::expected<io_token, std::error_code>
            submit(const io_request& request, io_completion_fn on_done, void* state) = 0;
        virtual void cancel(io_token token) noexcept = 0;
        [[nodiscard]] virtual io_backend_kind kind() const noexcept = 0;
    };

    class manual_io_backend final : public io_backend { /* see below */ };

    [[nodiscard]] std::unique_ptr<io_backend> make_thread_pool_backend(thread_pool& pool);
}
```

## Why the contract is this narrow

The interface is deliberately **not** widened per platform. A submission-queue backend
and a completion-port backend both fit in "submit one operation, receive one callback,
cancel is a hint". If a backend needs more than that, the design is wrong, not the
backend. `native_handle` is a signed pointer-sized integer for the same reason: it holds
an `int` descriptor and a `HANDLE` losslessly, so the header is includable everywhere
without `unistd.h` or `windows.h`.

## The callback

- It runs **on an I/O thread, and may run inline inside `submit()`**, before it returns.
  This layer never posts to a scheduler: the layer above, which owns one, does that. A
  caller that resumes a coroutine straight from the callback resumes it on the wrong
  thread. A caller holding a lock across `submit()` finds an inline callback running
  under it.
- It runs **exactly once for every submission that returns a token**. A submission that
  returns an error has not run it and never will. The layer above relies on both halves:
  the first so it never resumes a frame twice, the second so it never leaks one waiting
  on a callback that is not coming.
- It is a plain function pointer plus a state pointer, not a `std::function`, because it
  is invoked from an I/O thread and must not allocate or throw. `noexcept` is part of
  the type, so an escaping exception terminates.

## `cancel` is a hint

The operation's callback still runs exactly once, with whatever outcome the kernel
settled on, which may be success. io_uring and IOCP both complete after a cancel
request rather than at it, so **a cancel never makes a buffer safe to free; only the
callback does.** This is the contract, not a limitation. See
[file_io](file_io.md#cancellation-detaches-it-does-not-abort) for what it means to the
caller. `cancel` never blocks, and cancelling an empty, unknown or already completed
token is a no-op.

## The request is a description, copied by value

A backend **must copy whatever it needs before `submit()` returns**: the reference
passed in is not kept alive for the operation, and a backend that stores the pointer
instead of the contents reads a dead stack frame. The `buffer` member is the exception
that proves the rule, because copying it is exactly how a backend keeps the storage
alive. A backend keeps its copy until the completion callback has *returned*, not merely
until it starts.

`in` and `out` carry what the struct does not model: operation-specific input (a path
pair for `rename`), and storage for a result that is not a byte count (a `file_status`
for the stat operations, a `std::vector<directory_entry>` for `list_directory`, a
`native_handle` for `open`). **Both must point into storage the operation block
co-owns, never into a coroutine frame**, because a completion can land after the frame
is gone and a write into a destroyed frame is a use-after-free no sanitizer reliably
reports. `request.state` is how a layer hands that storage to the block. The seam cannot
check this; it is a precondition on whatever builds requests.

Which fields each operation reads, and where its result goes, is the table on
`io_request` in the header. Two operations deserve a note here:

- `list_directory` and `invoke` have no kernel equivalent on any platform, so **every
  backend runs them on a helper thread**. That is an implementation burden on the
  backend, not a gap in the interface.
- **An `invoke` job carries an obligation the others do not.** Its `out` is an
  `alt::detail::io_invocable*` (declared in `<alt/detail/io_operation.hpp>`), and the backend must call `invoke()` when the job runs, or
  `discard()` *instead* for a job cancelled before it started, and complete that job with
  `std::errc::operation_canceled`. `discard()` is what destroys the callable and its
  captures on the helper thread. A backend that skips a cancelled job leaves the
  callable to die on whichever thread drops the block's last reference, which silently
  loses the guarantee [`file_io::run()`](file_io.md#run-the-escape-hatch) documents.

A read or write moves at most `min(size, buffer.size())` bytes. A larger `size` is
clamped and the short transfer is reported as success, indistinguishable from a real
short read, so a layer that must refuse that request has to do so before submitting.
[`async_file`](async_file.md) does.

## `manual_io_backend`

A backend a test drives by hand. Nothing completes until the test says so, which turns
every ordering into something the test *writes* rather than hopes for: cancel, then
complete, then assert. The hardest property of the file layer is that a cancelled chain
must not resume while a kernel is still writing into its buffer, and it can only be
proven by ordering a cancel before a completion on purpose.

- Operations are addressed by submission index: the first accepted submission is 0, the
  next 1, and an index is never reused or shifted. A rejected submission takes no index.
  Its token's value is the index plus one.
- `complete(index, result)` runs the callback on the calling thread, outside the internal
  lock, and delivers `result` as is, cancelled or not. Completing an index twice, or one
  never submitted, is a test bug and asserts.
- `complete_inline_next(result)` arms the *next* `submit()` to complete before it
  returns, which models the inline path. `fail_next_submission(error)` arms it to be
  rejected, and the callback of a rejected submission never runs. Both are one-shot, and
  when both are armed the failure wins and consumes both.
- `cancel` records the request and never completes the operation; `was_cancelled(index)`
  reads it back, completed or not.
- Completing an operation releases the backend's copy of its buffer, so
  `request(index).buffer` is empty afterwards. A test that needs the buffer inside or
  after the callback takes its own copy *before* `complete()`.
- All members are safe to call concurrently. `request()` returns a reference valid for
  the backend's life, but the request it names is not synchronized with `complete()` of
  that same index.
- Operations still pending when the backend is destroyed never run their callbacks.

## `make_thread_pool_backend`

Performs every operation as a blocking system call on a pool thread. It is the one
backend every platform has, and it stays in use even where a faster engine exists:
`open`, `stat`, `rename`, `unlink` and `list_directory` have no asynchronous form on
Windows, and a submission queue has nowhere to run arbitrary code.

The pool is **borrowed, never owned**. The caller keeps it alive until every submitted
operation's callback has run, and must not destroy it from one of its own workers, which
would join itself. Destroying the returned backend while operations are queued is safe:
each job carries what it needs to finish.

Cancel has detach semantics. A job that has not started finds itself cancelled and
completes with `std::errc::operation_canceled` **without doing the I/O**. A job already
running runs to completion and reports its real result. Either way the callback runs
exactly once, on a pool thread, and the job's `io_buffer` copy is held until it returns
and then released on the worker.

## The IOCP backend (Windows)

Performs reads and writes as overlapped I/O completed through an I/O completion port, so a
transfer in flight holds no thread at all. It is not public: its factory lives in a header
under `src/`, and only `file_io::create(thread_pool&, ...)` builds one, because adopted
files need a thread pool to be routed to.

**What runs where.**

| Operation | Where it runs |
|-----------|---------------|
| `read`, `write` | Submitted as overlapped `ReadFile`/`WriteFile` from the calling thread; completed on the backend's completion thread, or inline inside `submit()` (below) |
| `open` | A pool worker, as on the thread-pool backend, with `FILE_FLAG_OVERLAPPED` added; the new handle is associated with the port and set to `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` before the open reports success. If either step fails the handle is closed and the open fails with that error |
| everything else | A pool worker: `stat`, `sync`, `rename`, `unlink` and `list_directory` have no overlapped form on Windows, and `invoke` has no kernel form anywhere |

The pool is borrowed, as for the thread-pool backend; the port and the completion thread
belong to the backend, which starts the thread when it is built and joins it when it is
destroyed. The thread runs nothing but completion dispatch.

**Transfers.** Each read or write gets a heap record holding its `OVERLAPPED`, its copy of
the request's `io_buffer` (which keeps the bytes alive while the kernel owns them), the
callback and the token. The record is registered in an in-flight registry, indexed by token
and by `OVERLAPPED` address under one mutex, *before* the `ReadFile`/`WriteFile` call. Then:

- **Completed synchronously.** `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` means no packet will
  come, so `submit()` completes it inline, before it returns. Named-pipe reads with data
  waiting complete this way. Cached file reads may too, but on the Windows 11 machine the
  backend was built on, every overlapped file read went pending, so do not count on it.
- **`ERROR_IO_PENDING`, or `ERROR_MORE_DATA`.** The packet comes later, and the completion
  thread finishes it. `ERROR_MORE_DATA` (a message-mode pipe read shorter than its message)
  is a warning-class status, which still queues a packet, so it is never completed inline.
  It is reported as the thread-pool backend reports it: that error, with 0 bytes.
- **Any other failure.** No packet is queued for an immediate failure, so it completes
  inline with the error.

"Completes" is always the same three steps, in order: remove the record from the registry,
call the callback, free the record (and its buffer copy with it). `ERROR_HANDLE_EOF` on a
read is a zero-byte success, wherever it surfaces; other errors are normalized as on the
thread-pool backend, including `is_a_directory` for a transfer on a directory handle.

**Packets that are not the backend's.** The completion thread looks each packet's
`OVERLAPPED` address up in the registry and completes only a record it finds there whose
`OVERLAPPED` the kernel has finished with. A packet whose address is not registered, such as
one from a consumer's own untagged overlapped call on a handle this backend opened, is
ignored: never cast to a record, never completed, never freed. So is one naming a registered
record that is still pending: an address alone does not say which call a packet is for, and
a consumer's packet whose heap `OVERLAPPED` was freed before the packet was dequeued can name
the address of a newer transfer of the backend's. The kernel writes a request's final status
into its `OVERLAPPED` before it queues the packet, so a record's own packet always finds it
complete, and a pending record's own packet is still to come. A consumer who uses such a handle directly should set the low-order bit of `hEvent`,
Windows' documented way of keeping a completion off the handle's port; the thread-pool
backend's own transfers already do, so a pool transfer on one of these handles posts nothing.

**Transfers need a handle this backend opened.** A handle must be opened with
`FILE_FLAG_OVERLAPPED` and associated with the port for its completion to arrive, and a
handle can be associated with only one port, for life. A transfer on any other overlapped
handle would wait for a packet that never comes, which is why `file_io` routes adopted files
to a thread-pool backend.

**Tokens.** A token for an IOCP transfer has bit 63 set; every other token came from the
internal thread-pool backend, and `cancel` forwards it there. Cancelling an IOCP transfer is,
for now, a hint the backend does not act on: the transfer completes once with its real
result, which the contract allows. Tokens are ids, never record
addresses, so a stale token cannot name a newer record at a reused address.

### What runs on the completion thread

Every IOCP transfer's callback runs on the one completion thread, and the file layer drops
an operation's backend reference there. For a cancelled operation, or a file whose
`async_file` is already gone, that is often the last reference, so the operation's teardown
runs on that thread too: the `io_buffer` release and, through the handle's owner,
`CloseHandle`. **A close that blocks, as one on a network filesystem can, therefore delays
every other transfer's completion on that `file_io` until it returns.** The thread-pool
backend has the same property, spread across its workers. This is accepted rather than
engineered around: local files close quickly, and `co_await file.close()` lets a caller who
cares choose where the close happens.

### Shutdown

The destructor posts a packet with a reserved completion key, which the completion thread
treats as "exit", joins the thread, closes the port, and then destroys the internal
thread-pool backend. Destroying it with **any** operation still in flight is a precondition
violation: a transfer, or an operation forwarded to the thread pool whose callback has not
yet run. `file_io` never does, because it waits for every outstanding operation first. It
asserts in debug builds and calls `std::terminate()` under `NDEBUG`, rather than free a
record the kernel may still write into, or let a queued `open` associate its new handle with
a port that is already closed (whose handle value may by then name another port). This is
unlike the thread-pool backend, which may be destroyed with operations queued.

Forwarded operations are counted through a small trampoline around the caller's callback.
It lowers the count *before* calling the caller's callback, and that is its last access to
the backend: by then the operation is done with the backend, and `file_io`'s callback may
end by letting `~file_io` destroy the backend at once.
# io_backend

**Header:** `<alt/io_backend.hpp>`

The seam between the awaitable file layer ([`alt::file_io`](file_io.md)) and a
platform's I/O engine. It is a small virtual interface: submit one operation, receive
one callback, and cancel as a hint. A backend implements it; application code rarely
touches it, except to inject a `manual_io_backend` in a test.

```cpp
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>

void drive_by_hand(alt::thread_pool& pool, alt::scheduler_ref loop)
{
    alt::manual_io_backend backend;
    auto io = alt::file_io::create(pool, loop, backend);   // the overload that is handed a backend

    // ... start an operation on *io, then decide the order things happen in:
    backend.cancel(alt::io_token{1});                      // the first operation's token
    backend.complete(0, alt::io_completion{.bytes = 5, .error = {}});   // the callback runs here, on this thread
}
```

This release ships one real backend, the thread-pool one, which `file_io::backend()`
reports as `io_backend_kind::thread_pool`. The enumerators `io_uring` and `iocp` name
backends this release does not provide.

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

# async_file

**Header:** `<alt/async_file.hpp>`

An open file whose operations are awaitable and positional. It comes from
[`file_io::open()`](file_io.md), or is adopted from a descriptor opened some other way.
Every operation returns `alt::task<std::expected<T, std::error_code>>`, so cancellation
checking, backpressure and composition come from the machinery tasks already have, at
the price of one coroutine frame allocation per operation.

```cpp
#include <alt/async_file.hpp>
#include <alt/file_io.hpp>

alt::task<void> save(alt::file_io& io, alt::io_buffer data)
{
    auto file = co_await io.open("out.bin", alt::open_flags{alt::open_flag::write} | alt::open_flags{alt::open_flag::create});
    if(!file)
    {
        co_return;
    }

    const auto written = co_await file->write_at(data, 0, data.size());
    if(written)
    {
        const auto synced = co_await file->sync(alt::sync_mode::data);   // [[nodiscard]]: check it
    }

    const auto closed = co_await file->close();
}
```

## Interface

```cpp
namespace alt
{
    using open_flags = flags<open_flag>;

    enum class sync_mode : std::uint8_t { full, data };

    class async_file
    {
    public:
        [[nodiscard]] static async_file adopt(const file_io& io, native_handle handle);

        async_file(async_file&&) noexcept;            // the source is left closed
        async_file& operator=(async_file&&) noexcept;
        ~async_file();                                // not copyable

        [[nodiscard]] task<std::expected<std::size_t, std::error_code>> read_at(io_buffer buffer, std::uint64_t offset, std::size_t size) const;
        [[nodiscard]] task<std::expected<std::size_t, std::error_code>> write_at(io_buffer buffer, std::uint64_t offset, std::size_t size) const;
        [[nodiscard]] task<std::expected<void, std::error_code>>        sync(sync_mode mode = sync_mode::full) const;
        [[nodiscard]] task<std::expected<file_status, std::error_code>> stat() const;
        [[nodiscard]] task<std::expected<void, std::error_code>>        close();
        [[nodiscard]] native_handle handle() const noexcept;
    };
}
```

`open_flag`, `file_mode`, `file_status`, `file_kind`, `file_identity` and
`directory_entry` are declared in `<alt/detail/file_types.hpp>`, which this header
includes. `open_flags` is `alt::flags<open_flag>`, whose construction from an enumerator
is explicit: `alt::open_flags{open_flag::read} | alt::open_flags{open_flag::create}`.

## Positional by design

There is no seek and no current position. Every transfer names its offset, so two
`read_at`/`write_at` calls on one file share no state and may be outstanding together.
That is what makes concurrent use supported rather than merely tolerated. A file opened
with `open_flag::append` ignores the offset of a write.

## What an operation owns

Each task a member returns holds its own references to the descriptor and to the
service state, taken **when the member is called**, not when the task first runs. So the
`async_file` may be moved, closed or destroyed while a task made from it is suspended, or
has never started, and the task still works. The descriptor is closed by whichever party
drops the last reference, which for a cancelled operation can be a worker thread long
after the awaiting frame is gone. That is what stops a cancelled read from reading
through a closed descriptor number the operating system has since given to another file.

The same holds for the buffer. `read_at` and `write_at` take an [`io_buffer`](io_buffer.md)
by value; the operation keeps its copy, so the memory stays valid for the kernel even if
the task is cancelled first.

## Errors and their order

`read_at` and `write_at` check, in this order and all before anything is submitted:

1. `std::errc::bad_file_descriptor`: the file was closed (even through a copy of the
   descriptor an operation was already holding), was moved from, or adopted an invalid
   handle. It is reported here rather than submitted because the descriptor number may
   have been reused by then.
2. `std::errc::invalid_argument`: `size` exceeds `buffer.size()`. It is **rejected, not
   clamped**. The backend does clamp, as a second line of defense, but a clamp reports a
   short read that a caller cannot tell from a real one.

A `size` of 0 completes with 0 bytes immediately and submits nothing. A read is shorter
than `size` at the end of the file and returns 0 at or past it. Anything else is the
backend's error, such as `std::errc::is_a_directory`.

The checks run before the operation takes a backpressure permit, so the rejecting paths
hold no permit and have nothing to release.

`std::errc::operation_canceled` is part of the result type's contract but is never
delivered to a caller in the current design: a cancelled chain is abandoned rather than
resumed. It is retained so that an error `switch` stays total.

## `sync` and `sync_mode`

`full` is `fsync` or `FlushFileBuffers`. `data` is `fdatasync` on Linux, `fsync` on every
other POSIX system, and `FlushFileBuffers` on Windows. **`data` degrades to the stronger
guarantee, never the weaker**: a caller who asked for it gets at least what it promises
on every platform and merely pays more where the platform cannot do better. Silently
downgrading to something that might lose the data would be a durability bug.

## `stat`

Asks the descriptor rather than the path, so it is correct for a file renamed or
unlinked since it was opened. `file_identity` is the same for every descriptor of one
file, which lets a caller build an ETag from inode, size and modification time.

## Closing

**`close()` takes effect when it is called, not when it is awaited.** From that instant
every operation that has not yet submitted, including tasks made earlier and not yet
started, reports `bad_file_descriptor`, and `handle()` reports `invalid_native_handle`.
The descriptor itself closes when the last reference goes: the one `close()` took over,
and those of any operation still in flight. An operation already at the backend finishes
against a live descriptor, and a descriptor number is never closed under one. Closing
twice, or closing a moved-from file, succeeds and does nothing.

The operating-system close runs wherever the last reference drops: typically inside the
returned task, on the thread awaiting it, so it can block that thread on a network
filesystem. The destructor closes inline only if it held the last reference; with an
operation outstanding it closes nothing and the last operation to finish does.

**Neither path can report the error `close(2)` may produce** in this release, because the
descriptor's destructor discards it. The result type is an `expected` so a backend with an
asynchronous close can report it later without changing the signature.

## `adopt`

Takes ownership of a descriptor opened some other way, for a consumer whose open is not
expressible through `open_flag`. The caller must not close it afterwards or adopt the
same one twice: two owners mean a double close, which can close a recycled descriptor.
`invalid_native_handle` is allowed and yields a file on which every operation reports
`bad_file_descriptor`.

Any other handle must be one on which a transfer **blocks until it is done**, which is
the caller's responsibility to arrange:

- A Windows `HANDLE` must **not** have been opened with `FILE_FLAG_OVERLAPPED`. The
  transfer arm passes an `OVERLAPPED` only to carry the offset and treats
  `ERROR_IO_PENDING` as a failure, after which the operation completes and releases its
  buffer while the kernel may still be writing into it.
- A POSIX descriptor must **not** be in `O_NONBLOCK` mode, as a FIFO or character device
  opened that way would be: every `read_at` on it fails with `EAGAIN`.

Handles opened by `file_io::open` satisfy both.

## What a cancelled operation can still do

Cancelling detaches, it does not abort; see
[file_io](file_io.md#cancellation-detaches-it-does-not-abort). A cancelled `write_at`
may still land, and a cancelled `read_at` may still fill its buffer. For a cancelled
`file_io::run()` callable, the guarantee that it is destroyed on a worker holds **only
after submission**. In the pre-submission window, while the operation is parked on an
in-flight slot or after the backend refused the submission, the callable is still
unrun but is destroyed on the thread that cancels the chain, which can be the completion
thread.

## Thread safety

Operations may be initiated from any thread and run concurrently. The `async_file` object
itself is not synchronized: it may move between threads but is not thread-safe, and
`close()` racing an outstanding operation on the same file is a precondition violation.
It is a logic error rather than a memory error, because the descriptor stays valid for
as long as any operation holds it.

The service ([`file_io`](file_io.md)) must outlive every operation started on a file it
produced.

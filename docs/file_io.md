# file_io

**Header:** `<alt/file_io.hpp>`

The service object for awaitable file I/O. It owns the backend choice, the backpressure
limit and the scheduler every operation's completion is posted to, and the operations
that need no open file hang off it: `open`, `stat`, `rename`, `unlink`,
`list_directory`, and the escape hatch `run`. Operations on an open file are on
[`async_file`](async_file.md).

```cpp
#include <alt/file_io.hpp>
#include <alt/async_file.hpp>

alt::task<void> copy_header(alt::file_io& io)
{
    using alt::open_flag;

    auto file = co_await io.open("data.bin", alt::open_flags{open_flag::read});
    if(!file)
    {
        co_return;      // file.error() is the operating system's error
    }

    auto buffer = alt::io_buffer::allocate(512);
    if(!buffer)
    {
        co_return;
    }

    const auto n = co_await file->read_at(*buffer, 0, buffer->size());
    // ...
}

// Once, on the thread that owns the loop:
void start(alt::thread_pool& pool, alt::scheduler_ref loop)
{
    auto service = alt::file_io::create(pool, loop);   // std::expected<file_io, std::error_code>
    if(!service)
    {
        return;
    }
    alt::file_io& io = *service;   // address-stable: built in place inside the expected
    static_cast<void>(io);
}
```

`pool` is an [`alt::thread_pool`](thread_pool.md), borrowed, and `loop` is the
[`alt::scheduler_ref`](scheduler.md) every awaiting coroutine is resumed on.

## Interface

```cpp
namespace alt
{
    struct file_io_options
    {
        io_backend_kind backend       = io_backend_kind::automatic;
        std::size_t     max_in_flight = 64;
        std::size_t     queue_depth   = 0;   // unused by the thread-pool backend
    };

    class file_io
    {
    public:
        [[nodiscard]] static std::expected<file_io, std::error_code>
            create(thread_pool& workers, scheduler_ref completion, const file_io_options& options = {});
        [[nodiscard]] static std::expected<file_io, std::error_code>
            create(scheduler_ref completion, io_backend& backend, const file_io_options& options = {});

        file_io(file_io&&) = delete;     // immovable; so are the copies
        ~file_io();                      // blocks until the backend is quiet

        [[nodiscard]] io_backend_kind backend() const noexcept;

        [[nodiscard]] task<std::expected<async_file, std::error_code>>
            open(const std::filesystem::path& path, open_flags how, file_mode mode = {}) const;
        [[nodiscard]] task<std::expected<file_status, std::error_code>>
            stat(const std::filesystem::path& path) const;
        [[nodiscard]] task<std::expected<void, std::error_code>>
            rename(const std::filesystem::path& from, const std::filesystem::path& to) const;
        [[nodiscard]] task<std::expected<void, std::error_code>>
            unlink(const std::filesystem::path& path) const;
        [[nodiscard]] task<std::expected<std::vector<directory_entry>, std::error_code>>
            list_directory(const std::filesystem::path& path) const;

        template<std::invocable F>
            requires std::is_nothrow_move_constructible_v<F>
                  && (!std::is_reference_v<std::invoke_result_t<F>>)
                  && (!std::is_array_v<std::invoke_result_t<F>>)
        [[nodiscard]] task<std::expected<std::invoke_result_t<F>, std::error_code>> run(F f) const;
    };
}
```

## Creating one

`create` fails rather than constructing something that would fail later on an I/O
thread:

- `std::errc::invalid_argument` for a zero `max_in_flight` (a zero bound would park the
  first operation on a permit that can never exist), for an empty `completion` (posting
  to an empty `scheduler_ref` asserts on whichever I/O thread finishes first, far from
  the mistake), and for `io_backend_kind::manual`, which cannot be selected by value;
- `std::errc::not_supported` for a kind this build does not provide;
- `std::errc::not_enough_memory` if constructing the service throws `std::bad_alloc`.

`automatic` asks the factory to pick. In this release that is always
`io_backend_kind::thread_pool`, and `backend()` reports what was chosen; it never reports
`automatic`. The overload that is handed an [`io_backend`](io_backend.md) is how a test
drives the service with a `manual_io_backend`: `options.backend` must then be `automatic`
or equal to that backend's `kind()`, and a mismatch is `invalid_argument` rather than a
silent substitution.

The type is immovable, for the reason `alt::thread_pool` gives: references to it live
inside suspended coroutine frames. Bind a reference into the returned `std::expected`,
whose address is stable because the object was built in place.

**`file_io` is a handle, not the state.** What an in-flight operation still touches when
its resumption runs (the backpressure permits) lives in a refcounted block that every
operation co-owns. That is what lets the destructor return while resumptions are still
queued. It does not relax the first precondition below: the service must still outlive
every operation started on it.

## Cancellation detaches; it does not abort

**A cancelled operation is not stopped. The awaiting chain is detached from it.** An
operation may still read, still write, still rename, after the chain that asked for it
is gone. Cancellation ends your wait; it does not promise to end the operation, and a
write that had reached the kernel may still land.

This is not a compromise reluctantly accepted. io_uring and IOCP both deliver
completions *after* a cancel request rather than at it, so a cancel can only ever be a
hint, and detaching is the only portable truth. What each backend does with the hint:

- **Thread pool** (the one this release ships): a job not yet started finds itself
  cancelled and returns without doing the I/O. A running job runs to completion and
  its result is discarded.
- A submission-queue or completion-port backend would issue its native cancel request as
  a hint and still consume the completion that follows. The [`io_backend`](io_backend.md)
  contract is written so those backends need nothing more than the thread-pool one does.

It already matches the contract of the consumer this was designed for: cpwf's README says
of a cancelled upload handler that work it started on another thread is not stopped. The
library's semantics and the consumer's documentation agree without either changing.

What does *not* happen is a use-after-free. The operation block co-owns the buffer, the
paths, the result slots and the descriptor, so a late completion writes into live memory
and the awaiting frame is never resumed. A cancelled `open` that completes after its
frame is gone does not leak the descriptor: the block closes it if nobody takes it.

## Shutdown: two preconditions, and what the destructor waits for

`~file_io` **waits for the backend, not for the resumptions.** It stops admitting
submissions and blocks until no operation is outstanding at the backend. When it
returns, no backend callback will fire again and the kernel holds none of this object's
memory. Resumptions already posted may still be queued; they release their permit
against refcounted state that outlives the handle.

The reason is that every completion ends in a post to the caller's loop, and for a
single-threaded event loop that thread is the one destroying this object. Waiting for
those posts to run would deadlock on the spot, with nothing pumping the loop.

Two preconditions, stated rather than enforced, both with precedent in
`alt::scheduler_ref` and `alt::thread_pool`:

1. **The object must outlive every operation started on it.** An operation still parked
   on a backpressure permit at destruction is a violation, and asserts in builds with
   asserts enabled.
2. **The destructor must not run on a thread of the pool the backend runs on**: the
   borrowed `thread_pool` for the built-in overload, or whichever pool an injected
   backend was built with, if any. It would wait for jobs queued behind itself. That is
   why the built-in overload borrows its pool rather than owning it: a precondition can be stated, an owned pool's last reference dying on its own
   worker cannot be.

There is deliberately no separate stop-accepting or drain API. One documented obligation
beats a second shutdown protocol with different ergonomics from `thread_pool`'s.

## Backpressure, and slot handoff through the home scheduler

At most `max_in_flight` operations are at the backend at once; further operations
suspend on an [`async_semaphore`](async_semaphore.md) until one finishes. A suspended
operation is a suspended coroutine, not a growing queue, and the wait is cancellable by
alt's ordinary machinery with no kernel involvement and so no buffer-lifetime question.

The slot is released on the completion scheduler, inside the posted completion, never
from the backend's callback: `async_semaphore::release()` resumes a parked waiter inline
on the releasing thread, so releasing from the callback would put the next operation's
submission on an I/O thread. Releasing on the home thread means **every submission
happens on a thread the completion scheduler runs on, never on an I/O thread**, and a
backend is never re-entered from its own completion thread. "Every" holds in practice
rather than absolutely: a chain's first submission happens on whichever thread awaits it.

The consequence is worth stating rather than discovering: **slot handoff is serialized
through the home scheduler. A loop that stops pumping stops issuing new I/O.** For a
backpressure limit that is the right failure mode, because the limit exists to couple
submission to the consumer's ability to keep up. Operations already at the backend
still finish; what stops is the next one leaving the queue.

## The path operations

All of them copy their paths into the operation, so a path need not outlive the call.

- `open(path, how, mode)`: `how` must name read, write or both, otherwise
  `std::errc::invalid_argument`. `mode` is octal (`file_mode{0644}`), used only with
  `open_flag::create`, and subject to the umask on POSIX. A missing path is
  `no_such_file_or_directory`; `open_flag::exclusive` over an existing one is
  `file_exists`.
- `stat(path)` follows symbolic links, so `file_status::kind` is never
  `file_kind::symlink` and a dangling link is an error.
- `rename(from, to)` replaces `to` if it is an existing file, on every platform, so it
  serves for "write a temporary, then move it into place". It is never a copy: across
  filesystems on POSIX it fails with `std::errc::cross_device_link`, and a failed rename
  leaves `from` where it was.
- `unlink(path)` removes a file or a symbolic link without following it. Not for
  directories.
- `list_directory(path)` reads the whole directory before the task completes, so a
  listing is all or an error, never partial. `.` and `..` are not reported and the order
  is unspecified. Each entry's kind describes the entry itself, so a symbolic link is
  `file_kind::symlink`; an entry whose kind could not be determined is
  `file_kind::unknown` rather than failing the listing. A file is
  `std::errc::not_a_directory`.

## `run`, the escape hatch

`run(f)` runs a callable on an I/O worker and resumes on the completion scheduler with
what it returned (`std::expected<void, std::error_code>` for a `void` callable). It is
for platform calls the portable operation set does not cover, such as a Linux `openat2`
with `RESOLVE_BENEATH`, which is several system calls that must run as a unit. It always
runs on the thread pool, on every platform (on an injected backend, wherever that backend's `submit()` sends it), and it consumes an in-flight slot like any
operation, so it cannot be used to route around `max_in_flight`.

**Why `f` is not simply kept in the coroutine.** Cancellation destroys frames, and a
worker may still be running `f` when it does. So `f` is moved, before anything can
suspend, into the operation block the backend co-owns. What is left behind in the frame
is a moved-from shell, which dies after the resumption or during teardown, so `f`'s move
constructor must leave a source that is inert to destroy (and `f` must be nothrow move
constructible).

What follows:

- **Cancelled after submission but before `f` ran:** never invoked, destroyed on the
  worker, with its captures.
- **Cancelled before submission** (parked on an in-flight slot, or the backend refused
  the submission): still never invoked and still destroyed, but **on the thread that
  tears the chain down**, which may be the completion thread. This window is exactly
  what `max_in_flight` creates, so a caller that needs its captures destroyed on a worker
  cannot rely on it there.
- **Cancelled while `f` runs:** it finishes, and its result is destroyed with the block.
- **`f` throws:** the exception is rethrown from the `co_await` if the chain is still
  there, and discarded with the block if not. Exceptions rethrow and error codes travel
  in the `expected`, as in [`task_completion_source`](task_completion_source.md). An
  exception from `f` is never in the `expected`'s error.
- The `expected`'s error carries failure to submit only: `not_enough_memory` if the block
  could not be allocated, or whatever the backend refused with, and
  `operation_canceled` where the chain was released before submission.

`f` runs on a worker concurrently with the caller, so it must not touch state the
caller's thread also touches without synchronization.

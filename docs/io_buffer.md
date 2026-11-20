# io_buffer

**Header:** `<alt/io_buffer.hpp>`

A refcounted byte buffer for asynchronous I/O. It exists because a read that the
operating system is still performing writes into memory the caller handed over, and
the coroutine that asked for the read can be destroyed first: cancelled, or abandoned
by its owner. A plain `std::span` would then dangle under the kernel's feet.
`io_buffer` is refcounted so the operation can hold its own copy, and the memory lives
until the last holder, kernel-facing or not, lets go.

```cpp
#include <alt/async_file.hpp>
#include <alt/io_buffer.hpp>

alt::task<void> fill(const alt::async_file& file)
{
    auto buffer = alt::io_buffer::allocate(4096);   // std::expected<io_buffer, std::error_code>
    if(!buffer)
    {
        co_return;      // std::errc::not_enough_memory, reported rather than thrown
    }

    // Copies share storage; handing one to an operation does not copy a byte.
    const auto n = co_await file.read_at(*buffer, 0, buffer->size());
}
```

The header includes no platform headers. The buffer is plain memory.

## Interface

```cpp
namespace alt
{
    class io_buffer
    {
    public:
        io_buffer() noexcept = default;
        io_buffer(const io_buffer&) noexcept;              // shares storage
        io_buffer& operator=(const io_buffer&) noexcept;
        io_buffer(io_buffer&& other) noexcept;             // leaves other empty and false
        io_buffer& operator=(io_buffer&& other) noexcept;
        ~io_buffer() = default;

        [[nodiscard]] static std::expected<io_buffer, std::error_code> allocate(std::size_t size);
        [[nodiscard]] static io_buffer borrow(std::span<std::byte> bytes, std::shared_ptr<void> keepalive) noexcept;
        [[nodiscard]] static io_buffer borrow_unsafe(std::span<std::byte> bytes) noexcept;

        [[nodiscard]] std::span<std::byte> bytes() const noexcept;
        [[nodiscard]] std::size_t          size() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
    };
}
```

## Three factories, three ownership stories

All three fit one shared pointer plus one span, so there is no variant and no second
ownership member.

- `allocate(size)` owns fresh, zero-initialized storage. Failure to allocate is
  reported as `std::errc::not_enough_memory` rather than thrown, so a caller sizing a
  buffer from untrusted input can handle it.
- `borrow(bytes, keepalive)` views storage someone else owns and holds that owner's
  keepalive for as long as any copy lives. The keepalive is stored by aliasing, so there
  is no extra control block. The buffer does not check that the keepalive really keeps
  `bytes` valid. A null keepalive makes this behave like `borrow_unsafe`.
- `borrow_unsafe(bytes)` views storage with nothing holding it. The caller promises the
  bytes outlive every operation that uses the buffer, **including one the caller has
  abandoned but the kernel has not finished**, which can be later than the moment a
  cancelled `co_await` returns. Prefer the other two unless the storage is static or
  provably immortal for that span.

## Copies share bytes

Copying is one atomic increment and never copies bytes: a write through one copy is
visible through every other. That is the point, and it is also why `bytes()` is `const`
without making the bytes immutable.

The reference count is atomic, so copying, moving and destroying buffers across threads
is safe. **The bytes are not interlocked.** Two concurrent operations must not share a
buffer region, and a caller must not touch a region while an operation owns it. This is
a precondition, not something the type enforces.

## `operator bool` is not "has bytes"

The buffer is `false` only when it names no storage at all: default-constructed, or
moved from. A zero-size `allocate(0)` succeeds, yields a real buffer, and is `true`. When
the question is whether there is anything to read or write, test `size()`. Whether
`bytes().data()` is null for a zero-size buffer is unspecified.

## The move constructor is user-provided on purpose

A defaulted move would move the owner out but copy the span and the flag, leaving the
source with a live-looking `bytes()` into storage it no longer keeps alive. That is the
dangling view this type exists to remove, and it would become a use-after-free if a
moved-from buffer ever reached the kernel. The moved-from buffer is reset to the
default-constructed state. Do not simplify the move operations back to `= default`.

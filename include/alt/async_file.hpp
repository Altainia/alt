#pragma once

#include <alt/detail/file_types.hpp>
#include <alt/flags.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/task.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>
#include <utility>

namespace alt
{

	class file_io;

	namespace detail
	{
		class io_handle;
		struct io_shared;
		struct async_file_access;
	}

	/**
	 * @brief The set of open_flag bits an open() takes, spelled once.
	 *
	 * `open_flags` is a value of alt::flags, whose construction from an enumerator is
	 * explicit: `flags(open_flag::read) | flags(open_flag::create)`.
	 */
	using open_flags = flags<open_flag>;

	/**
	 * @brief How much a sync() must make durable.
	 */
	enum class sync_mode : std::uint8_t
	{
		/** Data and the metadata needed to find it again: `fsync`, `FlushFileBuffers`. */
		full,

		/**
		 * Data, and only as much metadata as reading it back requires. `fdatasync` on
		 * Linux; `fsync` on every other POSIX system, which has no cheaper call; and
		 * `FlushFileBuffers` on Windows, which has no `fdatasync` equivalent.
		 *
		 * **This degrades to the STRONGER guarantee, never the weaker.** A caller who
		 * asked for `data` gets at least what `data` promises on every platform, and
		 * merely pays more where the platform cannot do better. The converse, silently
		 * downgrading to something that might lose the data, would be a durability bug.
		 */
		data,
	};

	/**
	 * @brief An open file, whose operations are awaitable and positional.
	 *
	 * Obtained from file_io::open() or adopted from a descriptor opened some other way.
	 * Operations return `alt::task<std::expected<T, std::error_code>>`: cancellation
	 * checking, backpressure and composition come from the machinery tasks already have,
	 * at the price of one coroutine frame allocation per operation.
	 *
	 * **There is no seek and no current position.** Every transfer names its offset, so
	 * two read_at()/write_at() calls on one async_file share no state and may be
	 * outstanding together; that is the property that makes concurrent use supported
	 * rather than merely tolerated.
	 *
	 * **What an operation owns.** Each task a member returns holds its own references to
	 * the descriptor and to the service state, taken when the member is CALLED rather
	 * than when the task first runs. So the async_file may be moved, closed or destroyed
	 * while a task made from it is suspended, or has never started, and the task still
	 * works. The descriptor is closed by whichever party drops the last reference, which
	 * for a cancelled operation can be a worker thread long after the awaiting frame is
	 * gone. That is what stops a cancelled read from reading through a closed descriptor
	 * number the operating system has since given to another file.
	 *
	 * **Closing.** The destructor drops this object's reference and closes inline only
	 * if that was the last one. With an operation outstanding it closes nothing and the
	 * last operation to finish does, off the thread that destroyed the file. Either way
	 * the close cannot report an error, and where it runs inline it can block on a
	 * network filesystem. close() does not remove either limit in this release (see its
	 * note); it exists so that a backend with an asynchronous close can later report the
	 * error and do the work off the calling thread without changing this interface.
	 *
	 * **Why every operation validates before it takes a backpressure permit.** The
	 * checks run first so that the rejecting paths hold no permit and so have nothing to
	 * release; see detail::io_awaiter for the permit's contract.
	 *
	 * Thread safety: operations may be initiated from any thread and run concurrently.
	 * The async_file object itself is not synchronized: it may move between threads but
	 * is not thread-safe, and close() racing an outstanding operation on the same file is
	 * a precondition violation. It is a logic error rather than a memory error, since the
	 * descriptor stays valid for as long as any operation holds it.
	 *
	 * The service (alt::file_io) must outlive every operation started on a file it
	 * produced; see its class note.
	 */
	class async_file
	{
	public:
		/**
		 * @brief Takes ownership of a descriptor opened some other way.
		 *
		 * This is how a consumer whose open is not expressible through open_flag, such
		 * as one that confines the lookup to a directory with several syscalls, brings
		 * the result into the awaitable world.
		 *
		 * @param io     The service the file's operations run on. Must outlive them.
		 * @param handle A descriptor on POSIX, a HANDLE on Windows. The caller must not
		 *               close it afterwards, nor adopt the same one twice: two owners
		 *               mean a double close, which can close a recycled descriptor.
		 *               invalid_native_handle is allowed and yields a file on which
		 *               every operation reports std::errc::bad_file_descriptor.
		 *               Any other handle must be one on which a transfer blocks until
		 *               it is done, and that is the caller's responsibility:
		 *               a Windows HANDLE must NOT have been opened with
		 *               FILE_FLAG_OVERLAPPED, because the transfer arm passes an
		 *               OVERLAPPED only to carry the offset and treats
		 *               ERROR_IO_PENDING as a failure, after which the operation
		 *               completes and releases its buffer while the kernel may still
		 *               be writing into it. A POSIX descriptor must be seekable,
		 *               because positional transfers need that: a non-seekable one (a
		 *               FIFO, socket or tty) reports std::errc::illegal_seek. Windows
		 *               has no equivalent: a pipe handle ignores the offset rather than
		 *               failing, so it is the caller's to avoid there. Handles
		 *               opened by file_io::open satisfy both.
		 */
		[[nodiscard]] static async_file adopt(const file_io& io, native_handle handle);

		/** @brief Drops this object's reference, closing inline only if it was the last. See the class note. */
		~async_file() = default;

		/** @brief Moves the descriptor; the source is left closed, and its operations report bad_file_descriptor. */
		async_file(async_file&& other) noexcept = default;

		/** @brief Move-assigns as the move constructor does, dropping this file's own reference first. */
		async_file& operator=(async_file&& other) noexcept = default;
		async_file(const async_file&)                      = delete;
		async_file& operator=(const async_file&)           = delete;

		/**
		 * @brief Reads up to @p size bytes at @p offset into @p buffer.
		 *
		 * Completes with the number of bytes read, which is shorter than @p size at the
		 * end of the file and 0 at or past it. @p buffer is kept alive by the operation,
		 * not by the caller: it stays valid for the kernel even if this task is
		 * cancelled first.
		 *
		 * Errors, checked in this order and all before anything is submitted:
		 *  - std::errc::bad_file_descriptor: the file was closed (close() was called,
		 *    even on a copy of the descriptor an operation was already holding), was
		 *    moved from, or adopted an invalid handle. Reported here rather than
		 *    submitted because a descriptor number may have been reused by then;
		 *  - std::errc::invalid_argument: @p size exceeds buffer.size(). It is rejected,
		 *    not clamped: the backend does clamp, as a second line of defense, but a
		 *    clamp reports a short read that a caller cannot tell from a real one.
		 *
		 * A @p size of 0 completes with 0 bytes immediately and submits nothing.
		 * Anything else is the backend's error, such as std::errc::is_a_directory.
		 *
		 * std::errc::operation_canceled is part of the result type's contract, and is never
		 * delivered to a caller in the current design: a cancelled chain is abandoned
		 * rather than resumed, and the service state outlives every operation parked on it.
		 * It is retained so that an error switch stays total, and every operation below
		 * shares it.
		 */
		[[nodiscard]] task<std::expected<std::size_t, std::error_code>> read_at(io_buffer buffer, std::uint64_t offset, std::size_t size) const;

		/**
		 * @brief Writes @p size bytes of @p buffer at @p offset.
		 *
		 * Completes with the number of bytes written. The errors, the ordering of the
		 * checks, the zero-length shortcut and the buffer's lifetime are read_at()'s.
		 * A file opened with open_flag::append ignores @p offset on POSIX.
		 */
		[[nodiscard]] task<std::expected<std::size_t, std::error_code>> write_at(io_buffer buffer, std::uint64_t offset, std::size_t size) const;

		/**
		 * @brief Flushes the file to stable storage.
		 *
		 * @param mode How much to make durable. See sync_mode, in particular that `data`
		 *             never gives less than it promises.
		 *
		 * Reports std::errc::bad_file_descriptor for a closed file before submitting, and
		 * carries std::errc::operation_canceled in its contract as read_at() describes;
		 * anything else is the backend's.
		 */
		[[nodiscard]] task<std::expected<void, std::error_code>> sync(sync_mode mode = sync_mode::full) const;

		/**
		 * @brief Describes the open file: size, modification time, kind and identity.
		 *
		 * Asks the descriptor rather than the path, so it is correct for a file that has
		 * been renamed or unlinked since it was opened. The file_identity is the same for
		 * every descriptor of one file, which is what lets a caller build an ETag from
		 * inode, size and mtime.
		 *
		 * Reports std::errc::bad_file_descriptor for a closed file before submitting, and
		 * carries std::errc::operation_canceled in its contract as read_at() describes.
		 */
		[[nodiscard]] task<std::expected<file_status, std::error_code>> stat() const;

		/**
		 * @brief Marks the file closed, and releases this object's reference to the descriptor.
		 *
		 * **The closed state takes effect when this is CALLED, not when it is awaited.**
		 * From that instant every operation that has not yet submitted, including tasks
		 * made earlier and not yet started, reports std::errc::bad_file_descriptor, and
		 * handle() reports invalid_native_handle. The descriptor itself closes when the
		 * last reference to it goes: the one this call took over (released when the
		 * returned task completes or is destroyed) and those of any operation still in
		 * flight. So an operation already at the backend finishes against a live
		 * descriptor, and a descriptor number is never closed under one.
		 *
		 * The operating-system close runs wherever the last reference drops: typically
		 * inside the returned task, on the thread awaiting it, and so it can block that
		 * thread on a network filesystem exactly as the destructor can. With an operation
		 * still in flight it runs on whichever thread finishes last.
		 *
		 * Closing twice, or closing a moved-from file, succeeds and does nothing.
		 *
		 * @note The error `::close` may report is not available in this release: the
		 *       descriptor's own destructor discards it, as a destructor must. The
		 *       result type is `expected` so that a backend with an asynchronous close
		 *       can report it without changing this signature.
		 */
		[[nodiscard]] task<std::expected<void, std::error_code>> close();

		/**
		 * @brief The underlying descriptor, or invalid_native_handle if closed or moved from.
		 *
		 * For handing to a call the library does not wrap. Do not close it, and do not use
		 * it after close() or once the file is destroyed.
		 */
		[[nodiscard]] native_handle handle() const noexcept;

	private:
		async_file(std::shared_ptr<detail::io_handle> file, std::shared_ptr<detail::io_shared> owner) noexcept;

		/** Co-owned with every operation block in flight on this file. */
		std::shared_ptr<detail::io_handle> m_file;
		/** The service state, co-owned for the same reason. */
		std::shared_ptr<detail::io_shared> m_owner;

		friend struct detail::async_file_access;
	};

	namespace detail
	{

		/**
		 * @brief The one route by which the file layer's coroutines build an async_file.
		 *
		 * A coroutine that completes an open cannot call async_file::adopt(), which takes the
		 * service by reference: its resumption may run after the service object is gone,
		 * which the shutdown contract allows, and only the refcounted state survives that.
		 */
		struct async_file_access
		{
			/** @brief An async_file over @p file, co-owning @p owner. */
			[[nodiscard]] static async_file make(std::shared_ptr<io_handle> file, std::shared_ptr<io_shared> owner) noexcept
			{
				return async_file{std::move(file), std::move(owner)};
			}
		};

	}

}

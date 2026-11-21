#pragma once

#include <alt/io_backend.hpp>
#include <atomic>
#include <cstdint>
#include <memory>

namespace alt::detail
{

	/**
	 * @brief Where an io_handle's handle came from, which decides the backend that serves it.
	 *
	 * Only the IOCP backend tells the two apart: it can transfer only on a handle it opened
	 * itself, overlapped and associated with its port, so alt::detail::io_shared::backend_for()
	 * sends a file of any other origin to the thread-pool backend an IOCP service also owns.
	 * Every other backend serves both alike.
	 */
	enum class handle_origin : std::uint8_t
	{
		/** Produced by the service's own open, and so prepared for its backend. */
		opened,

		/** Brought in from outside (alt::async_file::adopt), with no promise about how it was opened. */
		adopted,
	};

	/**
	 * @brief A refcounted owner of one operating-system file handle.
	 *
	 * Cancellation in `alt` destroys coroutine frames, while io_uring and IOCP both
	 * deliver a completion after a cancel request rather than at it. So a cancelled
	 * read can still be in the kernel's hands when the frame that asked for it, and
	 * the async_file it used, are gone. If that async_file closed the descriptor on
	 * destruction, a worker that had already issued its `pread` would read from a
	 * closed descriptor, and worse, the descriptor NUMBER can be reassigned to a
	 * different file in the meantime. The read then succeeds against someone else's
	 * data: a silent wrong answer, where a plain close would only have produced a
	 * benign EBADF.
	 *
	 * The fix is that nothing closes the descriptor except this object's destructor,
	 * and every in-flight operation holds a `shared_ptr<io_handle>`. The descriptor
	 * therefore outlives the last operation that might touch it, whichever of the
	 * file object or the operation finishes last.
	 *
	 * That is why request_close() does not close: it only records that the owner
	 * asked, so new operations can be refused while old ones drain. The actual
	 * close is deferred to the last reference.
	 *
	 * has_handle() is therefore true for the whole life of a handle that adopted a valid
	 * descriptor, and a handle never closes before it is destroyed. It exists so a
	 * caller can tell an adopted handle from one adopted with invalid_native_handle.
	 *
	 * Thread safety: every member is safe to call concurrently. The handle value is
	 * immutable after adopt(), and the close-requested flag is atomic.
	 */
	class io_handle
	{
		struct private_token
		{
			explicit private_token() = default;
		};

	public:
		/**
		 * @brief Takes ownership of a handle.
		 *
		 * The caller must not close it afterwards, and must not adopt the same
		 * descriptor twice: two owners mean a double close, which can close a recycled
		 * descriptor, the very hazard this class exists to prevent. Adopting invalid_native_handle is
		 * allowed and yields a handle for which has_handle() is false and the destructor
		 * closes nothing.
		 *
		 * @param handle A descriptor on POSIX, a HANDLE on Windows.
		 * @param origin Where @p handle came from; see handle_origin. The default is the safe
		 *               one: only the file layer's own open, which knows its backend prepared
		 *               the handle, passes handle_origin::opened.
		 */
		[[nodiscard]] static std::shared_ptr<io_handle> adopt(native_handle handle, handle_origin origin = handle_origin::adopted);

		/** @brief Constructor reserved for adopt(); the token makes it uncallable elsewhere. */
		io_handle(private_token /*token*/, native_handle handle, handle_origin origin) noexcept;

		/**
		 * @brief Closes the descriptor if one was adopted. The only place it is closed.
		 *
		 * The result of close()/CloseHandle() is discarded on purpose: a destructor cannot
		 * recover, and Linux releases the descriptor even when close() fails with EINTR.
		 */
		~io_handle();

		io_handle(const io_handle&)            = delete;
		io_handle& operator=(const io_handle&) = delete;
		io_handle(io_handle&&)                 = delete;
		io_handle& operator=(io_handle&&)      = delete;

		/** @brief The adopted handle, valid for as long as any reference to this object exists. */
		[[nodiscard]] native_handle native() const noexcept
		{
			return m_handle;
		}

		/** @brief True if a valid handle was adopted. A constant after construction: it does not track close_requested(). */
		[[nodiscard]] bool has_handle() const noexcept
		{
			return m_handle != invalid_native_handle;
		}

		/** @brief Where the handle came from, as given to adopt(). A constant after construction. */
		[[nodiscard]] handle_origin origin() const noexcept
		{
			return m_origin;
		}

		/**
		 * @brief Records that the owner asked to close, without closing.
		 *
		 * Closing here would reintroduce the number-reuse hazard described on the class.
		 * The descriptor closes when the last reference goes. Idempotent.
		 */
		void request_close() noexcept
		{
			m_close_requested.store(true, std::memory_order_release);
		}

		/** @brief True once request_close() has been called. */
		[[nodiscard]] bool close_requested() const noexcept
		{
			return m_close_requested.load(std::memory_order_acquire);
		}

	private:
		native_handle       m_handle;
		const handle_origin m_origin;
		std::atomic<bool>   m_close_requested{false};
	};

}

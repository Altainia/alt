#pragma once

#include <alt/async_file.hpp>
#include <alt/io_backend.hpp>
#include <alt/scheduler.hpp>
#include <alt/task.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <system_error>

namespace alt
{

	class thread_pool;

	namespace detail
	{
		struct io_shared;
		struct file_io_access;
	}

	/**
	 * @brief How an alt::file_io is configured.
	 *
	 * Plain data with usable defaults, so the common case is `create(pool, home)`.
	 */
	struct file_io_options
	{
		/**
		 * Which built-in backend to use. `automatic` probes the platform and settles on
		 * the best one it can run; in this release that is always
		 * io_backend_kind::thread_pool, and file_io::backend() reports what was chosen.
		 * Naming a kind this build does not provide, or `manual`, which cannot be
		 * selected by value, fails create() rather than silently substituting another.
		 * With the overload that is handed a backend, `automatic` means that backend and any
		 * other value must match its kind().
		 */
		io_backend_kind backend = io_backend_kind::automatic;

		/**
		 * The backpressure limit: how many operations may be at the backend at once.
		 * Further operations suspend until one finishes. Must be positive, because a
		 * zero bound would park the first operation on a permit that can never exist.
		 */
		std::size_t max_in_flight = 64;

		/**
		 * The submission ring size for a backend that has one; 0 derives it from
		 * max_in_flight. Unused by the thread-pool backend.
		 */
		std::size_t queue_depth = 0;
	};

	/**
	 * @brief The service object for awaitable file I/O.
	 *
	 * Owns the backend choice, the backpressure limit and the scheduler every
	 * operation's completion is posted to. Operations hang off it.
	 *
	 * **Construction goes through create(), and the type is immovable**, for the reason
	 * alt::thread_pool gives: references to it live inside suspended coroutine frames,
	 * so the caller binds a reference into the returned std::expected, whose address is
	 * stable because the object was built in place.
	 *
	 * **This is a handle, not the state.** The accounting an in-flight operation still
	 * touches when its resumption runs (the backpressure permits) lives in a refcounted
	 * block that every operation co-owns. That is what lets the destructor below return
	 * while resumptions are still queued.
	 *
	 * **Destruction waits for the backend, not for the resumptions.** Every completion
	 * ends in a post to the completion scheduler, and for a single-threaded event loop
	 * that is the thread destroying this object, so waiting for those posts to run would
	 * deadlock with nothing pumping the loop. The destructor stops admitting submissions
	 * and blocks until no operation is outstanding at the backend. When it returns, no
	 * backend callback will fire again and the kernel holds none of this object's
	 * memory. Resumptions already posted may still be queued; they release their permit
	 * against state that outlives this handle.
	 *
	 * **Two preconditions, stated rather than enforced** (both have precedent in
	 * alt::scheduler_ref and alt::thread_pool):
	 *  - this object must outlive every operation started on it. An operation still
	 *    parked on a backpressure permit at destruction is a violation, and asserts in
	 *    builds with asserts enabled;
	 *  - the destructor must not run on a thread of the borrowed alt::thread_pool, where
	 *    it would wait for jobs queued behind itself. That is why the pool is borrowed
	 *    rather than owned: a precondition can be stated, an owned pool's last
	 *    reference dying on its own worker cannot be.
	 *
	 * There is deliberately no separate stop-accepting/drain API: one documented
	 * obligation beats a second shutdown protocol with different ergonomics from
	 * alt::thread_pool's.
	 */
	class file_io
	{
	private:
		/** Makes the constructor unreachable except through create(). */
		struct passkey
		{
			explicit passkey() = default;
		};

	public:
		/**
		 * @brief Creates a service on a built-in backend, or reports why it could not.
		 *
		 * @param workers    The pool the thread-pool arm runs on. Borrowed: it must
		 *                   outlive this object, and this object must not be destroyed
		 *                   on one of its threads.
		 * @param completion Where every operation's awaiting coroutine is resumed. Must
		 *                   not be empty, and must outlive every operation.
		 * @param options    See file_io_options.
		 *
		 * Fails with std::errc::invalid_argument for a zero max_in_flight, an empty
		 * @p completion (reported here, because posting to an empty scheduler_ref
		 * asserts on whichever I/O thread finishes first, far from the mistake), or
		 * io_backend_kind::manual; with std::errc::not_supported for a kind this build
		 * does not provide.
		 */
		[[nodiscard]] static std::expected<file_io, std::error_code> create(thread_pool& workers, scheduler_ref completion, const file_io_options& options = {});

		/**
		 * @brief Creates a service over a backend the caller owns.
		 *
		 * This is how a test drives the service with a manual_io_backend, which an
		 * enumerator cannot conjure. @p backend must outlive this object. The same
		 * argument failures as the overload above, and options.backend must be
		 * `automatic` or equal to @p backend's kind(): a mismatch fails with
		 * std::errc::invalid_argument rather than silently using a backend the caller did
		 * not ask for. backend() reports @p backend's own kind().
		 */
		[[nodiscard]] static std::expected<file_io, std::error_code> create(thread_pool& workers, scheduler_ref completion, io_backend& backend, const file_io_options& options = {});

		/** Called only by create(). The passkey is private, so nothing else can. */
		file_io(passkey /*unused*/, std::unique_ptr<io_backend> owned, io_backend& backend, thread_pool& workers, scheduler_ref completion, std::size_t max_in_flight);

		file_io()                          = delete;
		file_io(const file_io&)            = delete;
		file_io& operator=(const file_io&) = delete;
		file_io(file_io&&)                 = delete;
		file_io& operator=(file_io&&)      = delete;

		/** Blocks until the backend is quiet. See the class note for what that excludes. */
		~file_io();

		/** @brief Which backend is actually in use. Never io_backend_kind::automatic. */
		[[nodiscard]] io_backend_kind backend() const noexcept;

		/**
		 * @brief Opens a file, resuming on the completion scheduler with an async_file.
		 *
		 * @param path  Copied into the operation, so it need not outlive the call.
		 * @param how   Must name read, write or both; neither is std::errc::invalid_argument.
		 * @param mode  The permission bits for a file this creates; used only with
		 *              open_flag::create, and subject to the umask on POSIX.
		 *
		 * Fails with the operating system's error: std::errc::no_such_file_or_directory for
		 * a missing path, file_exists for open_flag::exclusive over an existing one, and
		 * std::errc::operation_canceled as async_file::read_at() describes.
		 *
		 * **A cancelled open does not leak the descriptor.** The open may still complete
		 * at the backend after the awaiting frame is gone, and the descriptor it produces
		 * is owned by the operation's block until the task takes it, so it is closed there
		 * if nobody does.
		 *
		 * The returned file keeps the service's shared state alive but not this object:
		 * this must outlive every operation, as the class note says.
		 */
		[[nodiscard]] task<std::expected<async_file, std::error_code>> open(const std::filesystem::path& path, open_flags how, file_mode mode = {}) const;

	private:
		/** The one door to the engine-internal state; see alt::detail::file_io_access. */
		friend struct detail::file_io_access;

		/** The backend this object built, or null when the caller supplied one. */
		std::unique_ptr<io_backend>        m_owned_backend;
		std::shared_ptr<detail::io_shared> m_shared;
		io_backend_kind                    m_kind;
	};

}

namespace alt::detail
{

	/**
	 * @brief The single route from an alt::file_io to the state its operations co-own.
	 *
	 * Befriended once by file_io, so the operations built on it and the tests that drive
	 * its shutdown contract all take their co-ownership through this one door, and no
	 * engine-internal type appears in file_io's public interface.
	 */
	struct file_io_access
	{
		/** @brief The refcounted state every operation on @p io co-owns. */
		[[nodiscard]] static const std::shared_ptr<io_shared>& shared(const file_io& io) noexcept
		{
			return io.m_shared;
		}
	};

}

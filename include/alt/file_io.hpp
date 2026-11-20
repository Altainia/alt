#pragma once

#include <alt/async_file.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/io_backend.hpp>
#include <alt/scheduler.hpp>
#include <alt/task.hpp>
#include <concepts>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>
#include <type_traits>
#include <vector>

namespace alt
{

	class thread_pool;

	namespace detail
	{
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
		 * does not provide; and with std::errc::not_enough_memory if constructing the
		 * service throws std::bad_alloc.
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
		 * carries std::errc::operation_canceled in its contract as async_file::read_at() describes.
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

		/**
		 * @brief Describes the file at @p path, following symbolic links.
		 *
		 * @param path Copied into the operation, so it need not outlive the call.
		 *
		 * The path counterpart of async_file::stat(), for when there is no descriptor to
		 * ask. The kind is never file_kind::symlink, because the target is described, and
		 * a dangling link is an error. Fails with the operating system's error, for
		 * example std::errc::no_such_file_or_directory.
		 */
		[[nodiscard]] task<std::expected<file_status, std::error_code>> stat(const std::filesystem::path& path) const;

		/**
		 * @brief Renames @p from to @p to, replacing @p to if it is an existing file.
		 *
		 * Both paths are copied into the operation. Replacement is the behavior on every
		 * platform (POSIX rename(2) does it natively; Windows is asked for it explicitly),
		 * so a caller can use this for an atomic "write a temporary, then move it into
		 * place" without a platform branch.
		 *
		 * Fails with std::errc::cross_device_link when the two paths are on different
		 * filesystems on POSIX: this is a rename, never a copy, and a caller that wants a
		 * move across filesystems must copy and unlink itself. A failed rename leaves
		 * @p from where it was.
		 */
		[[nodiscard]] task<std::expected<void, std::error_code>> rename(const std::filesystem::path& from, const std::filesystem::path& to) const;

		/**
		 * @brief Removes the file or symbolic link at @p path.
		 *
		 * @p path is copied into the operation. A link is removed, not followed. Not for
		 * directories. Fails with the operating system's error:
		 * std::errc::no_such_file_or_directory for a missing path.
		 */
		[[nodiscard]] task<std::expected<void, std::error_code>> unlink(const std::filesystem::path& path) const;

		/**
		 * @brief Lists the entries of the directory at @p path, in no particular order.
		 *
		 * @p path is copied into the operation. `.` and `..` are never reported, and the
		 * whole directory is read before the task completes, so a listing is all or an
		 * error and never a partial result.
		 *
		 * Each entry's kind describes the entry itself, so a symbolic link is reported as a
		 * link (file_kind::symlink) rather than as its target. An entry whose kind could
		 * not be determined, for example because it was removed while the listing ran, is
		 * reported as file_kind::unknown instead of failing the whole listing; a caller
		 * that must know stats the entry. No test forces that case on a real filesystem.
		 *
		 * Fails with std::errc::not_a_directory for a path that is a file, and with the
		 * operating system's error otherwise.
		 */
		[[nodiscard]] task<std::expected<std::vector<directory_entry>, std::error_code>> list_directory(const std::filesystem::path& path) const;

		/**
		 * @brief The escape hatch: runs @p f on an I/O worker and resumes on the completion
		 *        scheduler with what it returned.
		 *
		 * For platform calls the portable operation set does not cover, such as a Linux
		 * `openat2` with `RESOLVE_BENEATH`, which is several system calls that must run as a
		 * unit. A `void` callable yields `std::expected<void, std::error_code>`.
		 *
		 * **Why @p f is not simply kept in the coroutine.** Cancellation tears a chain down by
		 * destroying its frames, and a worker may still be running @p f when it does. A callable
		 * left in the frame would then lose its captures from under itself. So @p f is moved,
		 * before anything can suspend, into the operation block, which the backend co-owns, and
		 * what is left behind in the frame is a moved-from shell. That shell dies at the end of
		 * the full-expression containing the `co_await`, that is after the resumption on the
		 * completion scheduler or during teardown on whichever thread abandons the chain, so
		 * a callable's move constructor must leave a source that is inert to destroy.
		 *
		 * What follows from that:
		 *  - **Cancelled after submission but before @p f ran:** it is never invoked, and it is
		 *    destroyed on the worker, taking its captures with it.
		 *  - **Cancelled before submission:** while the operation is parked on an in-flight
		 *    slot, or if the backend refused the submission. It is still never invoked and
		 *    still destroyed, but on the thread that tears the chain down, which may be the
		 *    completion thread. This window is exactly what max_in_flight creates, so a
		 *    caller that needs its captures destroyed on a worker cannot rely on it there.
		 *  - **Cancelled while @p f runs:** it finishes, and its result is destroyed with the
		 *    block. Cancellation detaches; it does not abort.
		 *  - **@p f throws:** the exception is rethrown from the `co_await` if the chain is
		 *    still there, and discarded with the block if it is not. Exceptions rethrow from
		 *    the await and error codes travel in the `expected`, as in
		 *    alt::task_completion_source.
		 *  - **The slot:** it consumes an in-flight slot like any operation, so it cannot be
		 *    used to route around max_in_flight.
		 *  - **Where it runs:** always on the thread pool, on every platform, because a
		 *    submission queue has nowhere to run arbitrary code.
		 *
		 * The `expected`'s error carries failure to submit only (std::errc::not_enough_memory
		 * if the block could not be allocated, or whatever the backend refused with), and
		 * std::errc::operation_canceled where the chain was released before submission.
		 * An exception thrown by @p f is never in it.
		 *
		 * @p f runs on a worker, concurrently with the caller, so it must not touch state the
		 * caller's thread also touches without synchronization.
		 */
		template<std::invocable F>
		  requires std::is_nothrow_move_constructible_v<F> && (!std::is_reference_v<std::invoke_result_t<F>>) && (!std::is_array_v<std::invoke_result_t<F>>)
		[[nodiscard]] task<std::expected<std::invoke_result_t<F>, std::error_code>> run(F f) const;

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

	/**
	 * @brief The coroutine behind alt::file_io::run(), taking the callable already erased.
	 *
	 * Separate from run() so that no coroutine parameter ever holds the callable: a
	 * coroutine's by-value parameters are copied into its frame, and the frame is exactly
	 * what cancellation destroys under a running worker. This takes only a shared_ptr to
	 * the block-side storage.
	 */
	template<typename F>
	task<std::expected<std::invoke_result_t<F>, std::error_code>> run_invocation(std::shared_ptr<io_shared> owner, std::shared_ptr<io_invocation<F>> call)
	{
		using result = std::expected<std::invoke_result_t<F>, std::error_code>;

		auto permit = co_await acquire_permit(owner); // non-const: io_permit is move-only
		if(!permit)
		{
			co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
		}
		const io_completion done = co_await io_awaiter{owner, io_request{.op = io_op::invoke, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = nullptr, .out = static_cast<io_invocable*>(call.get()), .state = call}, nullptr, std::move(*permit)};
		if(done.error)
		{
			co_return std::unexpected(done.error);
		}
		call->rethrow_if_failed();
		if constexpr(std::is_void_v<std::invoke_result_t<F>>)
		{
			co_return result{};
		}
		else
		{
			co_return result{call->take()};
		}
	}

	/** @brief The task a run() that could not allocate its block returns. */
	template<typename Result>
	task<Result> run_failed_to_allocate()
	{
		co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
	}

}

namespace alt
{

	template<std::invocable F>
	  requires std::is_nothrow_move_constructible_v<F> && (!std::is_reference_v<std::invoke_result_t<F>>) && (!std::is_array_v<std::invoke_result_t<F>>)
	task<std::expected<std::invoke_result_t<F>, std::error_code>> file_io::run(F f) const
	{
		using result = std::expected<std::invoke_result_t<F>, std::error_code>;

		// Not a coroutine, so `f` is never copied into a frame: it goes straight from the
		// argument into the block-side storage.
		std::shared_ptr<detail::io_invocation<F>> call;
		try
		{
			call = std::make_shared<detail::io_invocation<F>>(std::move(f));
		}
		catch(const std::bad_alloc&)
		{
			return detail::run_failed_to_allocate<result>();
		}
		return detail::run_invocation<F>(m_shared, std::move(call));
	}

}

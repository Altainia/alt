#pragma once

#include <alt/io_buffer.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

namespace alt
{

	class thread_pool;

	/**
	 * @brief An operating-system file handle, spelled without a platform header.
	 *
	 * Carries an `int` file descriptor on POSIX and a `HANDLE` on Windows. A signed
	 * pointer-sized integer holds both losslessly, which is the only reason it is
	 * not a typedef of either: `io_backend.hpp` must be includable everywhere
	 * without `unistd.h` or `windows.h`. The conversion to and from the native type
	 * belongs to the backend that owns the platform.
	 *
	 * `invalid_native_handle` (-1) is the "no handle" value on both platforms.
	 */
	using native_handle = std::intptr_t;

	/** @brief The value of a native_handle that refers to nothing. */
	inline constexpr native_handle invalid_native_handle = -1;

	/**
	 * @brief Which implementation an io_backend is.
	 *
	 * `automatic` is a request, never a report: it asks the factory to pick the best
	 * backend for the platform and is never returned by io_backend::kind().
	 * `manual` is what manual_io_backend reports. It cannot be selected by value,
	 * because an enumerator cannot conjure an instance; a test injects a
	 * manual_io_backend directly.
	 */
	enum class io_backend_kind : std::uint8_t
	{
		automatic,
		thread_pool,
		io_uring,
		iocp,
		manual,
	};

	/**
	 * @brief The operation an io_request asks a backend to perform.
	 *
	 * The per-operation field table is on io_request.
	 */
	enum class io_op : std::uint8_t
	{
		read,
		write,
		sync_full,
		sync_data,
		stat_handle,
		stat_path,
		open,
		rename,
		unlink,
		list_directory,
		invoke,
	};

	/**
	 * @brief One operation, described by value.
	 *
	 * The request is a plain description. A backend MUST copy whatever it needs
	 * before submit() returns: the reference passed to submit() is not kept alive
	 * for the operation, so a backend that stores the pointer instead of the
	 * contents reads a dead stack frame. (The buffer member is the exception that
	 * proves the rule: copying it is exactly how a backend keeps the storage alive.)
	 *
	 * `buffer` is held BY VALUE on purpose. A copy of the refcounted io_buffer is
	 * what keeps the bytes alive after the coroutine that asked for the read has
	 * been destroyed but before the kernel has finished writing into them. A
	 * backend keeps its copy alive until the completion callback has RETURNED, not
	 * merely until it starts. (manual_io_backend drops its stored request's copy
	 * before the callback but holds a private one across it; see its description.)
	 *
	 * `in` and `out` carry what the struct does not model. `in` is operation-specific
	 * input (a path pair for rename); `out` is
	 * storage for a result that is not a byte count (a `file_status` for the two
	 * stat operations, a `std::vector<directory_entry>` for list_directory). BOTH
	 * MUST POINT INTO STORAGE THE OPERATION BLOCK CO-OWNS (see `state`) AND NEVER INTO A
	 * COROUTINE FRAME: a
	 * completion can land after the frame is gone, and a write through `out` into a
	 * destroyed frame is a use-after-free no sanitizer reliably reports. This layer
	 * cannot check it; it is a precondition on the layer that builds requests.
	 *
	 * Fields each operation reads (`handle` is the target handle unless stated),
	 * and where its result goes. Every operation reports success or failure through
	 * io_completion::error; only read and write also set io_completion::bytes.
	 *
	 *  | op             | reads                          | result                           |
	 *  |----------------|--------------------------------|----------------------------------|
	 *  | read           | handle, buffer, offset, size   | bytes read                       |
	 *  | write          | handle, buffer, offset, size   | bytes written                    |
	 *  | sync_full      | handle                         | error only                       |
	 *  | sync_data      | handle                         | error only                       |
	 *  | stat_handle    | handle, out                    | `file_status` written to `out`   |
	 *  | stat_path      | in (path), out                 | `file_status` written to `out`   |
	 *  | open           | in (path and open flags), out  | `native_handle` written to `out` |
	 *  | rename         | in (path pair)                 | error only                       |
	 *  | unlink         | in (path)                      | error only                       |
	 *  | list_directory | in (path), out                 | `std::vector<directory_entry>`   |
	 *  | invoke         | out (`io_invocable*`)          | error only                       |
	 *
	 * `open` stores the new handle through `out`, which is a `native_handle*`;
	 * io_completion has no field for it and the seam does not grow one. On error
	 * the pointee is left untouched.
	 *
	 * list_directory and invoke have no kernel equivalent on any platform (neither
	 * io_uring nor IOCP can enumerate a directory or run arbitrary code). EVERY
	 * backend runs them on a helper thread. That is an implementation burden on the
	 * backend, not a gap in this interface.
	 *
	 * **An invoke job carries an obligation the other operations do not.** Its `out` is a
	 * `detail::io_invocable*` (declared in detail/io_operation.hpp), and a backend MUST, on
	 * the same helper thread that would have run it:
	 *  - call `io_invocable::invoke()` when the job runs, which runs the callable and destroys
	 *    it; and
	 *  - call `io_invocable::discard()` INSTEAD, for a job cancelled before it started, and
	 *    complete it with std::errc::operation_canceled. `discard()` is what destroys the
	 *    callable and its captures on the helper thread. A backend that simply skips a
	 *    cancelled job leaves the callable to die on whichever thread drops the operation
	 *    block's last reference, which may be the completion thread, and silently loses the
	 *    guarantee alt::file_io::run() documents.
	 * Neither is called for a job whose submit() failed. Both are noexcept.
	 *
	 * What `in` and `out` point at, per operation. `in` is `const void*` and the types
	 * below are what it is cast back to: a `const std::filesystem::path*` for stat_path,
	 * unlink and list_directory; a `const io_open_args*` for open; a
	 * `const io_rename_args*` for rename. `out` is a `file_status*` for the two stat
	 * operations, a `native_handle*` for open, and a `std::vector<directory_entry>*`
	 * (the whole vector, not one entry) for list_directory. All of these types are in
	 * detail/file_types.hpp. This header names them only in documentation and carries
	 * the pointers as `void`. `out` is a `detail::io_invocable*` for invoke, stored as the
	 * already-adjusted base pointer, so casting it back is exact.
	 *
	 * A read or write moves at most `min(size, buffer.size())` bytes: a `size` larger
	 * than the buffer is clamped, and the short transfer is reported as success, which
	 * is indistinguishable from a real short read. The Windows arm also clamps to
	 * `std::numeric_limits<DWORD>::max()` bytes, because ReadFile and WriteFile take a
	 * 32-bit count, so a transfer above that is short in the same way whatever the
	 * buffer holds. A layer that must refuse that
	 * request has to do so before submitting.
	 */
	struct io_request
	{
		io_op         op     = io_op::read;
		native_handle handle = invalid_native_handle;
		io_buffer     buffer;
		std::uint64_t offset = 0;
		std::size_t   size   = 0;
		const void*   in     = nullptr;
		void*         out    = nullptr;

		/**
		 * What `in` and `out` point into, kept alive by the operation block.
		 *
		 * The seam's own answer to the rule above: `in` and `out` must outlive the
		 * operation, a coroutine frame does not, and `buffer` is for the bytes of a transfer
		 * rather than for this. A layer whose request carries a result slot or arguments
		 * (a stat's file_status, an open's path and resulting handle) puts them in one
		 * object, points `in`/`out` into it and stores the owner here; the block co-owns it
		 * for its whole life, which is exactly as long as a late completion can write
		 * through `out`. Null for a request that needs nothing beyond `buffer`.
		 *
		 * A backend need not touch it: copying the request copies the reference, which at
		 * worst holds the object a little longer.
		 */
		std::shared_ptr<void> state;
	};

	/**
	 * @brief What an operation produced.
	 *
	 * `bytes` is meaningful for read and write only. Every other operation reports
	 * through `error` and, where it has one, the request's `out`. A default value is
	 * success with zero bytes.
	 */
	struct io_completion
	{
		std::size_t     bytes = 0;
		std::error_code error;
	};

	/**
	 * @brief The completion callback. Plain function pointer plus a state pointer.
	 *
	 * Not a std::function: it is invoked from an I/O thread, possibly inline, and
	 * must not allocate or throw. `noexcept` is part of the type; a callback that
	 * lets an exception escape terminates, which is the right outcome for an I/O
	 * thread that has nobody to hand the exception to.
	 */
	using io_completion_fn = void (*)(void* state, io_completion result) noexcept;

	/**
	 * @brief Identifies a submitted operation for io_backend::cancel().
	 *
	 * Opaque to everyone but the backend that issued it: an io_uring backend can
	 * carry its `user_data`, an IOCP backend the address of its OVERLAPPED wrapper,
	 * a thread-pool backend a job id. 64 bits holds any of them, which is why the
	 * seam needs nothing wider. A default-constructed token is empty and converts to
	 * false; cancelling an empty token is a no-op for every backend. Copyable and
	 * trivially so.
	 */
	class io_token
	{
	public:
		/** @brief Constructs the empty token. */
		constexpr io_token() noexcept = default;

		/**
		 * @brief Wraps a backend-defined value. Zero is reserved for "empty".
		 * @param value The backend's identifier for the operation.
		 */
		explicit constexpr io_token(std::uint64_t value) noexcept: m_value(value)
		{}

		/** @brief True for a token a backend issued, false for the empty token. */
		[[nodiscard]] explicit constexpr operator bool() const noexcept
		{
			return m_value != 0;
		}

		/** @brief The backend-defined value; only the issuing backend interprets it. */
		[[nodiscard]] constexpr std::uint64_t value() const noexcept
		{
			return m_value;
		}

		/** @brief Tokens compare by value. */
		[[nodiscard]] friend constexpr bool operator==(io_token, io_token) noexcept = default;

	private:
		std::uint64_t m_value = 0;
	};

	/**
	 * @brief The seam between the awaitable file layer and a platform's I/O engine.
	 *
	 * This is the contract a thread-pool, an io_uring and an IOCP backend all
	 * implement, and it is deliberately NOT widened per platform: a submission-queue
	 * backend and a completion-port backend both fit in "submit one operation,
	 * receive one callback, cancel is a hint". If a backend needs more than this,
	 * the design is wrong, not the backend.
	 *
	 * THE CALLBACK RUNS ON AN I/O THREAD, AND MAY RUN INLINE INSIDE submit(). This
	 * layer never posts to a scheduler and never chooses a thread for the caller.
	 * That is deliberate: it keeps the seam callback-level, and the layer above
	 * (which owns a scheduler) does the posting. A caller that resumes a coroutine
	 * straight from the callback resumes it on the wrong thread.
	 */
	class io_backend
	{
	public:
		/** @brief Backends are used through this interface and deleted through it. */
		virtual ~io_backend() = default;

		/** @brief Backends are identity objects: not copyable, not movable. */
		io_backend(const io_backend&)            = delete;
		io_backend(io_backend&&)                 = delete;
		io_backend& operator=(io_backend&&)      = delete;
		io_backend& operator=(const io_backend&) = delete;

		/**
		 * @brief Submits one operation.
		 *
		 * THE CALLBACK RUNS ON AN I/O THREAD and MAY run inline inside this call,
		 * before it returns: a thread-pool arm with an idle worker, or a ring
		 * submission the kernel rejects outright, both complete that way. A caller
		 * that needs to be somewhere specific posts; this layer never does it. That
		 * includes a caller holding a lock across submit(): an inline callback runs
		 * under it.
		 *
		 * THE CALLBACK RUNS EXACTLY ONCE for every submission that returns a token.
		 * A submission that returns an error has NOT run it and never will. The
		 * layer above relies on both halves: the first so it never resumes a frame
		 * twice, the second so it never leaks one waiting on a callback that is not
		 * coming.
		 *
		 * @param request The operation. Copied before this returns; see io_request.
		 * @param on_done Invoked once with @p state and the outcome.
		 * @param state   Passed back verbatim. Must stay valid until the callback ran.
		 * @return A token for cancel(), or why the operation could not be submitted.
		 */
		[[nodiscard]] virtual std::expected<io_token, std::error_code> submit(const io_request& request,
		                                                                      io_completion_fn  on_done,
		                                                                      void*             state) = 0;

		/**
		 * @brief Asks that an operation stop early. A HINT, and nothing more.
		 *
		 * The operation's callback still runs exactly once, with whatever outcome
		 * the kernel settled on, which may be success: io_uring and IOCP both
		 * complete AFTER a cancel request rather than at it. So a cancel never makes
		 * a buffer safe to free; only the callback does. This is the contract, not a
		 * limitation. Never blocks. Cancelling an empty, unknown or already
		 * completed token is a no-op.
		 *
		 * @param token A value previously returned by submit() on this backend.
		 */
		virtual void cancel(io_token token) noexcept = 0;

		/** @brief Which implementation this is. Never io_backend_kind::automatic. */
		[[nodiscard]] virtual io_backend_kind kind() const noexcept = 0;

	protected:
		io_backend() = default;
	};

	/**
	 * @brief A backend a test drives by hand.
	 *
	 * Nothing completes until the test says so, which turns every ordering into
	 * something the test WRITES rather than hopes for: cancel, then complete, then
	 * assert. That is the point of the class. The hardest property of the file layer
	 * is that a cancelled chain must not resume while a kernel is still writing
	 * into its buffer, and it can only be proven by ordering a cancel before a
	 * completion on purpose.
	 *
	 * Operations are addressed by submission index: the first accepted submission
	 * is 0, the next 1, and an index is never reused or shifted when an earlier
	 * operation completes. Rejected submissions (fail_next_submission) take no
	 * index. Indexing by submission rather than by position in the pending set
	 * keeps request() and was_cancelled() usable after the operation completed,
	 * which is when a test wants to assert on them.
	 *
	 * Completing an operation releases the backend's copy of its buffer, as a real
	 * backend does, so a test can observe that the storage outlives or does not
	 * outlive the owners it expects. request(index).buffer is empty afterwards.
	 *
	 * Thread safety: submit(), cancel(), complete(), complete_inline_next(),
	 * fail_next_submission(), pending(), was_cancelled() and kind() are safe to call
	 * concurrently from any threads. Callbacks run outside the internal lock, so a
	 * callback may call back into the backend. request() returns a reference that
	 * stays valid for the life of the backend, but the request it names is NOT
	 * synchronized: complete() of that same index modifies it, so a test reads it
	 * (or copies its buffer) before the operation can complete on another thread.
	 *
	 * Buffer lifetime: complete() takes the stored request's buffer out of the
	 * entry before running the callback and releases that copy only after the
	 * callback returns. So the storage is alive throughout the callback even though
	 * request(index).buffer is already empty during it.
	 *
	 * Operations still pending when the backend is destroyed never run their
	 * callbacks. A test that cares completes everything first.
	 */
	class manual_io_backend final: public io_backend
	{
	public:
		/** @brief Constructs a backend with nothing submitted. */
		manual_io_backend() = default;

		manual_io_backend(const manual_io_backend&)            = delete;
		manual_io_backend& operator=(const manual_io_backend&) = delete;
		manual_io_backend(manual_io_backend&&)                 = delete;
		manual_io_backend& operator=(manual_io_backend&&)      = delete;
		~manual_io_backend() override                          = default;

		/**
		 * @brief Accepts the operation and parks it until complete().
		 *
		 * If fail_next_submission() armed an error, returns it and records nothing.
		 * Otherwise, if complete_inline_next() armed a result, the callback runs
		 * before this returns, as a real backend's inline path does. The failure arm
		 * wins when both are armed and consumes both, so the inline arm cannot fire on
		 * a later, unrelated submission.
		 *
		 * @return The token for the operation, whose value is its index plus one.
		 */
		[[nodiscard]] std::expected<io_token, std::error_code> submit(const io_request& request,
		                                                              io_completion_fn  on_done,
		                                                              void*             state) override;

		/** @brief Records the cancel request. Never completes the operation. */
		void cancel(io_token token) noexcept override;

		/** @brief Always io_backend_kind::manual. */
		[[nodiscard]] io_backend_kind kind() const noexcept override;

		/** @brief The number of accepted operations that have not completed. */
		[[nodiscard]] std::size_t pending() const;

		/**
		 * @brief Completes operation @p index, running its callback on this thread.
		 *
		 * The callback runs outside the internal lock. Completing an index twice, or
		 * one never submitted, is a test bug and asserts.
		 *
		 * @param index  Submission index of a pending operation.
		 * @param result What the callback is given. Delivered as is, whether or not the
		 *               operation was cancelled: a cancel does not change the outcome.
		 */
		void complete(std::size_t index, io_completion result);

		/**
		 * @brief Arms the NEXT submit() to complete before it returns.
		 *
		 * One-shot. Models the inline path a backend may take; the operation never
		 * appears as pending.
		 */
		void complete_inline_next(io_completion result);

		/**
		 * @brief Arms the NEXT submit() to be rejected with @p error. One-shot.
		 *
		 * The callback of a rejected submission never runs.
		 */
		void fail_next_submission(std::error_code error);

		/** @brief Whether cancel() was called for operation @p index, completed or not. */
		[[nodiscard]] bool was_cancelled(std::size_t index) const;

		/**
		 * @brief The request submitted as operation @p index.
		 *
		 * Lets a test reach the request's buffer and write into it, which is how a
		 * test stands in for a backend's late write. After completion the buffer is
		 * empty; see the class description. A test that needs the buffer at or after
		 * completion (inside the callback, say) must take its own io_buffer copy
		 * BEFORE calling complete().
		 */
		[[nodiscard]] const io_request& request(std::size_t index) const;

	private:
		struct entry
		{
			io_request       request;
			io_completion_fn on_done   = nullptr;
			void*            state     = nullptr;
			bool             cancelled = false;
			bool             completed = false;
		};

		mutable std::mutex             m_mutex;
		std::deque<entry>              m_entries; // deque: push_back never invalidates references
		std::size_t                    m_pending = 0;
		std::optional<io_completion>   m_inline_next;
		std::optional<std::error_code> m_fail_next;
	};

	/**
	 * @brief Makes the backend that performs every operation as a blocking syscall on a pool thread.
	 *
	 * This is the one backend every platform has, and it stays in use even where
	 * io_uring or IOCP exists: open, stat, rename, unlink and list_directory have no
	 * asynchronous form on Windows, and a submission queue has nowhere to run
	 * arbitrary code.
	 *
	 * The pool is BORROWED, never owned. The caller keeps it alive until every
	 * submitted operation's callback has run, and must not destroy it from one of its
	 * own workers, which would join itself. Destroying the returned backend while
	 * operations are queued is safe: each job carries what it needs to finish.
	 *
	 * Cancel is a hint with detach semantics. A job that has not started finds itself
	 * cancelled and completes with std::errc::operation_canceled WITHOUT doing the
	 * I/O; a job already running runs to completion and reports its real result.
	 * Either way the callback runs exactly once, on a pool thread, and the job's
	 * io_buffer copy is held until it returns, then released on the worker.
	 *
	 * @param pool The pool whose threads run the syscalls.
	 */
	[[nodiscard]] std::unique_ptr<io_backend> make_thread_pool_backend(thread_pool& pool);

} // namespace alt

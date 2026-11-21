#pragma once

#ifdef __linux__

#	include <alt/io_backend.hpp>
#	include <alt/thread_pool.hpp>
#	include <cstddef>
#	include <cstdint>
#	include <expected>
#	include <memory>
#	include <system_error>

/**
 * The io_uring backend. Private to the library: only file_io::create builds one, and the tests
 * include this header to reach it. A consumer has no reason to, and cannot through
 * include/alt/, which never names liburing.h.
 */
namespace alt::detail
{
	/// Set on every token for a ring operation; a token without it came from the internal pool backend.
	inline constexpr std::uint64_t io_uring_token_tag = std::uint64_t{1} << 63U;

	/**
	 * Builds the backend that performs transfers through an io_uring submission ring, and every
	 * other operation on @p pool, which is borrowed.
	 *
	 * It owns the ring and the one thread that reaps completions from it; a completion's callback
	 * runs on that thread, or on a pool worker for an operation forwarded there.
	 *
	 * **Unlike the IOCP backend, this one needs nothing special of a descriptor.** io_uring
	 * operates on ordinary file descriptors, so the internal pool backend is built with the plain
	 * alt::make_thread_pool_backend(): no detail::pool_open_policy, no prepare hook, and no extra
	 * open flags. A reader arriving from src/io_backend_iocp.cpp will look for that machinery and
	 * will not find it, because IOCP needs FILE_FLAG_OVERLAPPED and association with its port
	 * while a ring needs neither. Descriptors that arrived from outside, through
	 * alt::async_file::adopt(), are as usable here as ones this service opened itself.
	 *
	 * @p queue_depth is the submission ring size, or 0 to derive it from @p max_in_flight; it is
	 * rounded up to a power of two and clamped to what the kernel allows. The completion ring is
	 * sized separately from @p max_in_flight, so completion-queue overflow is unreachable by
	 * construction whatever depth the caller asks for. The two bound different rings, which is why
	 * both are parameters.
	 *
	 * Creating a ring is not evidence that this backend can work: a 5.1 to 5.5 kernel creates one
	 * and then fails every transfer, because IORING_OP_READ and IORING_OP_WRITE arrived in 5.6 and
	 * IORING_OP_ASYNC_CANCEL in 5.5. So the running kernel is probed once, all-or-nothing, and a
	 * missing opcode fails this call with std::errc::not_supported.
	 *
	 * Destroying it with ANY operation in flight is a precondition violation: a ring operation, or
	 * an operation forwarded to the pool whose callback has not yet run. It asserts, and under
	 * NDEBUG terminates, rather than free a record the kernel may still own. Unlike the
	 * thread-pool backend, this one is NOT safe to destroy with operations queued; file_io never
	 * does.
	 *
	 * @return The backend, or why the ring, its capabilities or its thread would not do.
	 */
	[[nodiscard]] std::expected<std::unique_ptr<io_backend>, std::error_code> make_io_uring_backend(thread_pool& pool,
	                                                                                                std::size_t  queue_depth,
	                                                                                                std::size_t  max_in_flight);

	/// The submission ring entries @p ring was created with. Precondition: @p ring was built by make_io_uring_backend().
	[[nodiscard]] std::size_t io_uring_submission_depth_for_testing(const io_backend& ring) noexcept;

	/// The completion ring entries @p ring was created with. Same precondition.
	[[nodiscard]] std::size_t io_uring_completion_depth_for_testing(const io_backend& ring) noexcept;

	/// How many completions the reaper thread found no operation for and ignored. Same precondition.
	[[nodiscard]] std::size_t io_uring_ignored_completions_for_testing(const io_backend& ring) noexcept;
} // namespace alt::detail

#endif

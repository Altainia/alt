#pragma once

#include <algorithm>
#include <alt/detail/file_types.hpp>
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <alt/thread_pool.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>

#ifdef __linux__
#	include "io_backend_io_uring.hpp"
#endif

namespace alt::detail
{
	/**
	 * How a thread-pool backend's io_op::open differs from the default.
	 *
	 * Private to the library: the IOCP backend builds on a thread-pool backend for everything but
	 * the handles it opens, and tests include this header to exercise the seam.
	 */
	struct pool_open_policy
	{
		/// OR'd into CreateFileW's flags and attributes. Unused off Windows.
		std::uint32_t extra_flags = 0;
		/// Called with each newly opened handle. An error closes the handle and fails the open with it.
		std::error_code (*prepare)(void* context, native_handle handle) noexcept = nullptr;
		/// Passed to @ref prepare unchanged.
		void* context = nullptr;
	};

	/// A thread-pool backend whose io_op::open follows @p policy; otherwise identical to alt::make_thread_pool_backend().
	std::unique_ptr<io_backend> make_thread_pool_backend(thread_pool& pool, const pool_open_policy& policy);

	/**
	 * The byte count a positional transfer may move: never more than the buffer holds.
	 *
	 * This is the platform-neutral clamp only, and every arm applies a cap of its own on top of it:
	 * the Windows arm a DWORD clamp, and both Linux arms the kernel's MAX_RW_COUNT of 0x7ffff000 --
	 * explicitly in the io_uring arm's max_single_transfer, and by the kernel's own truncation of
	 * pread and pwrite in the thread-pool arm.
	 */
	inline std::size_t transfer_size(const io_request& request) noexcept
	{
		return std::min(request.size, request.buffer.size());
	}

#ifdef __linux__
	/// What builds an io_uring backend: make_io_uring_backend(), or a stand-in a test injects.
	using io_uring_factory = std::expected<std::unique_ptr<io_backend>, std::error_code> (*)(thread_pool& pool, std::size_t queue_depth, std::size_t max_in_flight);

	/**
	 * Linux' `automatic`: io_uring, or the thread pool if @p make_ring fails.
	 *
	 * Any ring failure falls back, because `automatic` promises the best backend that can run:
	 * `ENOSYS` where the kernel has no io_uring, `EPERM` where `kernel.io_uring_disabled` or a
	 * seccomp filter forbids it (common in containers), and `not_supported` where the kernel's
	 * opcodes fall short. file_io::backend() reports the fallback. @p make_ring is a parameter so a
	 * test can pass one that fails on a machine that has io_uring.
	 *
	 * @p queue_depth and @p max_in_flight are both std::size_t and are passed to @p make_ring in
	 * that order; nothing but the order tells them apart.
	 */
	std::expected<std::unique_ptr<io_backend>, std::error_code> automatic_on_linux(thread_pool& workers, std::size_t queue_depth, std::size_t max_in_flight, io_uring_factory make_ring = &make_io_uring_backend);

	/**
	 * Resolves @p options.backend to a backend this build provides, and builds it.
	 *
	 * Takes the options whole rather than their two std::size_t fields, so create() has no pair of
	 * adjacent integers to transpose. An explicit `io_uring` request fails with @p make_ring's own
	 * error; `automatic` falls back as automatic_on_linux() does.
	 */
	std::expected<std::unique_ptr<io_backend>, std::error_code> probe_backend(thread_pool& workers, const file_io_options& options, io_uring_factory make_ring = &make_io_uring_backend);
#else
	/// Resolves @p options.backend to a backend this build provides, and builds it.
	std::expected<std::unique_ptr<io_backend>, std::error_code> probe_backend(thread_pool& workers, const file_io_options& options);
#endif

#ifndef _WIN32
	/**
	 * An errno value as an error_code in the system category.
	 *
	 * For a caller that already holds the value, such as a completion-queue consumer that has negated a
	 * negative result. @p errno_value is a positive errno value, not a negative result.
	 */
	inline std::error_code errno_error(const int errno_value) noexcept
	{
		return {errno_value, std::system_category()};
	}

	/**
	 * The current errno as an error_code in the system category.
	 *
	 * Reads errno at call time, so call it before anything can clobber it. A caller that already holds an
	 * errno value passes it to errno_error(int) instead.
	 */
	inline std::error_code errno_error() noexcept
	{
		return errno_error(errno);
	}
#endif
} // namespace alt::detail

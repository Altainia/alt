#pragma once

#include <algorithm>
#include <alt/detail/file_types.hpp>
#include <alt/io_backend.hpp>
#include <alt/thread_pool.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

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
	 * This is the platform-neutral clamp only. The Windows arm applies its own DWORD clamp on top of it.
	 */
	inline std::size_t transfer_size(const io_request& request) noexcept
	{
		return std::min(request.size, request.buffer.size());
	}

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

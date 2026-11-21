#pragma once

#ifdef _WIN32

#	include <alt/io_backend.hpp>
#	include <alt/thread_pool.hpp>
#	include <cstddef>
#	include <cstdint>
#	include <expected>
#	include <memory>
#	include <system_error>

#	include "io_windows.hpp"

/**
 * The IOCP backend. Private to the library: only file_io::create builds one, and the tests
 * include this header to reach it. A consumer has no reason to, and cannot through
 * include/alt/, which never names windows.h.
 */
namespace alt::detail
{
	/// Set on every token for an IOCP transfer; a token without it came from the internal pool backend.
	inline constexpr std::uint64_t iocp_token_tag = std::uint64_t{1} << 63U;

	/**
	 * Builds the backend that performs reads and writes as overlapped I/O completed through a
	 * completion port, and every other operation on @p pool, which is borrowed.
	 *
	 * It owns the port and the one thread that waits on it; every transfer's callback runs on
	 * that thread, or inline inside submit() for a transfer that completed or failed
	 * synchronously. Its open adds FILE_FLAG_OVERLAPPED and associates the new handle with the
	 * port, so its transfers are valid only on handles it opened itself.
	 *
	 * Destroying it with ANY operation in flight is a precondition violation: a transfer, or an
	 * operation forwarded to the pool whose callback has not yet run. It asserts, and under NDEBUG
	 * terminates, rather than free a record the kernel may still own, or let a queued open
	 * associate its handle with a port that is already closed. Unlike the thread-pool backend,
	 * this one is NOT safe to destroy with operations queued; file_io never does.
	 *
	 * @return The backend, or why the port or its thread could not be created.
	 */
	[[nodiscard]] std::expected<std::unique_ptr<io_backend>, std::error_code> make_iocp_backend(thread_pool& pool);

	/// The completion port of @p iocp. Precondition: @p iocp was built by make_iocp_backend().
	[[nodiscard]] HANDLE iocp_port_for_testing(io_backend& iocp) noexcept;

	/// How many packets the completion thread found no record for and ignored. Same precondition.
	[[nodiscard]] std::size_t iocp_ignored_packets_for_testing(io_backend& iocp) noexcept;
} // namespace alt::detail

#endif

#pragma once

#ifdef _WIN32

#	include <alt/detail/file_types.hpp>
#	include <alt/io_backend.hpp>
#	include <system_error>

// windows.h defines min and max as function-like macros, which break std::min and
// std::numeric_limits<>::max(). NOMINMAX is its documented switch to leave them out.
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>

/// The Win32 primitives shared by the I/O backends. Private to the library.
namespace alt::detail::windows_io
{
	/**
	 * Translates the Win32 errors whose standard mapping differs from the POSIX error the
	 * documented contract promises, and leaves every other error as the system reports it.
	 *
	 * Translated: ERROR_DIRECTORY to not_a_directory, and ERROR_NOT_SAME_DEVICE to
	 * cross_device_link. A code not listed arrives faithfully as {code, system_category()}.
	 */
	std::error_code normalize_error(std::error_code error) noexcept;

	/// The calling thread's last Win32 error, normalized.
	std::error_code last_error() noexcept;

	HANDLE to_handle(native_handle handle) noexcept;

	/// Whether @p handle names a directory.
	bool is_directory_handle(native_handle handle) noexcept;

	/**
	 * Opens @p args.path as the open flags ask, with @p extra_flags OR'd into CreateFileW's flags
	 * and attributes. Writes @p out only on success.
	 */
	std::error_code open_file(const io_open_args& args, native_handle& out, DWORD extra_flags) noexcept;
} // namespace alt::detail::windows_io

#endif

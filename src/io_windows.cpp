#ifdef _WIN32

#	include "io_windows.hpp"

#	include <alt/detail/file_types.hpp>
#	include <alt/io_backend.hpp>
#	include <system_error>

namespace alt::detail::windows_io
{
	std::error_code normalize_error(std::error_code error) noexcept
	{
		if(error.category() != std::system_category())
		{
			return error;
		}
		switch(static_cast<DWORD>(error.value()))
		{
		case ERROR_DIRECTORY:
			return std::make_error_code(std::errc::not_a_directory);
		case ERROR_NOT_SAME_DEVICE:
			return std::make_error_code(std::errc::cross_device_link);
		default:
			return error;
		}
	}

	std::error_code last_error() noexcept
	{
		return normalize_error({static_cast<int>(::GetLastError()), std::system_category()});
	}

	HANDLE to_handle(native_handle handle) noexcept
	{
		return reinterpret_cast<HANDLE>(handle);
	}

	bool is_directory_handle(native_handle handle) noexcept
	{
		BY_HANDLE_FILE_INFORMATION info{};
		return ::GetFileInformationByHandle(to_handle(handle), &info) != FALSE && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	std::error_code open_file(const io_open_args& args, native_handle& out, const DWORD extra_flags) noexcept
	{
		const auto how    = args.open_flags;
		const bool reads  = how.has_any(flags(open_flag::read));
		const bool writes = how.has_any(flags(open_flag::write));
		DWORD      access = 0;
		if(reads)
		{
			access |= GENERIC_READ;
		}
		if(writes)
		{
			// Append-only access is what makes every write land at the end.
			access |= how.has_any(flags(open_flag::append)) ? static_cast<DWORD>(FILE_APPEND_DATA) : static_cast<DWORD>(GENERIC_WRITE);
		}
		const bool creates     = how.has_any(flags(open_flag::create));
		const bool truncates   = how.has_any(flags(open_flag::truncate));
		DWORD      disposition = OPEN_EXISTING;
		if(creates && how.has_any(flags(open_flag::exclusive)))
		{
			disposition = CREATE_NEW;
		}
		else if(creates)
		{
			disposition = truncates ? CREATE_ALWAYS : OPEN_ALWAYS;
		}
		else if(truncates)
		{
			disposition = TRUNCATE_EXISTING;
		}
		SECURITY_ATTRIBUTES security{};
		security.nLength        = static_cast<DWORD>(sizeof(security));
		security.bInheritHandle = how.has_any(flags(open_flag::close_on_exec)) ? FALSE : TRUE;
		// FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFileW open a directory at all.
		const HANDLE handle = ::CreateFileW(args.path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | extra_flags, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
		{
			return last_error();
		}
		out = reinterpret_cast<native_handle>(handle);
		return {};
	}
} // namespace alt::detail::windows_io

#endif

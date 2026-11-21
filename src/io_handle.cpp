#include <alt/detail/io_handle.hpp>

#ifdef _WIN32
// Keep windows.h from defining min and max as macros; see io_backend_thread_pool.cpp.
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <unistd.h>
#endif

namespace alt::detail
{

	std::shared_ptr<io_handle> io_handle::adopt(native_handle handle, const handle_origin origin)
	{
		return std::make_shared<io_handle>(private_token{}, handle, origin);
	}

	io_handle::io_handle(private_token /*token*/, native_handle handle, const handle_origin origin) noexcept: m_handle(handle), m_origin(origin)
	{}

	io_handle::~io_handle()
	{
		if(m_handle == invalid_native_handle)
		{
			return;
		}
#ifdef _WIN32
		::CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#else
		::close(static_cast<int>(m_handle));
#endif
	}

}

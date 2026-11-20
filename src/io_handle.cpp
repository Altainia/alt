#include <alt/detail/io_handle.hpp>

#ifdef _WIN32
#	include <windows.h>
#else
#	include <unistd.h>
#endif

namespace alt::detail
{

	std::shared_ptr<io_handle> io_handle::adopt(native_handle handle)
	{
		return std::make_shared<io_handle>(private_token{}, handle);
	}

	io_handle::io_handle(private_token /*token*/, native_handle handle) noexcept: m_handle(handle)
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

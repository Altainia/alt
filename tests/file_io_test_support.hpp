#pragma once

#include <gtest/gtest.h>

#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>

namespace alt_test
{

	/**
	 * The value of the environment variable @p name, or nothing when it is unset.
	 *
	 * MSVC deprecates std::getenv (C4996) in favour of _dupenv_s, which copies the value
	 * into a buffer the caller frees; that is used there, and std::getenv everywhere else.
	 */
	inline std::optional<std::string> environment_variable(const char* name)
	{
#ifdef _MSC_VER
		char*       buffer = nullptr;
		std::size_t length = 0;
		if(_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
		{
			return std::nullopt;
		}
		std::string value{buffer};
		std::free(buffer);
		return value;
#else
		const char* const value = std::getenv(name);
		if(value == nullptr)
		{
			return std::nullopt;
		}
		return std::string{value};
#endif
	}

	/**
	 * @p base with its backend chosen by the test run rather than by the fixture.
	 *
	 * The file-layer fixtures build their service through this, so one suite runs against
	 * every built-in backend: `ALT_TEST_IO_BACKEND=iocp` selects io_backend_kind::iocp,
	 * `ALT_TEST_IO_BACKEND=thread_pool` selects io_backend_kind::thread_pool, and with the
	 * variable unset @p base is returned unchanged. Any other value is a mistake in how
	 * the suite was launched, reported as a test failure, and @p base is returned so the
	 * fixture still has a service to tear down.
	 *
	 * Tests that are about one particular backend name it themselves and do not call this.
	 */
	inline alt::file_io_options file_io_options_under_test(alt::file_io_options base = {})
	{
		const std::optional<std::string> requested = environment_variable("ALT_TEST_IO_BACKEND");
		if(!requested.has_value())
		{
			return base;
		}
		if(*requested == "iocp")
		{
			base.backend = alt::io_backend_kind::iocp;
		}
		else if(*requested == "thread_pool")
		{
			base.backend = alt::io_backend_kind::thread_pool;
		}
		else
		{
			ADD_FAILURE() << "ALT_TEST_IO_BACKEND is \"" << *requested << "\"; expected \"iocp\", \"thread_pool\" or unset";
		}
		return base;
	}

}

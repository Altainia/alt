#pragma once

#include <gtest/gtest.h>

#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <unistd.h>
#endif

#ifdef __linux__
#	include <alt/thread_pool.hpp>
#	include <expected>

#	include "io_backend_io_uring.hpp"
#endif

namespace alt_test
{

	/**
	 * This process's id.
	 *
	 * For a scratch path a fixture builds. An atomic counter alone is NOT enough: `ctest -j` runs
	 * each test as its own process, so every process starts its counter at the same value, and a
	 * `this` pointer is no better -- two processes running the same binary readily place a fixture
	 * at the same address. Either one on its own lets two concurrent test processes compute the same
	 * directory and delete each other's files, which is issue #76. The counter distinguishes
	 * fixtures within a process and this distinguishes the processes.
	 */
	inline unsigned long process_id() noexcept
	{
#ifdef _WIN32
		return ::GetCurrentProcessId();
#else
		return static_cast<unsigned long>(::getpid());
#endif
	}

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
	 * `ALT_TEST_IO_BACKEND=io_uring` selects io_backend_kind::io_uring,
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
		else if(*requested == "io_uring")
		{
			base.backend = alt::io_backend_kind::io_uring;
		}
		else if(*requested == "thread_pool")
		{
			base.backend = alt::io_backend_kind::thread_pool;
		}
		else
		{
			ADD_FAILURE() << "ALT_TEST_IO_BACKEND is \"" << *requested << "\"; expected \"iocp\", \"io_uring\", \"thread_pool\" or unset";
		}
		return base;
	}

#ifdef __linux__
	/**
	 * Whether @p error says this environment forbids io_uring, as opposed to saying the code
	 * under test made a wrong decision.
	 *
	 * The refusals a container or an old kernel gives are EPERM (`kernel.io_uring_disabled`, or a
	 * seccomp filter), EACCES (a restricted `io_uring_group`), ENOSYS (no io_uring in the kernel)
	 * and ENOTSUP (the kernel lacks an opcode the backend needs). Anything else, notably EINVAL
	 * from a sizing mistake or ENOMEM, is a verdict on this library and is never environmental.
	 * ENOMEM is a deliberate choice: a low RLIMIT_MEMLOCK on an old kernel can produce it from the
	 * environment, but it is also what the backend reports for a failed allocation, so it fails
	 * the tests (and the canary) rather than skipping them.
	 */
	inline bool io_uring_refusal_is_environmental(const std::error_code& error) noexcept
	{
		return error == std::errc::operation_not_permitted || error == std::errc::permission_denied ||
		       error == std::errc::function_not_supported || error == std::errc::not_supported;
	}

	/**
	 * Why this machine cannot run io_uring tests, or nothing when it can.
	 *
	 * For a test that is about something other than io_uring's availability and should skip where
	 * a ring cannot be made. It asks the instrument the code under test uses,
	 * make_io_uring_backend(), rather than a syscall of its own, so "available" means "the backend
	 * can be built", and it reports a reason ONLY for an environmental refusal. A ring that fails
	 * for any other reason yields nothing: the test then runs and fails on its own assertions
	 * instead of hiding behind a skip. The one test that fails when the answer is "unavailable" is
	 * IoUringAvailability.TheRunnerProvidesIoUring, so a runner that stops permitting io_uring is
	 * still a red build.
	 */
	inline std::optional<std::string> io_uring_unavailable_reason()
	{
		auto pool = alt::thread_pool::create(1);
		if(!pool.has_value())
		{
			return std::nullopt;
		}
		const alt::file_io_options defaults;
		const auto                 made = alt::detail::make_io_uring_backend(*pool, defaults.queue_depth, defaults.max_in_flight);
		if(made.has_value() || !io_uring_refusal_is_environmental(made.error()))
		{
			return std::nullopt;
		}
		return "this environment does not permit io_uring: " + made.error().message();
	}
#endif

}

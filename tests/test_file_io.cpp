#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

namespace
{

	using alt::detail::io_awaiter;
	using alt::detail::io_handle;
	using alt::detail::io_shared;

	/** What one awaiting chain reports back to the test that started it. */
	struct op_record
	{
		bool slot_acquired = false;
		bool body_ran      = false;
	};

	/** A read request over @p buffer, with every field spelled out. */
	alt::io_request read_request(const alt::io_buffer& buffer)
	{
		return alt::io_request{
		  .op     = alt::io_op::read,
		  .handle = alt::invalid_native_handle,
		  .buffer = buffer,
		  .offset = 0,
		  .size   = buffer.size(),
		  .in     = nullptr,
		  .out    = nullptr,
		};
	}

	/** The two-phase operation: a backpressure permit, then the submitted phase. */
	alt::cancellable_task awaiting_chain(std::shared_ptr<io_shared> shared, alt::io_request request, std::shared_ptr<op_record> record)
	{
		const alt::wait_result slot = co_await shared->acquire_slot();
		if(!slot)
		{
			co_return;
		}
		record->slot_acquired = true;

		static_cast<void>(co_await io_awaiter{shared, std::move(request), nullptr});
		record->body_ran = true;
	}

	/** Starts a pool, which every test here needs to name and none of them uses. */
	alt::thread_pool& pool_of(std::expected<alt::thread_pool, std::error_code>& created)
	{
		EXPECT_TRUE(created.has_value());
		return *created;
	}

	/**
	 * Completes operation @p index from another thread, but only once @p destroying is
	 * set and a little longer after that, so the destructor under test is already
	 * blocked on the backend when the completion lands. Lifted out of the test body it
	 * serves, where the lambda's nesting pushes that body past clang-tidy's
	 * cognitive-complexity threshold.
	 */
	std::jthread complete_once_destroying(alt::manual_io_backend& backend, const std::atomic<bool>& destroying, const std::size_t index)
	{
		return std::jthread{[&backend, &destroying, index] {
			while(!destroying.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{20});
			backend.complete(index, alt::io_completion{.bytes = 16, .error = {}});
		}};
	}

	/**
	 * The scenario the death test runs in its child: one operation holds the only permit
	 * at the backend, a second is parked behind it, and the service is destroyed.
	 */
	void destroy_a_service_with_an_operation_parked()
	{
		auto                   created = alt::thread_pool::create(1);
		alt::thread_pool&      pool    = pool_of(created);
		alt::manual_io_backend backend;
		alt::manual_scheduler  home;
		const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

		const alt::file_io_options options{.backend = alt::io_backend_kind::automatic, .max_in_flight = 1, .queue_depth = 0};

		{
			const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend, options);

			const std::shared_ptr<op_record> first  = std::make_shared<op_record>();
			const std::shared_ptr<op_record> second = std::make_shared<op_record>();

			const alt::cancellation_handle chain_a = alt::start(awaiting_chain(io->shared_state(), read_request(buffer), first));
			const alt::cancellation_handle chain_b = alt::start(awaiting_chain(io->shared_state(), read_request(buffer), second));

			// Destroyed at the end of this scope with the second operation still parked: the violation.
		}
	}

}

TEST(FileIoTest, CreateSucceedsAndReportsTheThreadPoolBackend)
{
	auto                  created = alt::thread_pool::create(1);
	alt::thread_pool&     pool    = pool_of(created);
	alt::manual_scheduler home;

	const auto io = alt::file_io::create(pool, alt::scheduler_ref{home});

	ASSERT_TRUE(io.has_value());
	EXPECT_EQ(io->backend(), alt::io_backend_kind::thread_pool);
}

TEST(FileIoTest, CreateWithAnExplicitBackendReportsThatBackendsKind)
{
	auto                  created = alt::thread_pool::create(1);
	alt::thread_pool&     pool    = pool_of(created);
	alt::manual_scheduler home;

	const auto named = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::thread_pool});
	ASSERT_TRUE(named.has_value());
	EXPECT_EQ(named->backend(), alt::io_backend_kind::thread_pool);

	alt::manual_io_backend manual;
	const auto             injected = alt::file_io::create(pool, alt::scheduler_ref{home}, manual);
	ASSERT_TRUE(injected.has_value());
	EXPECT_EQ(injected->backend(), alt::io_backend_kind::manual);

	// A kind this build cannot provide is an error, not a silent substitution.
	const auto uring = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::io_uring});
	ASSERT_FALSE(uring.has_value());
	EXPECT_EQ(uring.error(), std::errc::not_supported);

	// manual cannot be conjured from an enumerator.
	const auto conjured = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::manual});
	ASSERT_FALSE(conjured.has_value());
	EXPECT_EQ(conjured.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, CreateRejectsAZeroMaxInFlight)
{
	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_scheduler  home;
	alt::manual_io_backend manual;

	// A zero bound would park the first operation on a permit that can never exist.
	const auto built_in = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.max_in_flight = 0});
	ASSERT_FALSE(built_in.has_value());
	EXPECT_EQ(built_in.error(), std::errc::invalid_argument);

	const auto injected = alt::file_io::create(pool, alt::scheduler_ref{home}, manual, alt::file_io_options{.max_in_flight = 0});
	ASSERT_FALSE(injected.has_value());
	EXPECT_EQ(injected.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, CreateRejectsAnEmptyCompletionScheduler)
{
	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend manual;

	// Reported here, not asserted inside scheduler_ref::post on an I/O thread.
	const auto built_in = alt::file_io::create(pool, alt::scheduler_ref{});
	ASSERT_FALSE(built_in.has_value());
	EXPECT_EQ(built_in.error(), std::errc::invalid_argument);

	const auto injected = alt::file_io::create(pool, alt::scheduler_ref{}, manual);
	ASSERT_FALSE(injected.has_value());
	EXPECT_EQ(injected.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, TheDestructorReturnsOnceTheBackendIsQuietEvenWithResumptionsQueued)
{
	alt_test::reset_coroutine_test_state();

	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> finished  = std::make_shared<op_record>();
	const std::shared_ptr<op_record> straggler = std::make_shared<op_record>();

	std::atomic<bool> destroying{false};
	std::jthread      late_completer;

	{
		const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend);
		ASSERT_TRUE(io.has_value());

		const alt::cancellation_handle chain_a = alt::start(awaiting_chain(io->shared_state(), read_request(buffer), finished));
		const alt::cancellation_handle chain_b = alt::start(awaiting_chain(io->shared_state(), read_request(buffer), straggler));
		ASSERT_EQ(backend.pending(), 2U);

		// One completion lands now, so its resumption is queued on a scheduler nobody
		// pumps. Were the destructor to wait for that to run, it would never return.
		backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
		ASSERT_EQ(backend.pending(), 1U);
		ASSERT_FALSE(finished->body_ran);

		// The other completes only once the destructor is under way, from another
		// thread, so the destructor has to wait for it.
		late_completer = complete_once_destroying(backend, destroying, 1);

		destroying.store(true, std::memory_order_release);
	}

	// Only reachable once the destructor returned, and the backend had to be quiet for
	// that: operation 1 is gone from it.
	EXPECT_EQ(backend.pending(), 0U);

	// And the resumptions are still waiting, unrun, with nobody having pumped them.
	EXPECT_FALSE(finished->body_ran);
	EXPECT_FALSE(straggler->body_ran);

	EXPECT_EQ(home.drain(), 2U);
	EXPECT_TRUE(finished->body_ran);
	EXPECT_TRUE(straggler->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(FileIoTest, AQueuedResumptionRunSafelyAfterTheDestructorReturned)
{
	alt_test::reset_coroutine_test_state();

	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();

	{
		const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend);
		ASSERT_TRUE(io.has_value());

		const alt::cancellation_handle chain = alt::start(awaiting_chain(io->shared_state(), read_request(buffer), record));
		ASSERT_EQ(backend.pending(), 1U);

		// Completed at the backend, so the resumption is queued and unpumped when the
		// service goes out of scope.
		backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
		ASSERT_FALSE(record->body_ran);
	}

	// The handle is gone. The resumption releases its permit against io_shared, which
	// only the operation block's own reference is keeping alive now: that is what the
	// refcounted state exists for, and AddressSanitizer is the instrument that reports
	// the release if it touches freed memory.
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body. See the death tests in test_waiter_list.cpp, whose suppression carries the measurement.
TEST(FileIoDeathTest, DestroyingAFileIoWithAnOperationParkedOnASlotAborts)
{
	if(!alt_test::asserts_are_compiled_in)
	{
		GTEST_SKIP() << "NDEBUG compiles out the assert in ~file_io that this test observes.";
	}

	if(alt_test::thread_sanitizer_is_active)
	{
		GTEST_SKIP() << alt_test::thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const alt_test::scoped_death_test_style style{"threadsafe"};

	EXPECT_DEATH(destroy_a_service_with_an_operation_parked(), "parked on a backpressure slot");
}

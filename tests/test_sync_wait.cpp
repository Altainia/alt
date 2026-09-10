#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace
{
	alt::task<int> answer()
	{
		co_return 42;
	}
	alt::task<void> nothing()
	{
		co_return;
	}

	alt::task<std::unique_ptr<int>> owned()
	{
		co_return std::make_unique<int>(5);
	}

	alt::task<int> thrower()
	{
		throw std::runtime_error{"boom"};
		co_return 0;
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): subject is owned by the calling test, which reaches sync_wait and blocks until this coroutine completes, so the reference cannot dangle.
	alt::task<std::string&> borrow(std::string& subject)
	{
		co_return subject;
	}

	// Hops onto the scheduler and back, so it cannot complete without pumping.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): scheduler is owned by the calling test, which reaches sync_wait and blocks or pumps until this coroutine completes, so the reference cannot dangle.
	alt::task<int> hops(alt::manual_scheduler& scheduler)
	{
		co_await alt::resume_on(scheduler);
		co_return 7;
	}
}

TEST(SyncWait, ReturnsAValue)
{
	EXPECT_EQ(alt::sync_wait(answer()), 42);
}

TEST(SyncWait, ReturnTypeIsAValueNotAnRvalueReference)
{
	static_assert(std::is_same_v<decltype(alt::sync_wait(answer())), int>);
}

TEST(SyncWait, HandlesAVoidResult)
{
	EXPECT_NO_THROW(alt::sync_wait(nothing()));
}

TEST(SyncWait, RethrowsAnExceptionFromTheBody)
{
	EXPECT_THROW(alt::sync_wait(thrower()), std::runtime_error);
}

TEST(SyncWait, MovesAMoveOnlyResultOut)
{
	const auto taken = alt::sync_wait(owned());
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 5);
}

TEST(SyncWait, PreservesAReferenceResult)
{
	std::string subject{"hello"};
	auto&       observed = alt::sync_wait(borrow(subject));
	EXPECT_EQ(&observed, &subject);
}

TEST(SyncWait, BlockingOverloadWaitsForAnotherThread)
{
	alt::manual_scheduler scheduler;

	// A second thread pumps, so parking is correct here rather than deadlocking.
	std::jthread pump{[&] {
		while(scheduler.run_one_or_wait())
		{
		}
	}};

	EXPECT_EQ(alt::sync_wait(hops(scheduler)), 7);
	scheduler.wake();
}

TEST(SyncWait, DrivingOverloadPumpsWithoutASecondThread)
{
	// The single-threaded case. The blocking overload would deadlock here.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(hops(scheduler), scheduler), 7);
	EXPECT_TRUE(scheduler.empty());
}

TEST(SyncWait, DrivingOverloadRethrows)
{
	alt::manual_scheduler scheduler;
	EXPECT_THROW(alt::sync_wait(thrower(), scheduler), std::runtime_error);
}

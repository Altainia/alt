#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
	// A work_item that appends its tag to a shared log.
	struct logged_call
	{
		std::vector<int>* log;
		int               tag;
	};

	void log_trampoline(void* state)
	{
		auto* const call = static_cast<logged_call*>(state);
		call->log->push_back(call->tag);
	}

	alt::work_item item_for(logged_call& call) noexcept
	{
		return alt::work_item{.invoke = &log_trampoline, .state = &call};
	}
}

TEST(SchedulerConcepts, ManualAndTrampolineAreSchedulers)
{
	static_assert(alt::scheduler<alt::manual_scheduler>);
	static_assert(alt::scheduler<alt::trampoline_scheduler>);
	static_assert(alt::drivable_scheduler<alt::manual_scheduler>);
	static_assert(!alt::drivable_scheduler<alt::trampoline_scheduler>);
}

TEST(ManualScheduler, RunsNothingUntilPumped)
{
	std::vector<int>      log;
	logged_call           call{.log = &log, .tag = 1};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(call));
	EXPECT_TRUE(log.empty());
	EXPECT_FALSE(scheduler.empty());

	EXPECT_TRUE(scheduler.run_one());
	EXPECT_EQ(log, std::vector<int>{1});
	EXPECT_TRUE(scheduler.empty());
	EXPECT_FALSE(scheduler.run_one());
}

TEST(ManualScheduler, RunsInPostOrder)
{
	std::vector<int>      log;
	logged_call           first{.log = &log, .tag = 1};
	logged_call           second{.log = &log, .tag = 2};
	logged_call           third{.log = &log, .tag = 3};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(first));
	scheduler.post(item_for(second));
	scheduler.post(item_for(third));

	EXPECT_EQ(scheduler.run_all(), 3U);
	EXPECT_EQ(log, (std::vector<int>{1, 2, 3}));
}

TEST(ManualScheduler, AcceptsPostsFromAnotherThread)
{
	std::vector<int>      log;
	logged_call           call{.log = &log, .tag = 9};
	alt::manual_scheduler scheduler;

	std::thread poster{[&] { scheduler.post(item_for(call)); }};
	poster.join();

	EXPECT_EQ(scheduler.run_all(), 1U);
	EXPECT_EQ(log, std::vector<int>{9});
}

TEST(ManualScheduler, RunOneOrWaitBlocksUntilAPostArrives)
{
	std::vector<int>      log;
	logged_call           call{.log = &log, .tag = 4};
	alt::manual_scheduler scheduler;

	std::thread poster{[&] { scheduler.post(item_for(call)); }};
	EXPECT_TRUE(scheduler.run_one_or_wait());
	poster.join();
	EXPECT_EQ(log, std::vector<int>{4});
}

TEST(ManualScheduler, WakeReleasesABlockedPumpWithoutRunningAnything)
{
	alt::manual_scheduler scheduler;
	std::atomic<bool>     returned{false};

	std::thread pump{[&] {
		const bool ran = scheduler.run_one_or_wait();
		returned.store(true, std::memory_order_release);
		EXPECT_FALSE(ran);
	}};

	scheduler.wake();
	pump.join();
	EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(TrampolineScheduler, RunsANonReentrantPostSynchronously)
{
	std::vector<int>          log;
	logged_call               call{.log = &log, .tag = 1};
	alt::trampoline_scheduler scheduler;

	scheduler.post(item_for(call));
	EXPECT_EQ(log, std::vector<int>{1});
}

TEST(TrampolineScheduler, DefersAReentrantPostToTheOutermostInvocation)
{
	// The outer item posts an inner item while it is running. The inner item must
	// not run inside the outer one, and must run before the outer post returns.
	alt::trampoline_scheduler scheduler;
	std::vector<int>          log;
	logged_call               inner{.log = &log, .tag = 2};

	struct outer_state
	{
		alt::trampoline_scheduler* scheduler;
		std::vector<int>*          log;
		logged_call*               inner;
	};

	outer_state state{.scheduler = &scheduler, .log = &log, .inner = &inner};

	const auto outer_trampoline = [](void* raw) {
		auto* const s = static_cast<outer_state*>(raw);
		s->log->push_back(1);
		s->scheduler->post(item_for(*s->inner));
		s->log->push_back(3); // must precede the inner item
	};

	scheduler.post(alt::work_item{.invoke = outer_trampoline, .state = &state});
	EXPECT_EQ(log, (std::vector<int>{1, 3, 2}));
}

TEST(SchedulerRef, IsEmptyWhenDefaultConstructed)
{
	const alt::scheduler_ref empty;
	EXPECT_FALSE(static_cast<bool>(empty));
}

TEST(SchedulerRef, ForwardsPostsToTheReferencedScheduler)
{
	std::vector<int>      log;
	logged_call           call{.log = &log, .tag = 5};
	alt::manual_scheduler scheduler;

	const alt::scheduler_ref reference{scheduler};
	EXPECT_TRUE(static_cast<bool>(reference));

	reference.post(item_for(call));
	EXPECT_EQ(scheduler.run_all(), 1U);
	EXPECT_EQ(log, std::vector<int>{5});
}

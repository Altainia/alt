#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	// A work_item that appends its tag to a shared log.
	struct logged_call
	{
		std::vector<int>* log;
		int               tag;
	};

	void log_trampoline(void* state) noexcept
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

	EXPECT_EQ(scheduler.drain(), 3U);
	EXPECT_EQ(log, (std::vector<int>{1, 2, 3}));
}

TEST(ManualScheduler, RunAllStillWorksAsTheDeprecatedNameForDrain)
{
	// The one site that still calls run_all(), so the compatibility alias keeps its
	// coverage after the rename. The deprecation warning is the point of the test,
	// which is why it is suppressed here and nowhere else: this build treats
	// warnings as errors, so without the suppression the alias could not be tested
	// at all.
	std::vector<int>      log;
	logged_call           first{.log = &log, .tag = 1};
	logged_call           second{.log = &log, .tag = 2};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(first));
	scheduler.post(item_for(second));

#if defined(__GNUC__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	EXPECT_EQ(scheduler.run_all(), 2U);
#if defined(__GNUC__)
#	pragma GCC diagnostic pop
#endif

	EXPECT_EQ(log, (std::vector<int>{1, 2}));
	EXPECT_TRUE(scheduler.empty());
}

TEST(ManualScheduler, AcceptsPostsFromAnotherThread)
{
	std::vector<int>      log;
	logged_call           call{.log = &log, .tag = 9};
	alt::manual_scheduler scheduler;

	std::thread poster{[&] { scheduler.post(item_for(call)); }};
	poster.join();

	EXPECT_EQ(scheduler.drain(), 1U);
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

TEST(ManualScheduler, RunOneOrWaitConsumesTheWakeAfterRunningAQueuedItem)
{
	// A wake and a queued item pending at the same time must not leave the
	// wake set once the item has run: a later call with nothing queued must
	// still block for a real post rather than returning false on a stale wake.
	std::vector<int>      log;
	logged_call           first{.log = &log, .tag = 1};
	logged_call           second{.log = &log, .tag = 2};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(first));
	scheduler.wake();

	EXPECT_TRUE(scheduler.run_one_or_wait());
	EXPECT_EQ(log, std::vector<int>{1});

	std::atomic<bool> returned{false};
	std::atomic<bool> ran_second{false};

	std::thread waiter{[&] {
		const bool ran = scheduler.run_one_or_wait();
		ran_second.store(ran, std::memory_order_release);
		returned.store(true, std::memory_order_release);
	}};

	// If the earlier wake had been left set, the call above would already
	// have returned by now instead of blocking for the post below.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(returned.load(std::memory_order_acquire));

	scheduler.post(item_for(second));
	waiter.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));
	EXPECT_TRUE(ran_second.load(std::memory_order_acquire));
	EXPECT_EQ(log, (std::vector<int>{1, 2}));
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

	const auto outer_trampoline = [](void* raw) noexcept {
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
	EXPECT_EQ(scheduler.drain(), 1U);
	EXPECT_EQ(log, std::vector<int>{5});
}

TEST(SchedulerConcepts, AWorkItemCallbackMayNotThrow)
{
	// work_item is an erased function pointer plus a void*, with no error channel.
	// An exception leaving a work item has no defined destination: it would surface
	// in whichever thread happened to be draining the queue, unrelated to whoever
	// posted the item. The contract is therefore that callbacks do not throw, and
	// the type enforces it rather than documenting it.
	static_assert(std::is_assignable_v<decltype(alt::work_item::invoke)&, void (*)(void*) noexcept>);
	static_assert(!std::is_assignable_v<decltype(alt::work_item::invoke)&, void (*)(void*)>);
}

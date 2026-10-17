#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/run_loop.hpp>
#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

namespace
{
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool ran_after_pool_hop = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool ran_after_loop_hop = false;

	// The thread the task's frame was destroyed on. That thread is the whole point
	// of a home scheduler, so the tests check it rather than only checking that the
	// frame went away. Atomic because the teardown may happen on a pool thread
	// while the test thread reads it.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, written during a teardown on another thread.
	std::atomic<std::thread::id> teardown_thread{};

	/** Records the thread its enclosing coroutine frame is destroyed on. */
	struct teardown_recorder
	{
		teardown_recorder() = default;

		~teardown_recorder()
		{
			teardown_thread.store(std::this_thread::get_id(), std::memory_order_release);
		}

		teardown_recorder(const teardown_recorder&)            = delete;
		teardown_recorder& operator=(const teardown_recorder&) = delete;
		teardown_recorder(teardown_recorder&&)                 = delete;
		teardown_recorder& operator=(teardown_recorder&&)      = delete;
	};

	// Occupies the pool's only thread until released, so a resumption posted behind
	// it is still queued when the stop is requested.
	struct pool_blocker
	{
		std::latch* release = nullptr;
	};

	void hold_a_pool_thread(void* state) noexcept
	{
		static_cast<pool_blocker*>(state)->release->wait();
	}

	// Posted behind the coroutine's resumption. The pool has one thread and the
	// queue is FIFO, so this running means that resumption has already completed.
	// That is what lets a test see where the teardown went without waiting on
	// something that may never arrive.
	struct pool_fence
	{
		std::latch* passed = nullptr;
	};

	void pass_a_pool_fence(void* state) noexcept
	{
		static_cast<pool_fence*>(state)->passed->count_down();
	}

	alt::cancellable_task hops_pool_then_loop(alt::thread_pool* pool, alt::run_loop* loop)
	{
		const frame_tracker     t{1};
		const teardown_recorder recorder;
		co_await alt::resume_on(*pool);
		ran_after_pool_hop = true;
		co_await alt::resume_on(*loop);
		ran_after_loop_hop = true;
	}
}

class SchedulerIntegrationTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
		ran_after_pool_hop = false;
		ran_after_loop_hop = false;
		teardown_thread.store(std::thread::id{}, std::memory_order_release);
	}
};

TEST_F(SchedulerIntegrationTest, AStopIsObservedWhenARealPoolResumesTheTask)
{
	// Issue #4's resumption check, through a real scheduler on a real thread rather
	// than through the hand-pumped one it was originally verified against.
	//
	// The loop, the latch and the blocker are all declared outside the scope that
	// holds the pool, so the pool's joining destructor runs while they are alive.
	alt::run_loop loop;
	std::latch    release{1};
	pool_blocker  blocker{.release = &release};

	{
		auto made = alt::thread_pool::create(1);
		ASSERT_TRUE(made.has_value()) << made.error().message();
		alt::thread_pool& pool = *made;

		pool.post(alt::work_item{.invoke = &hold_a_pool_thread, .state = &blocker});

		auto handle = alt::start(hops_pool_then_loop(&pool, &loop));

		// Deliberately EXPECT and not ASSERT. A fatal assertion here would return
		// from the test with the pool's only thread still parked in the blocker, and
		// the pool's joining destructor would then hang instead of letting the
		// failure be reported. Nothing below depends on this value.
		EXPECT_EQ(live_frames, 1);

		// The resumption is queued behind the blocker, so the stop lands first.
		handle.request_stop();
		release.count_down();
	} // the pool's destructor drains and joins, so the teardown has happened

	EXPECT_FALSE(ran_after_pool_hop);
	EXPECT_FALSE(ran_after_loop_hop);
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(destruction_order, std::vector<int>{1});

	// No home scheduler, so the frame was destroyed in place by whichever thread
	// observed the stop. That was a pool thread, not this one, and nothing was
	// deferred to the loop.
	EXPECT_NE(teardown_thread.load(std::memory_order_acquire), std::this_thread::get_id());
	EXPECT_NE(teardown_thread.load(std::memory_order_acquire), std::thread::id{});
	EXPECT_TRUE(loop.empty());
}

TEST_F(SchedulerIntegrationTest, AnUncancelledTaskRunsThroughBothSchedulers)
{
	// The control: without a stop, the same task hops to the pool, back to the loop,
	// and completes. If this fails, the resumption check is firing when it must not.
	//
	// The loop, the latch and the fence are declared ahead of the pool: a pool
	// thread touches all of them, so they must outlive the pool's joining
	// destructor.
	alt::run_loop loop;
	std::latch    resumption_done{1};
	pool_fence    fence{.passed = &resumption_done};

	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	auto handle = alt::start(hops_pool_then_loop(&pool, &loop));

	// Queued behind the resumption the start above posted.
	pool.post(alt::work_item{.invoke = &pass_a_pool_fence, .state = &fence});
	resumption_done.wait();

	// The pool thread ran the body as far as the second hop and posted the
	// continuation onto the loop. Asserted rather than waited for: were the
	// resumption check firing when it must not, a wait would hang where this fails.
	EXPECT_TRUE(ran_after_pool_hop);
	EXPECT_FALSE(ran_after_loop_hop);
	ASSERT_FALSE(loop.empty());

	// Run as a platform host would, a bounded batch at a time. One item completes
	// the task.
	EXPECT_EQ(loop.run_pending(), 1U);

	EXPECT_TRUE(ran_after_loop_hop);
	EXPECT_FALSE(handle.stop_requested());

	// Completion, not cancellation: the frame is freed by the final suspension on
	// the thread that ran the last resumption, and neither scheduler is left
	// holding anything.
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(teardown_thread.load(std::memory_order_acquire), std::this_thread::get_id());
	EXPECT_TRUE(loop.empty());
	EXPECT_TRUE(pool.empty());
}

TEST_F(SchedulerIntegrationTest, QuiescingTwoCrossPostingSchedulersTearsTheChainDown)
{
	// The case no destructor can handle on its own: the chain is parked on the pool
	// with the loop as its home, so the teardown is posted from a pool thread onto
	// the loop and only the loop can perform it.
	//
	// Everything the posted items point at is declared ahead of the pool, so the
	// pool's joining destructor runs while all of it is still alive.
	alt::run_loop loop;
	std::latch    release{1};
	std::latch    resumption_done{1};
	pool_blocker  blocker{.release = &release};
	pool_fence    fence{.passed = &resumption_done};

	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	pool.post(alt::work_item{.invoke = &hold_a_pool_thread, .state = &blocker});

	auto handle = alt::start(hops_pool_then_loop(&pool, &loop), {.home = alt::scheduler_ref{loop}});

	// Queued behind the resumption the start above posted, which is in turn behind
	// the blocker.
	pool.post(alt::work_item{.invoke = &pass_a_pool_fence, .state = &fence});

	// EXPECT rather than ASSERT for the reason given in the first test: the pool's
	// thread is parked in the blocker and must be released on every path out.
	EXPECT_EQ(live_frames, 1);

	handle.request_stop();
	release.count_down();

	// The pool thread is now past the resumption, so whatever it was going to do
	// with the cancelled chain it has already done.
	resumption_done.wait();

	// It posted the teardown rather than performing it, because the loop is this
	// chain's home. So the frame is still alive and the loop is holding exactly the
	// one item that frees it. Checked here rather than by waiting on the loop: were
	// the home scheduler ignored, a wait would hang where these fail.
	EXPECT_EQ(live_frames, 1);
	EXPECT_FALSE(loop.empty());
	EXPECT_EQ(loop.drain(), 1U);

	EXPECT_FALSE(ran_after_pool_hop);
	EXPECT_FALSE(ran_after_loop_hop);
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(destruction_order, std::vector<int>{1});

	// The home scheduler's whole purpose: the frames are destroyed on the loop's
	// thread, not on the pool thread that observed the stop.
	EXPECT_EQ(teardown_thread.load(std::memory_order_acquire), std::this_thread::get_id());
	EXPECT_TRUE(loop.empty());
	EXPECT_TRUE(pool.empty());
}

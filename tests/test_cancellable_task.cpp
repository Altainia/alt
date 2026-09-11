#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

namespace
{

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable shared test state
	bool reached_end = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable shared test state
	bool middle_reached_end = false;

	alt::task<void> parking_leaf()
	{
		const frame_tracker t{3};
		co_await park{};
		co_await park{};
		reached_end = true;
	}

	alt::task<void> middle()
	{
		const frame_tracker t{2};
		co_await parking_leaf();
		middle_reached_end = true;
	}

	alt::cancellable_task root_over_a_chain()
	{
		const frame_tracker t{1};
		co_await middle();
	}

	alt::cancellable_task root_that_parks_then_awaits_a_child()
	{
		const frame_tracker t{1};
		co_await park{};
		co_await parking_leaf(); // cancellation observed here, before the child starts
		reached_end = true;
	}

	alt::cancellable_task root_that_completes()
	{
		const frame_tracker t{1};
		co_await park{};
		reached_end = true;
	}

	alt::cancellable_task root_that_throws()
	{
		const frame_tracker t{1};
		co_await park{};
		throw std::runtime_error{"boom"};
	}

	// The iteration count is deliberately modest. Neither assertion depends on it:
	// an uncancelled loop must finish and a cancelled one must not, and a larger
	// count would prove nothing extra while costing every run.
	alt::cancellable_task compute_loop(int iterations)
	{
		const frame_tracker t{1};
		for(int i = 0; i < iterations; ++i)
		{
			co_await alt::cancellation_point{};
		}
		reached_end = true;
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable shared test state
	std::exception_ptr captured_error{};

	void capture_error(std::exception_ptr error, void* state)
	{
		*static_cast<std::exception_ptr*>(state) = std::move(error);
	}

}

class CancellableTaskTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
		reached_end        = false;
		middle_reached_end = false;
		captured_error     = {};
	}
};

TEST_F(CancellableTaskTest, DoesNotRunUntilStarted)
{
	{
		auto t = root_that_completes();
		EXPECT_EQ(live_frames, 0);
	}
	// Destroyed without being started: the body never ran and the frame is gone.
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(reached_end);
}

TEST_F(CancellableTaskTest, RunsToCompletionWhenNotCancelled)
{
	auto handle = alt::start(root_that_completes());
	EXPECT_EQ(live_frames, 1);
	resume_parked();
	EXPECT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, SelfDestructsTheWholeChainFromTheLeaf)
{
	auto handle = alt::start(root_over_a_chain());
	EXPECT_EQ(live_frames, 3); // root, middle, leaf

	handle.request_stop();
	resume_parked(); // the leaf observes cancellation at its second await

	EXPECT_FALSE(reached_end);
	// Proves the middle frame is torn down rather than resumed: a bug that let
	// the coroutine machinery resume middle() after its child chain was
	// abandoned would leave live_frames and destruction_order looking correct
	// (middle's own frame still gets destroyed eventually) but would let this
	// trailing statement run. Nothing else in this test would catch that.
	EXPECT_FALSE(middle_reached_end);
	EXPECT_EQ(live_frames, 0);

	// Root frame destruction reaches the leaf through the owning task destructors,
	// so leaf locals are destroyed before their parents'.
	EXPECT_EQ(destruction_order, (std::vector<int>{3, 2, 1}));
}

TEST_F(CancellableTaskTest, CancellationObservedAtTheRootNeverStartsTheChild)
{
	auto handle = alt::start(root_that_parks_then_awaits_a_child());
	EXPECT_EQ(live_frames, 1);

	handle.request_stop();
	resume_parked();

	EXPECT_FALSE(reached_end);
	// Zero tracked frames, which is the child's body never having started. Calling
	// parking_leaf() does create its frame; what the cancellation prevented is the
	// first statement of that body, and so the tracker it would have constructed.
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, AnExternalTokenCancelsTheTask)
{
	std::stop_source external;
	auto             handle = alt::start(root_over_a_chain(), {.token = external.get_token()});
	EXPECT_EQ(live_frames, 3);

	external.request_stop();
	EXPECT_TRUE(handle.stop_requested()); // linked, so the handle sees it too

	resume_parked();
	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, AnAlreadyStoppedExternalTokenIsObservedAtTheFirstSuspendPoint)
{
	std::stop_source external;
	external.request_stop();

	auto handle = alt::start(root_that_completes(), {.token = external.get_token()});

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0); // destroyed at its first co_await
	EXPECT_TRUE(parked == std::coroutine_handle<>{});
}

TEST_F(CancellableTaskTest, HomeSchedulerDefersDestructionOffTheObservingThread)
{
	alt::manual_scheduler home;
	auto                  handle = alt::start(root_over_a_chain(), {.home = alt::scheduler_ref{home}});

	handle.request_stop();
	resume_parked();

	EXPECT_EQ(live_frames, 3); // posted, not destroyed
	EXPECT_EQ(home.run_all(), 1U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(reached_end);
}

TEST_F(CancellableTaskTest, CancellationPointCreatesASuspendPointInAComputeLoop)
{
	auto handle = alt::start(compute_loop(1000));
	// The loop has no natural suspension point, so without cancellation_point it
	// would run to completion. It suspends only when cancellation is requested,
	// so an uncancelled loop still finishes.
	EXPECT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancellationPointObservesAPreRequestedStop)
{
	std::stop_source external;
	external.request_stop();
	auto handle = alt::start(compute_loop(1000), {.token = external.get_token()});

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancellationPointIsInertWithNoRootAboveIt)
{
	// A helper carrying a cancellation point must be safe to call from a plain
	// coroutine that no cancellable_task started.
	const auto helper = []() -> alt::task<int> {
		co_await alt::cancellation_point{};
		co_return 11;
	};

	EXPECT_EQ(alt::sync_wait(helper()), 11);
}

TEST_F(CancellableTaskTest, TheHandleIsSafeToUseAfterTheTaskIsGone)
{
	auto handle = alt::start(root_that_completes());
	resume_parked();
	ASSERT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);

	// The frame is gone; every handle operation must still be valid.
	EXPECT_FALSE(handle.stop_requested());
	EXPECT_NO_THROW(handle.request_stop());
	EXPECT_TRUE(handle.stop_requested());
	EXPECT_NO_THROW(handle.request_stop()); // idempotent
	EXPECT_TRUE(handle.get_token().stop_requested());
}

TEST_F(CancellableTaskTest, AnUnhandledExceptionReachesTheHandler)
{
	const alt::exception_handler handler{.invoke = &capture_error, .state = &captured_error};
	auto                         task_handle = alt::start(root_that_throws(), {.on_exception = handler});

	resume_parked();

	ASSERT_TRUE(captured_error);
	EXPECT_THROW(std::rethrow_exception(captured_error), std::runtime_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancelOnOneThreadAndSelfDestructOnAnother)
{
	// The motivating scenario: the window closes on this thread while the
	// coroutine is mid-flight, and the worker thread destroys the chain.
	auto handle = alt::start(root_over_a_chain());
	ASSERT_EQ(live_frames, 3);

	handle.request_stop();

	const auto resume_me = parked;
	parked               = {};
	std::thread worker{[resume_me] { resume_me.resume(); }};
	worker.join();

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

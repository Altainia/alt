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

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool ran_after_resume = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool child_ran_after_resume = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool root_ran_after_child = false;

	// Parks on the scheduler, so a stop arrives while the task is suspended. The
	// statement after the resume is the one issue #4 showed runs anyway.
	alt::cancellable_task resumes_through_a_scheduler(alt::manual_scheduler* scheduler)
	{
		const frame_tracker t{1};
		co_await alt::resume_on(*scheduler);
		ran_after_resume = true;
		co_await alt::cancellation_point{};
		reached_end = true;
	}

	// Parks on a foreign awaitable, so the stop arrives while the chain is suspended
	// inside the child. Its completion transfers back to the root, and that transfer
	// is the resumption which has to observe the stop.
	alt::task<void> parks_then_returns()
	{
		const frame_tracker t{2};
		co_await park{};
		child_ran_after_resume = true;
	}

	alt::cancellable_task root_awaiting_a_parking_child()
	{
		const frame_tracker t{1};
		co_await parks_then_returns();
		root_ran_after_child = true;
		co_await alt::cancellation_point{};
		reached_end = true;
	}
}

class CancellableTaskTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
		reached_end            = false;
		middle_reached_end     = false;
		ran_after_resume       = false;
		child_ran_after_resume = false;
		root_ran_after_child   = false;
		captured_error         = {};
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
	EXPECT_EQ(home.drain(), 1U);
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

TEST_F(CancellableTaskTest, AnExternalStopConcurrentWithTeardownIsSafe)
{
	// Exercises an external stop request landing while this thread tears the chain
	// down: request_stop runs the forwarder on the requesting thread, so the two
	// overlap. Worth running under both sanitizers.
	//
	// Be clear about what this does not do. It does not prove the teardown is safe
	// against every interleaving. The sharpest one, a forwarder dereferencing the
	// promise's own source mid-destruction, was only a couple of destructor calls
	// wide, and this test was measured against a deliberately reordered member block
	// and passed, so it would not have caught that defect. That is why the hazard was
	// removed structurally, by giving the forwarder a reference-counted copy rather
	// than a pointer into the frame, instead of being left to a test to catch.
	for(int attempt = 0; attempt < 100; ++attempt)
	{
		reset_coroutine_test_state();

		std::stop_source external;
		auto             handle = alt::start(root_over_a_chain(), {.token = external.get_token()});
		ASSERT_EQ(live_frames, 3) << "attempt " << attempt;

		std::thread stopper{[&external] { external.request_stop(); }};

		// The stop may land on either side of any given suspension point, so resume
		// until the chain is gone. It goes either by teardown or by running out of
		// park points, and both leave no frames behind.
		while(live_frames != 0)
		{
			const auto resume_me = parked;
			if(!resume_me)
			{
				break;
			}
			parked = {};
			resume_me.resume();
		}

		stopper.join();

		ASSERT_EQ(live_frames, 0) << "attempt " << attempt;

		// The handle outlives the frame whichever way the interleaving went.
		handle.request_stop();
		ASSERT_TRUE(handle.stop_requested()) << "attempt " << attempt;
	}
}

TEST_F(CancellableTaskTest, ObservesCancellationWhenResumedThroughAScheduler)
{
	// Issue #4. A task suspended when the stop arrives used to resume and run body
	// code until its next co_await. The resumption itself has to observe the stop,
	// because suspended is the state this component exists to be cancelled in.
	alt::manual_scheduler scheduler;

	auto handle = alt::start(resumes_through_a_scheduler(&scheduler));
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();            // the window closes while the task is parked
	EXPECT_EQ(scheduler.drain(), 1U); // the awaited thing completes

	EXPECT_FALSE(ran_after_resume);
	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, ObservesCancellationWhenAChildTaskCompletes)
{
	// The second resumption alt owns: a child task finishing and transferring back
	// to its parent. Issue #4's gap applied here too.
	auto handle = alt::start(root_awaiting_a_parking_child());
	ASSERT_EQ(live_frames, 2);

	handle.request_stop();
	resume_parked(); // the child resumes and runs to its co_return

	// The child's own resumption came from a foreign awaitable, which stays checked
	// on entry only, so its body does run. That is the documented boundary.
	EXPECT_TRUE(child_ran_after_resume);

	// The transfer back to the root is alt's own, and must observe the stop.
	EXPECT_FALSE(root_ran_after_child);
	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, ObservesCancellationWhenResumedOnAnotherThread)
{
	// The motivating scenario in full: the stop lands while the task is parked, and
	// the resumption happens on a worker rather than on the thread that asked.
	alt::manual_scheduler scheduler;

	auto handle = alt::start(resumes_through_a_scheduler(&scheduler));
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();

	std::jthread worker{[&scheduler] { scheduler.drain(); }};
	worker.join();

	EXPECT_FALSE(ran_after_resume);
	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, ResumptionTeardownHonorsTheHomeScheduler)
{
	// Abandoning at a resumption goes through the same teardown as abandoning on
	// entry, so a home scheduler still decides where the frames are destroyed.
	alt::manual_scheduler work;
	alt::manual_scheduler home;

	auto handle = alt::start(resumes_through_a_scheduler(&work), {.home = alt::scheduler_ref{home}});
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	EXPECT_EQ(work.drain(), 1U); // the resumption observes the stop and posts

	EXPECT_FALSE(ran_after_resume);
	EXPECT_EQ(live_frames, 1); // posted, not yet destroyed

	EXPECT_EQ(home.drain(), 1U);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, AnUncancelledResumptionProceedsNormally)
{
	// The check must not disturb the ordinary path: with no stop requested, the
	// resumption resumes and the body runs to completion as before.
	alt::manual_scheduler scheduler;

	auto handle = alt::start(resumes_through_a_scheduler(&scheduler));
	EXPECT_EQ(scheduler.drain(), 1U);

	EXPECT_TRUE(ran_after_resume);
	EXPECT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(handle.stop_requested());
}

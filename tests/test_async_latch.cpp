#include <gtest/gtest.h>

#include <alt/async_latch.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/task.hpp>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// The concept describes context-free awaitables, so this is the check that the
// awaiter still accepts an erased std::coroutine_handle<> and therefore works in
// a coroutine that relays no cancellation context at all.
static_assert(alt::awaitable<alt::async_latch&>);
static_assert(std::is_same_v<alt::await_result_t<alt::async_latch&>, std::expected<void, std::error_code>>);

// A latch is referred to by the awaiters inside suspended coroutine frames, so it
// can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<alt::async_latch>);
static_assert(!std::is_move_constructible_v<alt::async_latch>);

namespace
{
	/** How many times the contended tests repeat their race. */
	constexpr int race_repeats = 100;

	/** The error every released wait reports. */
	const std::error_code released_error = std::make_error_code(std::errc::operation_canceled);

	/** What one waiter coroutine recorded about its wait, for the test to inspect. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the wait reported success. */
		bool satisfied = false;

		/** The error a released wait reported. */
		std::error_code error;
	};

	/** Records one wait's outcome. Shared by every waiter coroutine below. */
	void record_outcome(wait_record& record, const std::expected<void, std::error_code>& result)
	{
		++record.resumptions;
		record.satisfied = result.has_value();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/** A cancellable waiter that records how its wait ended and then finishes. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the latch and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task waiting_root(alt::async_latch& latch, wait_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		record_outcome(record, co_await latch);
	}

	/** Starts a second waiter from the continuation of its own wait, which runs inside count_down(). */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::cancellable_task starts_another_waiter(alt::async_latch& latch, wait_record& first, wait_record& second)
	{
		const frame_tracker tracker{1};
		record_outcome(first, co_await latch);
		const auto handle = alt::start(waiting_root(latch, second, 2));
	}

	/** Awaits the latch a second time from the continuation of its first wait. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::cancellable_task twice_waiting_root(alt::async_latch& latch, wait_record& first, wait_record& second)
	{
		const frame_tracker tracker{1};
		record_outcome(first, co_await latch);
		record_outcome(second, co_await latch);
	}

	/**
	 * A waiter that rendezvouses immediately before its co_await, so that another
	 * thread's count_down lands in the window between this waiter's own ready check
	 * and its link.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the latch, the record and the counter are owned by the calling test, which joins both threads before leaving the scope that holds them.
	alt::cancellable_task racing_waiter(alt::async_latch& latch, wait_record& record, std::atomic<std::size_t>& arrived)
	{
		const frame_tracker tracker{1};
		rendezvous(arrived, 2);
		record_outcome(record, co_await latch);
	}

	/**
	 * Has @p threads threads each lower the latch by one, rendezvousing first so
	 * that they reach the count together. Returns once all of them have finished.
	 */
	void count_down_together(alt::async_latch& latch, std::size_t threads)
	{
		std::atomic<std::size_t>  arrived{0};
		std::vector<std::jthread> workers;
		workers.reserve(threads);

		for(std::size_t index = 0; index < threads; ++index)
		{
			workers.emplace_back([&] {
				rendezvous(arrived, threads);
				latch.count_down();
			});
		}
	}

	/**
	 * One round of the concurrent count-down test. A function of its own, with the
	 * assertions in it, so the loop in the test body is not charged for them.
	 */
	void count_down_together_and_expect_one_release()
	{
		constexpr std::size_t threads = 4;

		reset_coroutine_test_state();

		alt::async_latch latch{threads};
		wait_record      record;
		const auto       handle = alt::start(waiting_root(latch, record, 1));
		ASSERT_EQ(live_frames, 1);

		count_down_together(latch, threads);

		ASSERT_EQ(record.resumptions, 1);
		ASSERT_TRUE(record.satisfied);
		ASSERT_TRUE(latch.try_wait());
		ASSERT_EQ(live_frames, 0);
	}
}

class AsyncLatchTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(AsyncLatchTest, AwaitingAZeroLatchIsReady)
{
	alt::async_latch latch{0};

	// Taken through the awaiter directly, because never suspending is a property of
	// await_ready rather than of anything observable after the fact.
	auto awaiter = alt::get_awaiter(latch);
	EXPECT_TRUE(awaiter.await_ready());
	EXPECT_TRUE(awaiter.await_resume().has_value());

	// And end to end: nothing resumes this waiter, so it can only have run through.
	wait_record record;
	const auto  handle = alt::start(waiting_root(latch, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, ConstructingWithZeroIsImmediatelyReady)
{
	const alt::async_latch latch{0};
	EXPECT_TRUE(latch.try_wait());
}

TEST_F(AsyncLatchTest, CountingDownToZeroReleasesEveryWaiter)
{
	alt::async_latch latch{1};
	wait_record      first;
	wait_record      second;
	wait_record      third;

	const auto first_handle  = alt::start(waiting_root(latch, first, 1));
	const auto second_handle = alt::start(waiting_root(latch, second, 2));
	const auto third_handle  = alt::start(waiting_root(latch, third, 3));
	ASSERT_EQ(live_frames, 3);

	latch.count_down();

	// One release for all three, not just the head of the list.
	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_EQ(third.resumptions, 1);
	EXPECT_TRUE(first.satisfied);
	EXPECT_TRUE(second.satisfied);
	EXPECT_TRUE(third.satisfied);
	EXPECT_EQ(live_frames, 0);

	// Link order is resume order, so the oldest waiter ran first.
	EXPECT_EQ(destruction_order, (std::vector<int>{1, 2, 3}));
}

TEST_F(AsyncLatchTest, CountingDownPartiallyDoesNotRelease)
{
	alt::async_latch latch{3};
	wait_record      record;

	const auto handle = alt::start(waiting_root(latch, record, 1));
	ASSERT_EQ(live_frames, 1);

	latch.count_down();
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_FALSE(latch.try_wait());

	latch.count_down();
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_FALSE(latch.try_wait());
	EXPECT_EQ(live_frames, 1); // still parked, frame alive

	latch.count_down();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, CountDownByMoreThanOneReachesZero)
{
	alt::async_latch latch{5};
	wait_record      record;

	const auto handle = alt::start(waiting_root(latch, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	latch.count_down(2);
	EXPECT_EQ(record.resumptions, 0);

	latch.count_down(3);
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_TRUE(latch.try_wait());
}

TEST_F(AsyncLatchTest, CountingDownByZeroChangesNothing)
{
	alt::async_latch latch{1};
	wait_record      record;

	const auto handle = alt::start(waiting_root(latch, record, 1));

	latch.count_down(0);
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_FALSE(latch.try_wait());

	// And on a latch already at zero, where it must not look like a step onto zero.
	latch.count_down();
	ASSERT_EQ(record.resumptions, 1);
	latch.count_down(0);
	EXPECT_EQ(record.resumptions, 1);
}

TEST_F(AsyncLatchTest, TryWaitReflectsTheCount)
{
	alt::async_latch latch{2};
	EXPECT_FALSE(latch.try_wait());

	latch.count_down();
	EXPECT_FALSE(latch.try_wait());

	latch.count_down();
	EXPECT_TRUE(latch.try_wait());
}

TEST_F(AsyncLatchTest, CountingDownPastZeroIsHarmless)
{
	alt::async_latch latch{2};
	wait_record      record;

	const auto handle = alt::start(waiting_root(latch, record, 1));

	// Overshoots in a single call: the count clamps at zero and releases once.
	latch.count_down(5);
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(latch.try_wait());

	// And again after it is already there: nothing to release, nothing to break, and
	// the latch must still read as released rather than wrapping to some other count.
	latch.count_down();
	latch.count_down(7);
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(latch.try_wait());

	// Still a latch that does not suspend anyone new.
	wait_record late;
	const auto  late_handle = alt::start(waiting_root(latch, late, 2));
	EXPECT_EQ(late.resumptions, 1);
	EXPECT_TRUE(late.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, AReleasedLatchStaysReleasedForLaterWaiters)
{
	// Single use: there is no reset, so a waiter arriving after release never parks.
	alt::async_latch latch{1};
	latch.count_down();
	ASSERT_TRUE(latch.try_wait());

	wait_record record;
	const auto  handle = alt::start(waiting_root(latch, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, ASatisfiedWaitReportsSuccess)
{
	alt::async_latch latch{1};
	wait_record      record;

	const auto handle = alt::start(waiting_root(latch, record, 1));
	latch.count_down();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(record.error, std::error_code{});
}

TEST_F(AsyncLatchTest, CancellingAParkedWaiterReleasesIt)
{
	// The stop callback is the only thing that can reach a parked waiter: nothing in
	// the coroutine machinery runs again until someone resumes the frame.
	alt::async_latch latch{1};
	wait_record      record;

	auto handle = alt::start(waiting_root(latch, record, 1));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	// Released rather than satisfied: the count never reached zero.
	EXPECT_FALSE(latch.try_wait());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, ACancelledWaiterIsNotResumedAgainByTheCountReachingZero)
{
	// The waiter has gone, so the count reaching zero finds an empty queue and must
	// not resume the frame the cancellation already finished.
	alt::async_latch latch{1};
	wait_record      record;

	auto handle = alt::start(waiting_root(latch, record, 1));
	handle.request_stop();
	ASSERT_EQ(record.resumptions, 1);

	latch.count_down();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(latch.try_wait());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, DestroyingTheLatchReleasesParkedWaiters)
{
	wait_record first;
	wait_record second;

	{
		alt::async_latch latch{2};
		const auto       first_handle  = alt::start(waiting_root(latch, first, 1));
		const auto       second_handle = alt::start(waiting_root(latch, second, 2));
		ASSERT_EQ(live_frames, 2);
		ASSERT_EQ(first.resumptions, 0);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_EQ(first.error, released_error);
	EXPECT_EQ(second.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, AWaiterThatReAwaitsWhileTheLatchIsBeingDestroyedIsStillReleased)
{
	// As async_event's test of the same name: the re-await parks in a list that is
	// still whole, and the member's destructor releases it, but only because the
	// latch's own destructor released the first round itself.
	wait_record first;
	wait_record second;

	{
		alt::async_latch latch{1};
		const auto       handle = alt::start(twice_waiting_root(latch, first, second));
		ASSERT_EQ(live_frames, 1);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, AWaiterArrivingFromInsideTheReleasingCountDownDoesNotPark)
{
	// Why count_down publishes zero before it walks the queue. The first waiter's
	// continuation runs from inside count_down(), the one moment a waiter can arrive
	// after the walk is over and before count_down() has returned; the count is the
	// only thing that can tell it the latch is already released.
	alt::async_latch latch{1};
	wait_record      first;
	wait_record      second;

	const auto handle = alt::start(starts_another_waiter(latch, first, second));
	ASSERT_EQ(live_frames, 1);

	latch.count_down();

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_TRUE(first.satisfied);

	// Nothing will ever walk the queue again, so a second waiter that parked here
	// would wait forever.
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncLatchTest, ALatchReleasedBetweenTheReadyCheckAndTheLinkDoesNotPark)
{
	// The window the link's condition closes, reached deterministically by driving
	// the awaiter by hand: its ready check is asked first, the latch is released
	// second, and only then does it try to park.
	alt::async_latch latch{1};

	auto awaiter = alt::get_awaiter(latch);
	ASSERT_FALSE(awaiter.await_ready());

	latch.count_down();

	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));
	EXPECT_TRUE(awaiter.await_resume().has_value());
}

TEST_F(AsyncLatchTest, ALatchNotYetReleasedStillParksAtTheLink)
{
	// The converse, so the test above cannot be passed by a link that never parks.
	alt::async_latch latch{2};

	auto awaiter = alt::get_awaiter(latch);
	ASSERT_FALSE(awaiter.await_ready());

	latch.count_down();

	EXPECT_TRUE(awaiter.await_suspend(std::noop_coroutine()));
}

TEST_F(AsyncLatchTest, ConcurrentCountDownsReleaseTheWaiterExactlyOnce)
{
	// Several threads each lower the count by one, rendezvousing first so that they
	// reach the compare-exchange together. A decrement lost to a race leaves the
	// count above zero, and the waiter is never released.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		SCOPED_TRACE(repeat);
		count_down_together_and_expect_one_release();

		if(HasFatalFailure())
		{
			return;
		}
	}
}

TEST_F(AsyncLatchTest, AStopAndTheFinalCountDownDoNotBothResume)
{
	// End to end, the property the claim election exists for: the releasing
	// count_down and a cancellation reaching for the same parked waiter must not both
	// resume it. The rendezvous is what makes it contended: both threads block on one
	// counter and the second arrival releases both, so the detach and the stop
	// callback's claim reach the same state word together.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_latch         latch{1};
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		auto handle = alt::start(waiting_root(latch, record, 1));
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		{
			std::jthread counter{[&] {
				rendezvous(arrived, 2);
				latch.count_down();
			}};

			std::jthread stopper{[&handle, &arrived] {
				rendezvous(arrived, 2);
				handle.request_stop();
			}};
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncLatchTest, AConcurrentReleaseIsNeverMissedByAnArrivingWaiter)
{
	// A waiter that checks the latch, finds it unreleased, and links, against a
	// count_down landing between those steps. The rendezvous sits in the waiter's own
	// body immediately before the co_await. The deterministic tests above are the
	// guards for this window; what this adds is ThreadSanitizer over a count_down
	// that really is concurrent with a waiter arriving. The latch is inspected before
	// it leaves scope because its destructor would release a lost waiter and hide the
	// defect.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_latch         latch{1};
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		{
			std::jthread waiter{[&] {
				const auto handle = alt::start(racing_waiter(latch, record, arrived));
			}};

			rendezvous(arrived, 2);
			latch.count_down();
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_TRUE(record.satisfied) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

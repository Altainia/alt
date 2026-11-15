#include <gtest/gtest.h>

#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/completion_counter.hpp>
#include <atomic>
#include <cstddef>
#include <system_error>
#include <thread>
#include <type_traits>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// A counter is referred to by the awaiters inside suspended coroutine frames, so it
// can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<alt::detail::completion_counter>);
static_assert(!std::is_move_constructible_v<alt::detail::completion_counter>);
static_assert(std::is_same_v<alt::await_result_t<alt::detail::completion_counter::join_awaiter>, alt::wait_result>);

namespace
{
	/** How many times the contended test repeats its race. */
	constexpr int race_repeats = 100;

	/** The condition every released wait reports. See alt::wait_result. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/** What one joiner coroutine recorded about its wait, for the test to inspect. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the join reported success. */
		bool satisfied = false;

		/** The error a released join reported. */
		std::error_code error;
	};

	/** Records one join's outcome. */
	void record_outcome(wait_record& record, const alt::wait_result& result)
	{
		++record.resumptions;
		record.satisfied = result.has_value();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/** A cancellable joiner that records how its wait ended and then finishes. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the counter and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task joining_root(alt::detail::completion_counter& counter, wait_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		record_outcome(record, co_await counter.join());
	}

	/**
	 * A joiner that rendezvouses immediately before its co_await, so that another
	 * thread's complete() lands in the window between this joiner's own ready check
	 * and its link.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the counter, the record and the arrival count are owned by the calling test, which joins the other thread before leaving the scope that holds them.
	alt::cancellable_task racing_joiner(alt::detail::completion_counter& counter, wait_record& record, std::atomic<std::size_t>& arrived)
	{
		const frame_tracker tracker{1};
		rendezvous(arrived, 2);
		record_outcome(record, co_await counter.join());
	}
}

class CompletionCounterTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(CompletionCounterTest, JoiningAnIdleCounterIsReadyWithoutSuspending)
{
	alt::detail::completion_counter counter;
	ASSERT_EQ(counter.outstanding(), 0U);

	// Taken through the awaiter directly, because never suspending is a property of
	// await_ready rather than of anything observable after the fact.
	auto awaiter = counter.join();
	EXPECT_TRUE(awaiter.await_ready());
	EXPECT_TRUE(awaiter.await_resume().has_value());

	// And end to end: nothing resumes this joiner, so it can only have run through.
	wait_record record;
	const auto  handle = alt::start(joining_root(counter, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CompletionCounterTest, AddMakesTheCounterOutstanding)
{
	alt::detail::completion_counter counter;
	counter.add();

	EXPECT_EQ(counter.outstanding(), 1U);
	counter.complete();
	EXPECT_EQ(counter.outstanding(), 0U);
}

TEST_F(CompletionCounterTest, AddTakesACountGreaterThanOne)
{
	alt::detail::completion_counter counter;
	counter.add(3);

	EXPECT_EQ(counter.outstanding(), 3U);

	counter.complete();
	counter.complete();
	counter.complete();
	EXPECT_EQ(counter.outstanding(), 0U);
}

TEST_F(CompletionCounterTest, JoiningABusyCounterSuspendsUntilTheLastCompletion)
{
	alt::detail::completion_counter counter;
	counter.add();
	wait_record record;

	const auto handle = alt::start(joining_root(counter, record, 1));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1);

	counter.complete();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CompletionCounterTest, CompleteBelowTheLastOneReleasesNoJoiner)
{
	alt::detail::completion_counter counter;
	counter.add(2);
	wait_record record;

	const auto handle = alt::start(joining_root(counter, record, 1));
	counter.complete();

	EXPECT_EQ(counter.outstanding(), 1U);
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_EQ(live_frames, 1);

	counter.complete();
	EXPECT_EQ(record.resumptions, 1);
}

TEST_F(CompletionCounterTest, ReachingZeroReleasesEveryJoiner)
{
	alt::detail::completion_counter counter;
	counter.add();
	wait_record first;
	wait_record second;
	wait_record third;

	const auto first_handle  = alt::start(joining_root(counter, first, 1));
	const auto second_handle = alt::start(joining_root(counter, second, 2));
	const auto third_handle  = alt::start(joining_root(counter, third, 3));
	ASSERT_EQ(live_frames, 3);

	counter.complete();

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_EQ(third.resumptions, 1);
	EXPECT_TRUE(first.satisfied);
	EXPECT_TRUE(second.satisfied);
	EXPECT_TRUE(third.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CompletionCounterTest, ASatisfiedJoinReportsSuccess)
{
	alt::detail::completion_counter counter;
	counter.add();
	wait_record record;

	const auto handle = alt::start(joining_root(counter, record, 1));
	counter.complete();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(record.error, std::error_code{});
}

TEST_F(CompletionCounterTest, AddAfterReachingZeroMakesItBusyAgain)
{
	// Not single-use, unlike std::latch and alt::async_latch: a task_group accepts
	// spawns after a join has begun, so reaching zero must not retire the counter.
	alt::detail::completion_counter counter;
	counter.add();
	counter.complete();
	ASSERT_EQ(counter.outstanding(), 0U);

	counter.add();
	EXPECT_EQ(counter.outstanding(), 1U);

	wait_record record;
	const auto  handle = alt::start(joining_root(counter, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	counter.complete();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
}

TEST_F(CompletionCounterTest, DestroyingACounterReleasesItsJoinersUnsatisfied)
{
	wait_record first;
	wait_record second;

	{
		alt::detail::completion_counter counter;
		counter.add();
		const auto first_handle  = alt::start(joining_root(counter, first, 1));
		const auto second_handle = alt::start(joining_root(counter, second, 2));
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

TEST_F(CompletionCounterTest, ACancelledJoinerIsReleasedWithOperationCanceled)
{
	alt::detail::completion_counter counter;
	counter.add();
	wait_record record;

	auto handle = alt::start(joining_root(counter, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 0);

	// Cancelling the joiner says nothing about the work it was waiting for.
	EXPECT_EQ(counter.outstanding(), 1U);
	counter.complete();
}

TEST_F(CompletionCounterTest, CompletionFromAnotherThreadReleasesTheJoiner)
{
	alt::detail::completion_counter counter;
	counter.add();
	wait_record record;

	const auto handle = alt::start(joining_root(counter, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	{
		const std::jthread completer{[&counter] { counter.complete(); }};
	}

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CompletionCounterTest, AJoinerLinkingAsTheCountReachesZeroIsNeverLost)
{
	// The whole correctness of the type: a joiner that read a non-zero count and a
	// complete() that took it to zero reach the list's mutex in either order, and
	// either order must end with the joiner satisfied -- parked and detached, or
	// never parked at all. A lost wakeup leaves the joiner suspended, which here
	// shows as a join that was never satisfied (the counter's destructor then
	// releases it unsatisfied, so the test fails rather than hangs).
	//
	// The rendezvous is what makes this contended: both threads block on one
	// counter and the second arrival releases both, so complete() and the joiner's
	// ready check land within nanoseconds of each other.
	//
	// Measured rather than argued. With join_awaiter::ready() replaced by a plain
	// return false, so that a joiner always parks whatever the count says and the
	// link's second check can no longer save it, this test failed in 100 of 100
	// repeats in each of 5 runs (500 of 500, assertions relaxed to EXPECT_ for the
	// count), every one by the joiner still parked with resumptions == 0. That is one
	// machine, one compiler, one sample. It also says which interleaving the test
	// reaches: in all 500 the complete() landed before the joiner's link, so what
	// this guards is the second check under the mutex, not the other order, where a
	// joiner links first and detach_all takes it. That order is covered by
	// JoiningABusyCounterSuspendsUntilTheLastCompletion and
	// CompletionFromAnotherThreadReleasesTheJoiner, deterministically.
	//
	// reset_coroutine_test_state() is called per repeat because this loop asserts
	// live_frames.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		wait_record                     record;
		std::atomic<std::size_t>        arrived{0};
		alt::detail::completion_counter counter;
		counter.add();

		{
			std::jthread completer{[&] {
				rendezvous(arrived, 2);
				counter.complete();
			}};

			const auto handle = alt::start(racing_joiner(counter, record, arrived));
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_TRUE(record.satisfied) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

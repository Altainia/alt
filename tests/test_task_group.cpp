#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/scheduler.hpp>
#include <alt/task_group.hpp>
#include <atomic>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// A group is pointed at by every hook its tasks carry, so it can be neither copied
// nor moved.
static_assert(!std::is_copy_constructible_v<alt::task_group>);
static_assert(!std::is_move_constructible_v<alt::task_group>);
static_assert(!std::is_copy_assignable_v<alt::task_group>);
static_assert(!std::is_move_assignable_v<alt::task_group>);
static_assert(std::is_same_v<alt::await_result_t<decltype(std::declval<alt::task_group&>().join())>, alt::wait_result>);

// Three asserts are documented preconditions that no test exercises: the destructor's
// "no live tasks", and spawn's two ownership claims (a start_options::token and a
// start_options::on_completion of the caller's own). The suite has no death tests (no
// EXPECT_DEATH or ASSERT_DEATH anywhere), and introducing the first one for these would
// be a larger change than this component. They compile away under NDEBUG, so no
// release-build behavior is reachable from a Debug preset either. They are therefore a
// known gap in coverage, stated here rather than left silent. The destructor's is the
// one a caller can reach without a mistake, through a cancelled joiner, and
// ACancelledJoinerIsReleasedWithOperationCanceledWhileTasksAreLive pins the state that
// assert exists to catch.

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

	/** A task that parks on a gate nobody need set, and records that it started. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the gate and the counter are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task parked_root(alt::async_event& gate, int& started, int tag)
	{
		const frame_tracker tracker{tag};
		++started;
		const auto result = co_await gate;
		static_cast<void>(result);

		// A suspension point after the wait, so a task released by a stop has somewhere
		// to tear its frames down, and a home scheduler something to defer.
		co_await alt::cancellation_point{};
	}

	/** A task with no suspension point: it runs to completion inside start. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the counter is owned by the calling test, which outlives the synchronous run.
	alt::cancellable_task immediate_root(int& ran)
	{
		++ran;
		co_return;
	}

	/** A task with no suspension point that reports the count it sees from inside spawn. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the group and the slot are owned by the calling test, which joins both threads before leaving their scope.
	alt::cancellable_task observing_root(alt::task_group& group, std::atomic<std::size_t>& observed)
	{
		observed.store(group.outstanding(), std::memory_order_release);
		co_return;
	}

	/** A task whose body throws before any suspension point. */
	alt::cancellable_task throwing_root()
	{
		throw std::runtime_error{"escaped"};
		co_return;
	}

	/** A cancellable joiner that records how its wait ended and then finishes. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the group and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task joining_root(alt::task_group& group, wait_record& record, int tag)
	{
		const frame_tracker    tracker{tag};
		const alt::wait_result result = co_await group.join();
		++record.resumptions;
		record.satisfied = result.has_value();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/**
	 * Completes the task parked on @p gate on one thread while another spawns an
	 * immediate task, released together by a rendezvous so the two reach the group's
	 * counter within nanoseconds of each other. Returns once both threads are done.
	 */
	void release_and_spawn_together(alt::task_group& group, alt::async_event& gate, std::atomic<std::size_t>& observed)
	{
		std::atomic<std::size_t> arrived{0};

		std::jthread completer{[&] {
			rendezvous(arrived, 2);
			gate.set();
		}};

		std::jthread spawner{[&] {
			rendezvous(arrived, 2);
			group.spawn(observing_root(group, observed));
		}};
	}

	/**
	 * One repeat of the race. Returns true when the spawn's raise landed before the
	 * other task's retire, judged by the count the spawned body saw from inside spawn.
	 * reset_coroutine_test_state() is called per repeat because this asserts
	 * live_frames, which the fixture zeroes only once.
	 */
	bool race_once(int repeat)
	{
		reset_coroutine_test_state();

		alt::async_event         gate;
		alt::task_group          group;
		int                      started = 0;
		std::atomic<std::size_t> observed{0};

		group.spawn(parked_root(gate, started, 1));
		EXPECT_EQ(group.outstanding(), 1U) << "repeat " << repeat;

		release_and_spawn_together(group, gate, observed);

		EXPECT_EQ(group.outstanding(), 0U) << "repeat " << repeat;
		EXPECT_EQ(live_frames, 0) << "repeat " << repeat;

		return observed.load(std::memory_order_acquire) == 2U;
	}

	/** Counts the exceptions the group let reach the caller's own handler. */
	// NOLINTNEXTLINE(performance-unnecessary-value-param): the signature is fixed by alt::exception_handler::invoke, which takes the exception_ptr by value.
	void count_escaped(std::exception_ptr error, void* state) noexcept
	{
		if(error)
		{
			++*static_cast<int*>(state);
		}
	}
}

class TaskGroupTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(TaskGroupTest, SpawnStartsTheTaskImmediately)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;

	group.spawn(parked_root(gate, started, 1));

	// Not lazy: the body ran up to its first suspension point before spawn returned.
	EXPECT_EQ(started, 1);
	EXPECT_EQ(live_frames, 1);

	group.request_stop();
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, OutstandingCountsLiveTasks)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;
	ASSERT_EQ(group.outstanding(), 0U);

	group.spawn(parked_root(gate, started, 1));
	EXPECT_EQ(group.outstanding(), 1U);

	group.spawn(parked_root(gate, started, 2));
	EXPECT_EQ(group.outstanding(), 2U);

	group.request_stop();
	EXPECT_EQ(group.outstanding(), 0U);
}

TEST_F(TaskGroupTest, ATaskThatCompletesDecrementsTheCount)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;

	group.spawn(parked_root(gate, started, 1));
	group.spawn(parked_root(gate, started, 2));
	ASSERT_EQ(group.outstanding(), 2U);

	gate.set(); // both run to the end and destroy themselves

	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(group.stop_requested()); // completing is not stopping
}

TEST_F(TaskGroupTest, JoiningAnEmptyGroupIsReadyWithoutSuspending)
{
	alt::task_group group;

	auto awaiter = group.join();
	EXPECT_TRUE(awaiter.await_ready());
	EXPECT_TRUE(awaiter.await_resume().has_value());

	// And end to end: nothing resumes this joiner, so it can only have run through.
	wait_record record;
	const auto  handle = alt::start(joining_root(group, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, JoinCompletesWhenTheLastTaskFinishes)
{
	alt::async_event first_gate;
	alt::async_event second_gate;
	alt::task_group  group;
	int              started = 0;
	wait_record      record;

	group.spawn(parked_root(first_gate, started, 1));
	group.spawn(parked_root(second_gate, started, 2));
	const auto handle = alt::start(joining_root(group, record, 3));
	ASSERT_EQ(record.resumptions, 0);

	first_gate.set();
	EXPECT_EQ(record.resumptions, 0); // one task is still live

	second_gate.set();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, RequestStopCancelsEveryTask)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;

	group.spawn(parked_root(gate, started, 1));
	group.spawn(parked_root(gate, started, 2));
	ASSERT_EQ(live_frames, 2);

	EXPECT_TRUE(group.request_stop());

	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_FALSE(gate.is_set()); // torn down by the stop, not released by the gate
}

TEST_F(TaskGroupTest, StopThenJoinIsTheTeardownSequence)
{
	// The order the documentation gives: request_stop(), co_await join(), then
	// destroy. A home scheduler defers the frames' destruction, so the join really
	// does park rather than finding the group already empty, and it is the drain that
	// finishes the teardown. That is also the check that a caller's home passes
	// through spawn untouched.
	alt::manual_scheduler home;
	alt::async_event      gate;
	alt::task_group       group;
	int                   started = 0;
	wait_record           record;

	group.spawn(parked_root(gate, started, 1), {.home = alt::scheduler_ref{home}});
	group.spawn(parked_root(gate, started, 2), {.home = alt::scheduler_ref{home}});

	group.request_stop();
	const auto handle = alt::start(joining_root(group, record, 3));

	EXPECT_EQ(record.resumptions, 0); // stopped, but the frames are posted, not gone
	EXPECT_EQ(group.outstanding(), 2U);

	EXPECT_EQ(home.drain(), 2U);

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, ATaskParkedOnAPrimitiveIsReleasedByRequestStopAndJoinCompletes)
{
	// The circularity the spec's first revision created: the primitive is destroyed
	// only after the join, and the join waits on a task parked on that primitive. It
	// is finite only because the stop releases the parked waiter through its own stop
	// callback. Declared in the order the documentation prescribes, so the group is
	// destroyed first.
	alt::async_event never_set;
	alt::task_group  group;
	int              started = 0;
	wait_record      record;

	group.spawn(parked_root(never_set, started, 1));
	ASSERT_EQ(group.outstanding(), 1U);

	group.request_stop();
	const auto handle = alt::start(joining_root(group, record, 2));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(never_set.is_set());
}

TEST_F(TaskGroupTest, TheReturnedHandleCancelsOnlyThatTask)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;

	auto first = group.spawn(parked_root(gate, started, 1));
	group.spawn(parked_root(gate, started, 2));
	ASSERT_EQ(group.outstanding(), 2U);

	first.request_stop();

	EXPECT_EQ(group.outstanding(), 1U);
	EXPECT_EQ(live_frames, 1);
	EXPECT_FALSE(group.stop_requested()); // the task's stop does not climb to the group

	group.request_stop();
	EXPECT_EQ(group.outstanding(), 0U);
}

TEST_F(TaskGroupTest, StopRequestedReportsTheGroupsOwnState)
{
	alt::task_group group;
	EXPECT_FALSE(group.stop_requested());

	EXPECT_TRUE(group.request_stop());
	EXPECT_TRUE(group.stop_requested());

	EXPECT_FALSE(group.request_stop()); // only the first call is the one that requested it
	EXPECT_TRUE(group.stop_requested());
}

TEST_F(TaskGroupTest, ATaskSpawnedAfterRequestStopIsCancelledAtItsFirstSuspension)
{
	// The group's token is already stopped, so the linked callback fires at once and
	// the task stops at the first suspension it reaches.
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;

	group.request_stop();
	group.spawn(parked_root(gate, started, 1));

	EXPECT_EQ(started, 1);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, SpawnAfterAJoinHasBegunIsCounted)
{
	// The shape alt::detail::completion_counter's own tests justify themselves with
	// but cannot reach: a joiner parked with one task outstanding, then a further
	// spawn, then both completing. The joiner must stay parked across the second
	// spawn and across the first completion, and be released only by the last.
	alt::async_event first_gate;
	alt::async_event second_gate;
	alt::task_group  group;
	int              started = 0;
	wait_record      record;

	group.spawn(parked_root(first_gate, started, 1));
	const auto handle = alt::start(joining_root(group, record, 2));
	ASSERT_EQ(record.resumptions, 0);

	group.spawn(parked_root(second_gate, started, 3));
	EXPECT_EQ(group.outstanding(), 2U);
	EXPECT_EQ(record.resumptions, 0);

	first_gate.set();
	EXPECT_EQ(group.outstanding(), 1U);
	EXPECT_EQ(record.resumptions, 0); // the second spawn is still live

	second_gate.set();
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, SeveralJoinersAreAllReleased)
{
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;
	wait_record      first;
	wait_record      second;
	wait_record      third;

	group.spawn(parked_root(gate, started, 1));
	const auto first_handle  = alt::start(joining_root(group, first, 2));
	const auto second_handle = alt::start(joining_root(group, second, 3));
	const auto third_handle  = alt::start(joining_root(group, third, 4));
	ASSERT_EQ(live_frames, 4);

	gate.set();

	for(const wait_record* record: {&first, &second, &third})
	{
		EXPECT_EQ(record->resumptions, 1);
		EXPECT_TRUE(record->satisfied);
	}

	EXPECT_EQ(live_frames, 0);
}

TEST_F(TaskGroupTest, AnEscapedExceptionStillReachesTheCallersHandler)
{
	// on_exception is the caller's field; the group must not overwrite it. The throw
	// is before any suspension, so it escapes from inside spawn, and the count must
	// still come back to zero because the frame is destroyed on this path too.
	alt::task_group group;
	int             escaped = 0;

	group.spawn(throwing_root(), {.on_exception = {.invoke = &count_escaped, .state = &escaped}});

	EXPECT_EQ(escaped, 1);
	EXPECT_EQ(group.outstanding(), 0U);
}

TEST_F(TaskGroupTest, ATaskCompletingSynchronouslyInsideSpawnLeavesTheCountCorrect)
{
	// A body with no suspension point runs to completion inside start, which destroys
	// the frame and fires the completion hook there. So the count must be raised
	// before start, or the hook would retire something never counted.
	//
	// What this cannot test is a joiner released from inside spawn, and that is
	// structurally unreachable rather than merely untested: spawn raises the count
	// to at least one before starting, and a synchronous completion returns it to
	// exactly its value before the spawn. A joiner can only be parked if that value
	// was already at least one, so the count after the completion is at least one
	// too and no release occurs. What can be pinned is the half that is reachable: a
	// joiner parked on an earlier task is not released by it.
	//
	// Measured by moving the add() to after the start call in spawn. This test then
	// failed in 100 of 100 runs of itself: the immediate task's retire took the count
	// from one to zero before the add, which released the joiner parked on the earlier
	// task while that task was still live, and the joiner assertion caught it. That is
	// one machine, one compiler, one sample.
	//
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;
	int              ran     = 0;
	wait_record      record;

	group.spawn(parked_root(gate, started, 1));
	const auto handle = alt::start(joining_root(group, record, 2));
	ASSERT_EQ(group.outstanding(), 1U);

	group.spawn(immediate_root(ran));

	EXPECT_EQ(ran, 1);
	EXPECT_EQ(group.outstanding(), 1U);
	EXPECT_EQ(record.resumptions, 0);

	gate.set();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(group.outstanding(), 0U);
}

TEST_F(TaskGroupTest, ACancelledJoinerIsReleasedWithOperationCanceledWhileTasksAreLive)
{
	// The joiner is cancelled through its own chain, not the group's. It reports
	// operation_canceled and the group is left unjoined with a task still live, which
	// is the state the destructor's assert exists to catch, and the one way to reach
	// it without a mistake: an owner whose join can itself be cancelled.
	alt::async_event gate;
	alt::task_group  group;
	int              started = 0;
	wait_record      record;

	group.spawn(parked_root(gate, started, 1));
	auto joiner = alt::start(joining_root(group, record, 2));
	ASSERT_EQ(record.resumptions, 0);

	joiner.request_stop();

	// The released await resumes the body with the error, rather than leaving the
	// joiner parked, so the owner learns the join did not complete.
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(group.outstanding(), 1U);
	EXPECT_EQ(live_frames, 1); // only the spawned task is left, and it is still live

	// Cleaned up the documented way so this test does not trip its own assert.
	group.request_stop();
	EXPECT_EQ(group.outstanding(), 0U);
}

TEST_F(TaskGroupTest, ASpawnRacingACompletionNeverUndercountsOrOvercounts)
{
	// A task completes on one thread while the other spawns a task that finishes
	// synchronously, so spawn's raise and the completion's retire reach the counter
	// within nanoseconds of each other, from a rendezvous that releases both. The
	// count must settle at zero whichever lands first; an undercount would trip
	// complete()'s assert, and an overcount would leave this non-zero.
	//
	// Which interleaving was reached is recorded rather than assumed: the spawned
	// body reads the count from inside spawn, and sees 2 when the spawn's raise
	// landed before the other task's retire, 1 when after. The tally is asserted only
	// to be a full accounting, because how it splits is the scheduler's business.
	// Measured over five runs of the 100 repeats below, the raise landing first gave
	// 69, 97, 61, 81 and 43 against 31, 3, 39, 19 and 57 for the retire landing
	// first, so both orders are reached, though one run came within 3 of reaching only
	// one.
	//
	// Measured against the mutation as well, with the add() moved to after the start
	// call: 100 of 100 runs of this test aborted, each on complete()'s assertion that
	// something is outstanding, reached when the other task's retire landed first and
	// the immediate task retired a count that had not been raised. The assert is what
	// catches it, so under NDEBUG this test would see an undercount only through the
	// final count. Which repeat first aborted was not recorded.
	int spawn_first  = 0;
	int retire_first = 0;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		(race_once(repeat) ? spawn_first : retire_first) += 1;
	}

	EXPECT_EQ(spawn_first + retire_first, race_repeats);
}

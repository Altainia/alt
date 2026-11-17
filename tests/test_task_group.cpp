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
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

using namespace alt_test;

// A group is pointed at by every hook its tasks carry, so it can be neither copied
// nor moved.
static_assert(!std::is_copy_constructible_v<alt::task_group>);
static_assert(!std::is_move_constructible_v<alt::task_group>);
static_assert(!std::is_copy_assignable_v<alt::task_group>);
static_assert(!std::is_move_assignable_v<alt::task_group>);
static_assert(std::is_same_v<alt::await_result_t<decltype(std::declval<alt::task_group&>().join())>, alt::wait_result>);

// Three documented preconditions assert in a debug build, and the TaskGroupDeathTest
// cases at the bottom of this file are what pin them: the destructor's "no live tasks"
// (src/task_group.cpp:12) and spawn's two ownership claims (:17 and :18), a
// start_options::token and a start_options::on_completion of the caller's own. They
// were a stated gap in coverage until issue #29; the three tests close it.
//
// Each of the three was checked by removing just its own assert and confirming that
// exactly its own test failed while the other two passed. That mutation check is this
// work's substitute for a red phase: the asserts predate the tests by many commits, so
// no test here could ever have failed before the code it guards existed, and a test
// that aborted for some unrelated reason would otherwise look identical to one that
// works.
//
// What they do NOT reach, stated rather than left for a reader to assume:
//
// - A spawn running CONCURRENTLY with the destructor, as opposed to either observed on
//   its own. The last-statement rule inside spawn exists for that interleaving, but no
//   rendezvous a test can write pins a point inside a destructor body against another
//   thread's spawn. Uncovered, not covered.
// - Anything under NDEBUG, which compiles all three asserts away. No release-build
//   behavior is reachable from a Debug preset, and the three tests skip there and say
//   which guard is gone. They also skip under ThreadSanitizer, which does not support
//   fork.
//
// The destructor's assert is also the one a caller can reach without a mistake, through
// a cancelled joiner, and ACancelledJoinerIsReleasedWithOperationCanceledWhileTasksAreLive
// pins the state that assert exists to catch.

namespace
{
	/** How many times the contended test repeats its race. */
	constexpr int race_repeats = 100;

	/** The condition every released wait reports. See alt::wait_result. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	// asserts_are_compiled_in, thread_sanitizer_is_active and scoped_death_test_style
	// come from tests/death_test_support.hpp, which records why they live there, and are
	// already visible through this file's using namespace alt_test. The two skip messages
	// stay local, each naming this file's own asserts.

	/** Why a death test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "the destructor's liveness check and spawn's two ownership claims are asserts, and NDEBUG "
	  "compiles them away, so this build has no guard for a death test to observe. That is a real "
	  "gap rather than a configuration detail: a release build gets none of the three checks.";

	/** Why a death test is skipped under ThreadSanitizer. */
	constexpr std::string_view skipped_under_thread_sanitizer =
	  "a GoogleTest death test forks, and ThreadSanitizer does not support fork, so the child would "
	  "die of the tool rather than of the assert. The debug and AddressSanitizer presets run it.";

	/** What one joiner coroutine recorded about its wait, for the test to inspect. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the join reported success. */
		bool satisfied = false;

		/** The error a released join reported. */
		std::error_code error;

		/** The thread the statement after the co_await ran on. */
		std::thread::id resumed_on;
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

	/**
	 * A completion hook of a caller's own, for the death test that pins spawn's claim on
	 * start_options::on_completion. Never called: the assert fires before spawn starts
	 * anything, and the group overwrites the field in any case.
	 *
	 * A named function purely for readability: the call site is already inside an
	 * EXPECT_DEATH statement, and a lambda spelled there would put a function body in the
	 * middle of one. It is NOT a necessity. Substituting
	 * {.invoke = [](void*) noexcept {}} at the call site was measured to compile, the
	 * only diagnostic being that this function had become unused. What genuinely cannot
	 * be written there is a second designated initializer -- see the call site, which
	 * records why.
	 */
	void unused_completion_hook(void* /*state*/) noexcept
	{}

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
		record.resumed_on = std::this_thread::get_id();
		record.satisfied  = result.has_value();

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
	void release_and_spawn_together(alt::task_group& group, alt::async_event& gate, std::atomic<std::size_t>& observed, std::thread::id& spawner_id)
	{
		std::atomic<std::size_t> arrived{0};

		std::jthread completer{[&] {
			rendezvous(arrived, 2);
			gate.set();
		}};

		std::jthread spawner{[&] {
			spawner_id = std::this_thread::get_id();
			rendezvous(arrived, 2);
			group.spawn(observing_root(group, observed));
		}};
	}

	/** What one repeat of the race showed. */
	struct race_outcome
	{
		/** The spawn's raise was seen to precede the other task's retire. A lower bound. */
		bool raise_first = false;

		/** The joiner was released from inside spawn, on the spawning thread. */
		bool released_inside_spawn = false;
	};

	/**
	 * One repeat of the race, with a joiner parked on the first task so a release
	 * that lands inside spawn really happens. The joiner must be released exactly
	 * once whichever order the two land in. reset_coroutine_test_state() is called
	 * per repeat because this asserts live_frames, which the fixture zeroes only once.
	 */
	race_outcome race_once(int repeat)
	{
		reset_coroutine_test_state();

		alt::async_event         gate;
		alt::task_group          group;
		int                      started = 0;
		std::atomic<std::size_t> observed{0};
		std::thread::id          spawner_id;
		wait_record              record;

		group.spawn(parked_root(gate, started, 1));
		const auto handle = alt::start(joining_root(group, record, 2));
		EXPECT_EQ(group.outstanding(), 1U) << "repeat " << repeat;

		release_and_spawn_together(group, gate, observed, spawner_id);

		EXPECT_EQ(group.outstanding(), 0U) << "repeat " << repeat;
		EXPECT_EQ(live_frames, 0) << "repeat " << repeat;
		EXPECT_EQ(record.resumptions, 1) << "repeat " << repeat;
		EXPECT_TRUE(record.satisfied) << "repeat " << repeat;

		return {.raise_first = observed.load(std::memory_order_acquire) == 2U, .released_inside_spawn = record.resumed_on == spawner_id};
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

	// Mutation-checked in a debug build. Moving spawn's add() after start: 0 of 3
	// runs failed, since a task that parks inside start is counted at the same value
	// whichever side of it the add falls, so this test does not cover that mutation.
	// Making complete() release on every completion, which is the ordering this test
	// is for: 3 of 3 runs passed with the drain taken as a whole, and 3 of 3 failed
	// once the frames were run one at a time, as below. The first version of this test
	// guarded nothing against that mutation.
	//
	// One frame at a time, so the join is seen to wait for the last of them and not
	// merely for the drain as a whole.
	EXPECT_TRUE(home.run_one());
	EXPECT_EQ(group.outstanding(), 1U);
	EXPECT_EQ(record.resumptions, 0);

	EXPECT_TRUE(home.run_one());

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(group.outstanding(), 0U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(home.drain(), 0U);
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
	//
	// Mutation-checked in a debug build. Moving spawn's add() after start: 0 of 3 runs
	// failed, for the reason given in StopThenJoinIsTheTeardownSequence: both spawns
	// park inside start. Making complete() release on every completion: this test
	// failed (1 run), as did JoinCompletesWhenTheLastTaskFinishes and
	// ATaskCompletingSynchronouslyInsideSpawnLeavesTheCountCorrect, which is the
	// ordering it exists for.
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
	// A joiner released from inside spawn is not reachable from this single-threaded
	// test, where a synchronous completion returns the count to its value before the
	// spawn and so never reaches zero with a joiner parked. It is reachable with
	// another thread retiring concurrently, and
	// ASpawnRacingACompletionNeverUndercountsOrOvercounts exercises that. What this
	// test pins is the single-threaded half: a joiner parked on an earlier task is
	// not released by the immediate one.
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
	// A joiner is parked on the first task, so the release from inside spawn is
	// actually reached: with one task live and the joiner parked, spawn raises the
	// count, the completer retires the live task, and the immediate task's retire then
	// takes it to zero on the spawning thread. That is why spawn's start call must be
	// its last statement. Every repeat asserts the joiner was released exactly once.
	//
	// Which interleaving was reached is recorded rather than assumed. The spawned
	// body reads the count from inside spawn. Seeing 2 proves the raise preceded the
	// retire; seeing 1 does not prove the opposite, since the retire may also have
	// landed between the raise and the read. The raise_first tally is therefore a
	// lower bound on how often the spawn won. Over six runs of the 100 repeats it was
	// 43, 40, 49, 17, 37 and 65. That both orders occur is shown by the mutation
	// below, not by this tally. The joiner was released on the spawning thread, that
	// is from inside spawn, in 99, 99, 85, 96, 93 and 94 repeats of those same runs.
	//
	// Measured against the mutation, with the add() moved to after the start call, in
	// a debug build and before the joiner was added: 100 of 100 runs of this test
	// aborted, each on complete()'s assert that something is outstanding. That
	// requires at least one repeat per run in which the other task's retire landed
	// before the spawn's raise, so the retire-first order is reached. Which repeat
	// first aborted was not recorded.
	//
	// Under NDEBUG the same mutation was measured too, since the assert is what
	// caught it above. Without the joiner this test failed 0 of 100 runs: complete()
	// is an unconditional fetch_sub, so the count wraps to the maximum and the
	// mutation's trailing add() brings it back to zero, and the final count and
	// live_frames both come out right. It was therefore blind to the mutation without
	// the assert. With the joiner it first failed 4 of 100 runs, live_frames coming
	// out 1, while live_frames was a plain int. The mutated build loses the acq_rel
	// edge that orders the two threads' frame_tracker destructors, so that was
	// almost certainly a lost non-atomic decrement: detection by undefined behavior
	// in the mutated build, not by the property under test. With live_frames made
	// std::atomic<int> the same mutation under NDEBUG failed 0 of 100 runs, so this
	// test does not detect the mutation without the assert. The detection that
	// survives, independent of the assert, lives in
	// ATaskCompletingSynchronouslyInsideSpawnLeavesTheCountCorrect, which failed 100 of
	// 100 runs under NDEBUG through record.resumptions.
	int raise_first           = 0;
	int released_inside_spawn = 0;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		const race_outcome outcome = race_once(repeat);
		raise_first += outcome.raise_first ? 1 : 0;
		released_inside_spawn += outcome.released_inside_spawn ? 1 : 0;
	}

	// What this test proves is the per-repeat expectations in race_once. The tallies
	// are deliberately not asserted: recorded in the run's XML output, nothing more.
	RecordProperty("raise_first", raise_first);
	RecordProperty("released_inside_spawn", released_inside_spawn);
}

// The three death tests that pin src/task_group.cpp's asserts. See the note at the top
// of this file for what they cover and what they do not.
//
// Each puts its whole setup INSIDE the EXPECT_DEATH statement, which runs only in the
// child process. Nothing is constructed in the parent, so there is no parent-side
// lifetime or leak question to reason about, and nothing here depends on the order these
// tests run in.
//
// Each matcher is the assert's own text rather than a generic "Assertion".
// _GLIBCXX_ASSERTIONS is on by default on this distribution's GCC and its Clang alike
// and aborts with a message of its own, so a loose matcher would pass on a death that
// had nothing to do with the precondition. tests/test_waiter_list.cpp records hitting
// exactly that.
//
// These take no fixture, as the suite's other death tests do not, so SetUp never runs
// and reset_coroutine_test_state() is never called for them. They must therefore not
// read live_frames or destruction_order. GoogleTest runs *DeathTest suites ahead of the
// others, and the fixtures that follow reset that state themselves.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, and clang-tidy-19 counts it only when some `if` precedes it in the same function. Measured in this file with the pinned clang-tidy-19 (19.1.7): all three bodies below score 39 against the threshold of 25, and this one scores 0 when its two skip guards are removed and the macro left in place. So the guards are what let the expansion count, and they cannot move -- GTEST_SKIP only skips the function it is written in. tests/test_waiter_list.cpp measured the same figures for an identically shaped body. What this body actually contains is two skip guards and one expectation.
TEST(TaskGroupDeathTest, DestroyingAGroupWithALiveTaskAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << skipped_under_thread_sanitizer;
	}

	// The child re-executes this binary rather than forking a process that has already
	// run every test ahead of this one. GoogleTest recommends this style for a
	// multi-threaded program, and this suite is one, even though these death tests are
	// single-threaded by construction. Scoped so the flag does not outlive this test;
	// see scoped_death_test_style.
	const scoped_death_test_style style{"threadsafe"};

	// DELICATE. The declaration order inside this block is what the test rests on.
	//
	// The gate is declared BEFORE the group, so destruction, being in reverse, destroys
	// the group FIRST -- while parked_root is still suspended on the gate and
	// outstanding() is 1, which is the state the assert exists to catch. Declared after
	// the group, the gate would be destroyed first, release the waiter, let the task run
	// to completion and bring the count to zero, and the destructor would pass.
	//
	// Nothing requests a stop and nothing joins, deliberately: a cancellation_handle has
	// no stop-on-destruction, so discarding spawn's return value leaves the task parked.
	// OutstandingCountsLiveTasks relies on the same thing.
	//
	// The abort lands inside the group's destructor, which is ahead of the gate's, so the
	// task's completion hook never fires against the destroyed group. That dangling hook
	// is the harm the assert documents, and the mutation check measured it rather than
	// taking it on trust: with src/task_group.cpp:12 commented out, this block runs on,
	// the gate's destructor releases the parked task, and the task's completion hook
	// calls m_counter.complete() on the destroyed group, whose joiner list is dead. The
	// process then aborts in waiter_list::detach_all's liveness sentinel instead --
	// "detaching every alt waiter on a destroyed list",
	// include/alt/detail/waiter_list.hpp:618. The mutated build does NOT exit 0; it dies
	// of a different assert one step further on. The other two tests in this file do exit
	// cleanly when mutated, which GoogleTest reports as "failed to die".
	//
	// That measurement is the whole argument for matching this assert's own text below.
	// A matcher of "Assertion", or anything else both aborts share, would have PASSED on
	// the mutated build: the test would have survived the removal of the very assert it
	// exists to pin, and reported nothing.
	EXPECT_DEATH(
	  {
		  alt::async_event gate;
		  int              started = 0;

		  alt::task_group group;
		  group.spawn(parked_root(gate, started, 1));
	  },
	  "destroying an alt::task_group with live tasks");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): same cause, same shape, same measured 39 as the test above, whose suppression carries the measurement.
TEST(TaskGroupDeathTest, SpawningWithACallersStopTokenAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << skipped_under_thread_sanitizer;
	}

	const scoped_death_test_style style{"threadsafe"};

	// A token from a live std::stop_source, which is what makes stop_possible() true and
	// so what the assert tests. A default-constructed std::stop_token would pass it.
	EXPECT_DEATH(
	  {
		  std::stop_source signal;
		  alt::task_group  group;
		  int              ran = 0;

		  const alt::start_options options{.token = signal.get_token()};
		  group.spawn(immediate_root(ran), options);
	  },
	  "alt::task_group supplies its own stop token");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): same cause, same shape, same measured 39 as the test above, whose suppression carries the measurement.
TEST(TaskGroupDeathTest, SpawningWithACallersCompletionHookAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << skipped_under_thread_sanitizer;
	}

	const scoped_death_test_style style{"threadsafe"};

	// Only .invoke is named. alt::completion_handler::state already defaults to nullptr,
	// and the assert reads invoke alone, so naming state would add nothing but a comma --
	// which the preprocessor would read as the end of this macro's first argument. See
	// unused_completion_hook.
	EXPECT_DEATH(
	  {
		  alt::task_group group;
		  int             ran = 0;

		  const alt::start_options options{.on_completion = {.invoke = &unused_completion_hook}};
		  group.spawn(immediate_root(ran), options);
	  },
	  "alt::task_group owns start_options::on_completion");
}

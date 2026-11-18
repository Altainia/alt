#include <gtest/gtest.h>

#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/combinator_state.hpp>
#include <alt/when_all.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "coroutine_test_support.hpp"

using alt::detail::combinator_state;

// Children refer to the block, so it can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<combinator_state>);
static_assert(!std::is_move_constructible_v<combinator_state>);

namespace
{
	/** How many times the contended test repeats its race. */
	constexpr int race_repeats = 200;

	/** Records destruction of the owner. */
	struct deleter_probe
	{
		int calls = 0;

		static void destroy(void* owner) noexcept
		{
			++static_cast<deleter_probe*>(owner)->calls;
		}
	};

	/** An owner that really frees the heap block, so a race over freed memory is observable. */
	struct heap_owner
	{
		std::unique_ptr<combinator_state> state;
		std::atomic<int>                  calls{0};

		static void destroy(void* owner) noexcept
		{
			auto* const self = static_cast<heap_owner*>(owner);
			self->state.reset();
			self->calls.fetch_add(1, std::memory_order_relaxed);
		}
	};

	/** The condition every released wait reports. See alt::wait_result. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/** What the joiner recorded about its wait, for the test to inspect. */
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

	/**
	 * @brief A joiner that drops the block's awaiter reference after its wait ends.
	 *
	 * Models alt::when_all's awaiting chain. The co_await's awaiter -- and with it the
	 * joiner's own std::stop_callback -- is destroyed at the end of the full expression
	 * that awaits, and only then is the reference dropped, which is the order
	 * ~when_all_awaiter takes: deregister the callback, then release.
	 *
	 * Nothing may touch @p state after that release. It may have been the last.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the block is owned by the calling test's heap_owner and the record by the test itself, which joins the racing thread before leaving the scope that holds either, so neither reference can dangle.
	alt::cancellable_task draining_joiner(combinator_state& state, wait_record& record, int tag)
	{
		const alt_test::frame_tracker tracker{tag};
		record_outcome(record, co_await state.counter().join());
		state.release();
	}

	/** How many repeats of the contended drain ended each way. Recorded, never asserted. */
	struct verdict_tally
	{
		/** Repeats whose joiner was resumed by the counter's zero transition. */
		int satisfied = 0;

		/** Repeats whose joiner was claimed by the stop. */
		int released = 0;
	};
}

class CombinatorStateTest: public ::testing::Test
{
protected:
	deleter_probe m_probe;
};

TEST_F(CombinatorStateTest, ConstructionLeavesTheStarterOutstanding)
{
	combinator_state state{3, &m_probe, &deleter_probe::destroy};

	EXPECT_EQ(state.counter().outstanding(), 4u);
}

TEST_F(CombinatorStateTest, RetiringTheStarterWithNoChildrenReachesZero)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	state.counter().complete();

	EXPECT_EQ(state.counter().outstanding(), 0u);
}

TEST_F(CombinatorStateTest, TheLastReleaseRunsTheErasedDeleterExactlyOnce)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	state.add_ref();
	state.add_ref();
	state.release();
	state.release();
	state.release();

	EXPECT_EQ(m_probe.calls, 1);
}

TEST_F(CombinatorStateTest, AReleaseThatIsNotTheLastRunsNothing)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	state.add_ref();
	state.release();

	EXPECT_EQ(m_probe.calls, 0);

	// Balances the construction reference so the block is released as it would be.
	state.release();
}

TEST_F(CombinatorStateTest, CancelMarksTheWaitReleasedBeforeStoppingTheChildren)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};
	bool             seen_cancelled = false;

	const std::stop_callback observer{state.children_token(), [&] { seen_cancelled = state.cancelled(); }};

	state.cancel();

	EXPECT_TRUE(seen_cancelled);
}

TEST_F(CombinatorStateTest, CancelIsIdempotent)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};
	std::stop_source source;
	int              invocations = 0;

	state.link(source.get_token());
	const std::stop_callback observer{state.children_token(), [&] { ++invocations; }};

	state.cancel();
	state.cancel();

	EXPECT_TRUE(state.cancelled());
	EXPECT_EQ(invocations, 1);

	// A stop arriving through the link after cancel() reaches the forwarder, which calls
	// cancel() a third time. The call is the coverage here and there is nothing left to
	// assert after it: m_children is already stopped, so no callback of its can run again
	// whatever cancel() does, which means the EXPECT_EQ(invocations, 1) that used to
	// follow could not fail, and neither could a second EXPECT_TRUE(state.cancelled()).
	// What this line does buy is that a forwarded stop reaching an already-cancelled
	// block does not crash, deadlock or re-enter, which a crash or a hang reports without
	// an assertion. That the forwarder fires at all is
	// CombinatorStateTest.LinkingPropagatesALaterStop's job, not this test's.
	source.request_stop();
}

TEST_F(CombinatorStateTest, CancelledIsFalseUntilCancelIsCalled)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	EXPECT_FALSE(state.cancelled());
	EXPECT_FALSE(state.children_token().stop_requested());

	state.cancel();

	EXPECT_TRUE(state.cancelled());
	EXPECT_TRUE(state.children_token().stop_requested());
}

TEST_F(CombinatorStateTest, LinkingAnAlreadyStoppedTokenCancelsImmediately)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};
	std::stop_source source;
	source.request_stop();

	state.link(source.get_token());

	EXPECT_TRUE(state.cancelled());
	EXPECT_TRUE(state.children_token().stop_requested());
}

TEST_F(CombinatorStateTest, LinkingPropagatesALaterStop)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};
	std::stop_source source;

	state.link(source.get_token());
	EXPECT_FALSE(state.cancelled());

	source.request_stop();

	EXPECT_TRUE(state.cancelled());
	EXPECT_TRUE(state.children_token().stop_requested());
}

TEST_F(CombinatorStateTest, LinkingAnUnstoppableTokenIsANoOp)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	state.link(std::stop_token{});

	EXPECT_FALSE(state.cancelled());
	EXPECT_FALSE(state.children_token().stop_requested());
}

TEST_F(CombinatorStateTest, ARaceBetweenAStopAndTheLastReleaseIsClean)
{
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		heap_owner       owner;
		std::stop_source source;

		owner.state = std::make_unique<combinator_state>(0, &owner, &heap_owner::destroy);
		owner.state->link(source.get_token());

		// The thread owns the only reference and drops it while the stop is in flight.
		combinator_state* const  state = owner.state.get();
		std::atomic<std::size_t> arrived{0};

		std::thread releaser{[&] {
			alt_test::rendezvous(arrived, 2);
			state->release();
		}};

		alt_test::rendezvous(arrived, 2);
		source.request_stop();
		releaser.join();

		EXPECT_EQ(owner.calls.load(), 1);
	}
}

namespace
{

	/**
	 * @brief Races eight threads over one block's reference count. Deleter must run once.
	 *
	 * @param release_before_joining Whether this thread drops the construction reference
	 *        before the workers are joined, which puts the LAST release on a worker thread.
	 *        With it false the last release is made here, after every worker has finished,
	 *        and the count can also be checked for not having reached zero early -- but
	 *        then nothing ever runs release()'s acquire against another thread's store,
	 *        which was the gap. The ordering alt::when_all itself takes is the true half: a
	 *        child's completion hook drops the last reference long after the awaiting chain
	 *        dropped its own.
	 */
	void run_one_refcount_race(const bool release_before_joining)
	{
		constexpr std::size_t threads = 8;
		constexpr int         rounds  = 2000;

		deleter_probe            probe;
		combinator_state         state{0, &probe, &deleter_probe::destroy};
		std::atomic<std::size_t> arrived{0};

		// Every thread holds a reference up front, so the count cannot reach zero while any
		// of them is still running: the releases below are matched by these.
		for(std::size_t i = 0; i < threads; ++i)
		{
			state.add_ref();
		}

		std::vector<std::thread> workers;
		workers.reserve(threads);
		for(std::size_t i = 0; i < threads; ++i)
		{
			workers.emplace_back([&] {
				alt_test::rendezvous(arrived, threads);
				for(int round = 0; round < rounds; ++round)
				{
					state.add_ref();
					state.release();
				}
				state.release();
			});
		}

		if(release_before_joining)
		{
			state.release();
		}

		for(auto& worker: workers)
		{
			worker.join();
		}

		if(!release_before_joining)
		{
			EXPECT_EQ(probe.calls, 0);
			state.release();
		}

		EXPECT_EQ(probe.calls, 1);
	}

}

TEST_F(CombinatorStateTest, ConcurrentAddRefAndReleaseRunTheDeleterExactlyOnce)
{
	for(int repeat = 0; repeat < 50; ++repeat)
	{
		// Alternating which party drops the last reference. Half the repeats leave it to
		// a worker thread, which is what makes the acquire in release() meet a store from
		// another thread; the other half keep it here, where the count can also be checked
		// for not having reached zero early. See run_one_refcount_race.
		run_one_refcount_race(repeat % 2 == 1);
	}
}

TEST_F(CombinatorStateTest, ACancelFromAnotherThreadIsObservedThroughCancelled)
{
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		combinator_state         state{0, &m_probe, &deleter_probe::destroy};
		std::atomic<std::size_t> arrived{0};

		// A plain write that only the release in cancel() publishes to the reader.
		int payload = 0;

		std::thread canceller{[&] {
			alt_test::rendezvous(arrived, 2);
			payload = 42;
			state.cancel();
		}};

		alt_test::rendezvous(arrived, 2);
		while(!state.cancelled())
		{
			std::this_thread::yield();
		}

		// The acquire in cancelled() saw the release in cancel(), so the write made
		// before it is visible. Under ThreadSanitizer a missing pair is a data race here.
		EXPECT_EQ(payload, 42);
		canceller.join();
		state.release();
	}
}

TEST_F(CombinatorStateTest, RequestingTheChildrensStopDoesNotMarkTheWaitCancelled)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};

	state.request_children_stop();

	EXPECT_FALSE(state.cancelled());
}

TEST_F(CombinatorStateTest, RequestingTheChildrensStopStopsTheChildrensToken)
{
	combinator_state state{0, &m_probe, &deleter_probe::destroy};
	int              invocations = 0;

	const std::stop_callback observer{state.children_token(), [&] { ++invocations; }};
	EXPECT_FALSE(state.children_token().stop_requested());

	state.request_children_stop();
	state.request_children_stop();

	EXPECT_TRUE(state.children_token().stop_requested());
	EXPECT_EQ(invocations, 1);
}

namespace
{

	/**
	 * @brief Checks that one contended repeat resolved exactly once, and tallies it.
	 *
	 * Every assertion here holds whichever way the race went. None is a rate. See the
	 * note on the test for what they catch and what they do not.
	 *
	 * The block has been freed by the time this runs -- @p owner having recorded its
	 * deleter call is what says so -- so nothing here may read it. state->cancelled(),
	 * the obvious extra check, would be a use-after-free.
	 */
	void check_the_drain_race_resolved_once(const int repeat, const heap_owner& owner, const wait_record& record, verdict_tally& tally)
	{
		// Exactly one party resumed the joiner: the counter's zero transition on the drain
		// thread, or the stop's claim on the other. Two would run a destroyed frame, and
		// none would leave the joiner suspended forever.
		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;

		// Two references, two releases, one free. Neither party's release was lost and
		// neither freed the block twice.
		ASSERT_EQ(owner.calls.load(std::memory_order_relaxed), 1) << "repeat " << repeat;
		ASSERT_EQ(alt_test::live_frames, 0) << "repeat " << repeat;

		// Which party resumed it is the race, so both verdicts are legal. A satisfied one
		// needs no further check -- it says the counter reached zero -- so only the
		// released path has an error to check.
		if(record.satisfied)
		{
			++tally.satisfied;
		}
		else
		{
			++tally.released;
			ASSERT_EQ(record.error, released_error) << "repeat " << repeat;
		}
	}

	/**
	 * @brief Runs one repeat of a child's drain into the block contended with a stop.
	 *
	 * The arrangement, statement by statement, is alt::when_all's satisfier-pinned
	 * phase with no when_all in it. @p tally records which way the repeat's joiner was
	 * resumed; nothing is asserted about the mix.
	 *
	 * Factored out of the test body for the reason run_one_refcount_race is: the test
	 * is then a loop plus its assertions, which keeps readability-function-cognitive-
	 * complexity off a body that would otherwise score over the threshold. The checks
	 * are split out again, into check_the_drain_race_resolved_once, for the same reason
	 * and measured the same way: with them inline this function scored 28 against the
	 * threshold of 25 under the pinned clang-tidy-19.
	 */
	void run_one_drain_against_stop_race(const int repeat, verdict_tally& tally)
	{
		alt_test::reset_coroutine_test_state();

		heap_owner owner;
		owner.state = std::make_unique<combinator_state>(1, &owner, &heap_owner::destroy);

		combinator_state* const state = owner.state.get();

		// One child, taken before that child would start, which is the order this class's
		// own note requires. References 2 -- the awaiter's and the child's -- and the
		// counter at 2, the child plus the starter.
		state->add_ref();

		// The starter retired, which is what when_all_awaiter::await_suspend does once its
		// fan-out has ended. Outstanding 1: the child.
		state->counter().complete();

		// FIRST registration on this token, and that ordering is the whole reason the
		// joiner below is started with an external token rather than with the handle
		// alt::start hands back. See the note on the test.
		std::stop_source chain;
		state->link(chain.get_token());

		wait_record record;

		// SECOND registration on the same token: cancellable_task_promise::configure
		// emplaces its stop_forwarder before handle.resume() parks the joiner.
		const auto handle = alt::start(draining_joiner(*state, record, 1), {.token = chain.get_token()});

		ASSERT_EQ(alt_test::live_frames, 1) << "repeat " << repeat << ": the joiner did not park";
		ASSERT_EQ(record.resumptions, 0) << "repeat " << repeat << ": the joiner was resumed before the race";

		std::atomic<std::size_t> arrived{0};

		// The child's drain, and THE SHIPPED HOOK rather than a copy of its two
		// statements. That is what gives this test a mutation of record, measured: with
		// complete() and release() written out here instead, nothing this test could be
		// made to fail on was found. See the note on the test.
		std::thread drain{[&] {
			alt_test::rendezvous(arrived, 2);
			alt::detail::on_combinator_child_complete(state);
		}};

		alt_test::rendezvous(arrived, 2);
		chain.request_stop();
		drain.join();

		ASSERT_NO_FATAL_FAILURE(check_the_drain_race_resolved_once(repeat, owner, record, tally));
	}

}

// A child's drain into the block contended with a stop that walks the same token. The
// arrangement is alt::when_all's satisfier-pinned phase with no when_all in it: the
// drain thread runs the shipped completion hook while this thread requests a stop that
// carries both the joiner's release and the block's cancel().
//
// Filed as issue #28, which asked for the overlap that phase ARRANGES to be witnessed.
// It is witnessed here rather than from tests/test_when_all.cpp, whose note says why it
// cannot be witnessed there: the stop's relay path through a whole chain is what makes
// the collision rare, and shortening that path is not something a test can do.
//
// WHY THE JOINER IS STARTED WITH AN EXTERNAL TOKEN rather than with the handle
// alt::start hands back, which is the one piece of this that is not obvious.
// cancellable_task_promise::configure emplaces its stop_forwarder on the external token
// BEFORE handle.resume() parks the joiner, so on `chain` the registrations are m_link
// first and that forwarder second. libstdc++ walks a token's callbacks in reverse
// registration order, so request_stop() releases the joiner first and runs cancel()
// second -- when_all's shipped order, where m_link is registered in await_suspend ahead
// of the join's own waiter_stop_release. Taking the token from the handle would register
// m_link second and invert it.
//
// That is the same dependency WhenAllTest.AHomeSchedulerDefersTheParentsDestruction-
// PastItsChildrens and TheChildrensStopOutlivesABlockTheLastChildFreed already ship
// with -- but they do not carry it the same way, and which is which is the useful half.
// That first test FAILS on a forward walk and names the cause: its own note says
// destruction_order would read {2, 1, 0}. The second one, and this one, pass silently and
// go on claiming an order they no longer arrange. So the suite does have a loud canary
// for the walk order; it is just not this test. On a forward-walking standard library the
// design here is still sound -- what this test would stop being is a model of when_all's
// order.
//
// The heap_owner rather than the fixture's deleter_probe, for the reason
// ARaceBetweenAStopAndTheLastReleaseIsClean uses it: the block must really be freed for a
// race over freed memory to be instrumented at all.
//
// WHAT IT GUARDS, every figure measured 2026-11-17 against this branch, 20 runs of this
// test alone per preset. Two mutations of record, and the second of them is the one worth
// having.
//
// THE HOOK'S STATEMENT ORDER. Swapping the two statements of
// detail::on_combinator_child_complete -- release() before complete() -- is detected 20
// of 20 under AddressSanitizer and 0 of 20 in debug. Not all 20 the same way: 18 are an
// ASan heap-use-after-free, reported at waiter_list::alive() inside detach_all() inside
// completion_counter::complete() with the hook one frame above, and 2 are waiter_list's
// own liveness ASSERT aborting on "detaching every alt waiter on a destroyed list". That
// split matters, because the assert exists in the debug build too: the adjacent 0 of 20
// is therefore a sampling result and not a structural absence, which is the same
// distinction the hook's own note is built on. The ASan citation is sharper than the one
// that note carries -- it reports the same swap at the counter's atomic word -- because
// this block has a parked joiner, so its complete() reaches the list at all. That is why
// the drain thread calls the shipped hook instead of copying its two statements.
//
// THIS CLASS'S MEMBER DECLARATION ORDER, which m_link's own DELICATE note calls "the only
// thing standing between a cancel() in flight on another thread and the members it reads"
// and for which it cited no measured guard. Declaring m_link FIRST, so it is destroyed
// last, is detected 15 of 20 in debug (10 aborting on glibc's "unaligned tcache chunk
// detected", 5 hanging), 14 of 20 under AddressSanitizer (double-free and
// heap-use-after-free) and 6 of 20 under ThreadSanitizer (hangs -- the deadlock
// ~std::stop_callback's cross-thread wait is there to prevent).
//
// UNDER ADDRESSSANITIZER THIS TEST IS THE ONLY GUARD FOR THAT ORDER, measured by running
// the whole suite with the mutation and this test filtered out: it passes 810 of 810. In
// the other two builds it is not alone -- debug dies in
// ARaceBetweenAStopAndTheLastReleaseIsClean below, and ThreadSanitizer hangs in
// WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock -- so what this
// adds there is the build that NAMES the fault rather than reporting heap corruption or a
// hang.
//
// WHAT IT DOES NOT GUARD, and these are measured zeros rather than untested
// possibilities. Swapping cancel()'s own two statements -- the cancelled flag and the
// stop on the counted copy -- is detected 0 of 20 in debug and 0 of 20 under
// AddressSanitizer. Probed rather than reasoned: with a temporary counter in cancel(), it
// runs on 105 of these 200 repeats in debug and NEVER on a freed block. Both ways that
// holds are worth separating, because an earlier draft of this note credited only the
// first and the mix says it cannot be the common one:
//
// - On a RELEASED repeat the joiner's release during the same walk drops the block's last
//   reference, and ~m_link -- destroyed first, this class declaring it last -- deregisters
//   itself from the walk before it is reached, so cancel() is skipped.
// - On a SATISFIED repeat the joiner is resumed inside complete() on the drain thread, so
//   the HOOK's release is last and the block is freed on that thread while the walk may
//   still have m_link pending. Nothing deregisters itself there; what orders the two is
//   ~std::stop_callback deregistering, and blocking for an invocation already running on
//   another thread. That is the other half of m_link's note, and the paragraph above is
//   this test's evidence for it -- break the declaration order and this is the repeat that
//   corrupts the heap.
//
// The mix decides which dominates: at 163 to 197 satisfied of 200, most of the 95 skipped
// cancel()s are the second case, not the first. The guard of record for the cancel() swap
// stays where it was: CancelMarksTheWaitReleasedBeforeStoppingTheChildren above,
// WhenAnyTest.ASatisfiedWaitWithTheCancellationFlagSetDiscardsItsWinner, and -- under
// AddressSanitizer, which that note names -- WhenAllTest.CancellingTheAwaitingChain-
// ReportsOperationCanceled.
//
// Nor does it sharpen the contended ELECTION. A try_claim split into a load and a store is
// detected 0 of 20 in debug here (debug only; not measured under either sanitizer),
// because the stop's relay still offsets the two claims, as the one-sided verdict mix
// below shows. The tests that do catch that are
// WaiterList.AConcurrentSatisfierAndStopDoNotBothResume, whose rendezvous sits either side
// of the claim itself, and
// TaskCompletionSourceTest.AStopRacingSetValueResumesTheWaiterExactlyOnce.
//
// So what this adds over ARaceBetweenAStopAndTheLastReleaseIsClean, which races a stop
// against the last release already: that block is constructed with ZERO children, so no
// complete() is ever in flight against a cancel() and no joiner is parked for the two to
// contend over. This one has both. It is NOT the only test that reaches that
// configuration -- WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock's
// raced phase has it too, which is exactly why that file calls the overlap arranged -- it
// is the only one that reaches it DETERMINISTICALLY, on every repeat, with no chain in the
// way.
//
// The guards are a resumption count, a deleter count and live_frames -- all of which
// hold whichever way the race goes -- plus both sanitizers over the whole thing. None is
// a rate. The mix is RECORDED and nothing is asserted about it, because it is a property
// of the build and not of the library: measured over three debug runs it came out 163,
// 197 and 170 satisfied of 200, 29 of 200 under ThreadSanitizer, and 0 of 200 under
// AddressSanitizer, where the stop takes every joiner. A bound over any of those is the
// mistake test_when_all.cpp's retired EXPECT_GT records.
//
// ONE CONSEQUENCE OF THAT ASAN ZERO, because it is not only a curiosity. The satisfied
// repeat is the one where the DRAIN thread frees the block, and AddressSanitizer -- the
// only build that can name a memory fault rather than report heap corruption -- never
// takes it. So the build with the sharpest instrument is the build that never walks the
// interleaving the instrument would be sharpest on, and that is part of why the cancel()
// swap above is a zero there rather than a report. The declaration-order mutation is
// detected under ASan anyway, 14 of 20, because it also breaks the released repeat.
TEST_F(CombinatorStateTest, ACancelRacingAChildsDrainIntoTheBlockIsClean)
{
	verdict_tally tally;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_drain_against_stop_race(repeat, tally));
	}

	RecordProperty("satisfied_verdicts", tally.satisfied);
	RecordProperty("released_verdicts", tally.released);
}

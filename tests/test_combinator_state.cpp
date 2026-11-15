#include <gtest/gtest.h>

#include <alt/detail/combinator_state.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
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

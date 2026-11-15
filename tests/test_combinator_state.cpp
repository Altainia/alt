#include <gtest/gtest.h>

#include <alt/detail/combinator_state.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>

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
	int              invocations = 0;

	const std::stop_callback observer{state.children_token(), [&] { ++invocations; }};

	state.cancel();
	state.cancel();

	EXPECT_EQ(invocations, 1);
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

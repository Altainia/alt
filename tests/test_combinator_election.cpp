#include <gtest/gtest.h>

#include <alt/detail/combinator_election.hpp>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"

using alt::detail::combinator_election;

namespace
{
	/** How many times the contended test repeats its race. */
	constexpr int race_repeats = 200;

	/** How many threads contend in each repeat. */
	constexpr std::size_t contenders = 8;
}

TEST(CombinatorElectionTest, WinnerIsNoWinnerBeforeAnyElection)
{
	const combinator_election election;

	EXPECT_EQ(election.winner(), combinator_election::no_winner);
}

TEST(CombinatorElectionTest, TheFirstElectionWinsAndLaterOnesLose)
{
	combinator_election election;

	EXPECT_TRUE(election.elect(2));
	EXPECT_FALSE(election.elect(0));
	EXPECT_FALSE(election.elect(2));
	EXPECT_FALSE(election.elect(5));
	EXPECT_EQ(election.winner(), 2U);
}

TEST(CombinatorElectionTest, PositionZeroCanWin)
{
	combinator_election election;

	EXPECT_TRUE(election.elect(0));
	EXPECT_FALSE(election.elect(1));
	EXPECT_EQ(election.winner(), 0U);
}

TEST(CombinatorElectionTest, ConcurrentElectionsYieldExactlyOneWinner)
{
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		combinator_election       election;
		std::atomic<std::size_t>  arrived{0};
		std::atomic<int>          winners{0};
		std::atomic<std::size_t>  winning_position{combinator_election::no_winner};
		std::vector<std::jthread> threads;

		threads.reserve(contenders);
		for(std::size_t position = 0; position < contenders; ++position)
		{
			threads.emplace_back([&, position] {
				alt_test::rendezvous(arrived, contenders);
				if(election.elect(position))
				{
					winning_position.store(position, std::memory_order_relaxed);
					winners.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}

		threads.clear();

		ASSERT_EQ(winners.load(), 1) << "repeat " << repeat;
		EXPECT_EQ(election.winner(), winning_position.load());
	}
}

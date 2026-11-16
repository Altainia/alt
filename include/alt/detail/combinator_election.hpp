#pragma once

#include <atomic>
#include <cstddef>

namespace alt::detail
{

	/**
	 * @brief Decides which of several racing children completed first.
	 *
	 * Every child calls elect() with its own position when it completes; exactly one
	 * call returns true. Neither copyable nor movable, as it is shared by reference.
	 */
	class combinator_election
	{
	public:
		/** The value winner() reports while no child has been elected. */
		static constexpr std::size_t no_winner = static_cast<std::size_t>(-1);

		/**
		 * @brief Tries to become the winner.
		 *
		 * One compare_exchange_strong from no_winner to @p position, so two children
		 * cannot both win: a load followed by a store would let both see no_winner.
		 *
		 * Acquire-release on success and acquire on failure, matching
		 * waiter_list::try_claim and for the reason its note gives: the winner's
		 * earlier writes are published by the election, and a loser that observes
		 * another child's win also observes what that child published before electing.
		 *
		 * @param position The caller's index. Must not equal no_winner.
		 * @return true for exactly one caller.
		 */
		[[nodiscard]] bool elect(std::size_t position) noexcept
		{
			std::size_t expected = no_winner;
			return m_winner.compare_exchange_strong(expected, position, std::memory_order_acq_rel, std::memory_order_acquire);
		}

		/**
		 * @brief The elected position, or no_winner if none yet.
		 *
		 * Only meaningful once the completion counter has drained: an election can
		 * land after any particular read, so a read taken earlier proves nothing.
		 */
		[[nodiscard]] std::size_t winner() const noexcept
		{
			return m_winner.load(std::memory_order_acquire);
		}

	private:
		std::atomic<std::size_t> m_winner{no_winner};
	};

}

#pragma once

#include <atomic>
#include <cassert>
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
		 * Acquire-release on success and acquire on failure, the orderings of
		 * waiter_list::try_claim, and chosen for the reason its note gives: an election
		 * needs both halves so that a winner's later writes cannot be hoisted above it
		 * and a party that observes another's win also observes what that party did
		 * before claiming. Cite that note for the ORDERING CHOICE only.
		 *
		 * Its publication argument does not carry over, and the two differ. try_claim's
		 * note says its CAS does not hand over the node's contents, because the handle is
		 * written before link() takes the mutex and read by parties who take that same
		 * mutex; there the CAS is purely an election. Here a winner writes its slot
		 * BEFORE calling elect(), that write is ordered before the successful exchange's
		 * release, and winner() loads with acquire, so a reader that goes through
		 * winner() and sees a position synchronizes with that release and sees the slot.
		 * This word is a publication channel as well as an election.
		 *
		 * A combinator's awaiter should not rely on that, though. The election decides
		 * WHICH slot to read; what makes any slot's contents visible to the awaiting
		 * chain is completion_counter::complete()'s zero transition, which resumes the
		 * joiner through resume_claimed(m_joiners.detach_all(), true), and detach_all()
		 * takes the list's mutex. Rely on the counter's mutex for visibility.
		 *
		 * These orderings are inherited and are NOT exercised by this class's tests.
		 * The concurrent test counts winners, which a relaxed election would also get
		 * right, and cannot observe a publication either way. That is a limit rather than
		 * an oversight: x86 is strongly ordered, so a relaxed election would pass on this
		 * machine whatever the argument, and ThreadSanitizer sees no unsynchronized
		 * access because the test publishes nothing through the election.
		 *
		 * @param position The caller's index. Must not equal no_winner; asserted.
		 * @return true for exactly one caller.
		 */
		[[nodiscard]] bool elect(std::size_t position) noexcept
		{
			assert(position != no_winner && "electing the no_winner sentinel would be indistinguishable from no election");

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

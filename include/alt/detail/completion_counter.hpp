#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>

namespace alt::detail
{

	/**
	 * @brief A count of outstanding items and the coroutines waiting for it to reach zero.
	 *
	 * The accounting behind "is everything I started gone?": add() records items,
	 * complete() retires them, and awaiting join() suspends until none is
	 * outstanding. It lives in detail/ because more than one primitive is expected to
	 * share it, not because it is hidden.
	 *
	 * Awaiting join() yields alt::wait_result. It holds a value when the count
	 * reached zero (or was already there), and std::errc::operation_canceled when the
	 * wait ended without that: the awaiting chain was cancelled, or this counter was
	 * destroyed with joiners still parked.
	 *
	 * Not single-use, unlike std::latch and alt::async_latch: reaching zero does not
	 * retire the counter, and a later add() makes it outstanding again. Any number of
	 * coroutines may join at once, and all of them are released together.
	 *
	 * What this does not promise: an add() racing a complete() that reaches zero may
	 * release a joiner while the new item is outstanding. An owner that is tearing
	 * down stops adding first, and then the question has one answer.
	 *
	 * Every member but the destructor is safe to call from any thread. Joiners are
	 * resumed on the thread whose complete() reached zero, inside it, so complete()
	 * must not be called with a lock held that a joiner might take. Like
	 * alt::async_event, neither copyable nor movable, and a joiner's body may destroy
	 * the counter: complete() reads nothing from it after resuming.
	 *
	 * Two things a joiner's body may not do, both inherited from
	 * detail::wait_awaiter, which cannot enforce them, and both shared with
	 * alt::async_latch. It may not destroy another joiner of the same counter: the
	 * releasing complete() takes the whole queue before it resumes anyone, so a
	 * frame destroyed from inside one resumption may be one that call is already
	 * committed to resuming. And a coroutine parked here may not have its frame
	 * destroyed from outside, because the node it is parked on lives in that frame;
	 * destroying this counter instead releases them all, which is the supported
	 * teardown. Both matter because any number of coroutines may join at once.
	 */
	class completion_counter
	{
	public:
		/** Constructs a counter with nothing outstanding. */
		completion_counter() noexcept = default;

		/**
		 * @brief Releases every joiner still parked, unsatisfied.
		 *
		 * For the reason alt::async_event::~async_event gives, and by the same
		 * explicit release rather than leaving it to the list's own destructor.
		 */
		~completion_counter()
		{
			resume_claimed(m_joiners.detach_all(), false);
		}

		completion_counter(const completion_counter&)            = delete;
		completion_counter& operator=(const completion_counter&) = delete;
		completion_counter(completion_counter&&)                 = delete;
		completion_counter& operator=(completion_counter&&)      = delete;

		/** Records @p n more outstanding items. */
		void add(std::size_t n = 1) noexcept
		{
			// acq_rel rather than relaxed, though the count alone needs only relaxed:
			// read-modify-writes of one atomic are totally ordered whatever their
			// ordering. The acquire half is there so that a spawner whose add() follows
			// a complete() in that order also sees what the completing party wrote
			// before it retired; the release half publishes the spawner's own writes
			// to the complete() that eventually retires this item. No test depends on
			// either half, and weakening it was not measured.
			m_outstanding.fetch_add(n, std::memory_order_acq_rel);
		}

		/**
		 * @brief Retires one outstanding item, releasing every joiner if it was the last.
		 *
		 * Completing with nothing outstanding is a precondition violation, asserted in
		 * debug builds. A release build is left with the decrement already made: the
		 * count has wrapped to the maximum, so the counter is permanently busy, no
		 * join() completes through it again, and the owner can never be joined.
		 *
		 * Joiner bodies run on this thread before this returns.
		 */
		void complete() noexcept
		{
			const std::size_t before = m_outstanding.fetch_sub(1, std::memory_order_acq_rel);
			assert(before != 0 && "completing an alt::detail::completion_counter with nothing outstanding");

			if(before != 1)
			{
				return;
			}

			// DELICATE. Read this before changing it.
			//
			// The count is decremented outside the list's mutex and the list is taken
			// only when that decrement reached zero. waiter_list has no conditional
			// detach, so what makes this lose no wakeup is the mutex, not the order of
			// the statements:
			//
			// - join_awaiter::ready() is outstanding() == 0, and wait_awaiter consults it
			//   a second time under the list's mutex as the condition on the link. So a
			//   joiner parks only if it read a non-zero count while holding that mutex.
			// - A complete() that reached zero then acquires the same mutex. Either the
			//   joiner got there first, in which case it may be parked and this
			//   detach_all takes it; or this got there first, in which case the joiner's
			//   later acquisition synchronizes with this release, it sees zero, and it
			//   does not park at all.
			// - There is no third interleaving, which is why no conditional detach is
			//   needed and why detach_all is called only on the zero transition. A
			//   complete() that did not reach zero has nothing to release: any joiner it
			//   could race read a non-zero count.
			//
			// Of any number of concurrent calls, exactly one fetch_sub returns 1, so
			// exactly one call reaches the walk below and the rest return above.
			//
			// The resumption is last, and nothing may be read from *this after it: a
			// resumed joiner runs its body, and that may destroy whatever owns this
			// counter.
			resume_claimed(m_joiners.detach_all(), true);
		}

		/** How many items are outstanding. A snapshot: it may be stale on return. */
		[[nodiscard]] std::size_t outstanding() const noexcept
		{
			return m_outstanding.load(std::memory_order_acquire);
		}

		/**
		 * @brief One suspended joiner on a completion_counter.
		 *
		 * Constructed by join() and lives in the awaiting coroutine's frame.
		 * detail::wait_awaiter holds everything but the two questions this answers:
		 * whether the wait is over, and where to park.
		 */
		class join_awaiter: public wait_awaiter<join_awaiter>
		{
		public:
			/** The counter must outlive the suspension. */
			explicit join_awaiter(completion_counter& counter) noexcept: m_counter(&counter)
			{}

			/**
			 * Whether nothing is outstanding. Also the condition the link is made under,
			 * with the list's mutex held, so it reads one atomic: no lock of its own, no
			 * call back into the list, nothing resumed. See detail::wait_awaiter for the
			 * whole of that contract.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_counter->outstanding() == 0;
			}

			/** The queue this joiner parks in. */
			[[nodiscard]] waiter_list& list() const noexcept
			{
				return m_counter->m_joiners;
			}

		private:
			completion_counter* m_counter;
		};

		/** Awaits the count reaching zero, yielding satisfied-or-released. */
		[[nodiscard]] join_awaiter join() noexcept
		{
			return join_awaiter{*this};
		}

	private:
		/** Read without the list's mutex by a joiner deciding whether to wait at all, and under it as the condition on the link. */
		std::atomic<std::size_t> m_outstanding{0};

		waiter_list m_joiners;
	};

}

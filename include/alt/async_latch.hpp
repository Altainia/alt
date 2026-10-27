#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cstddef>

namespace alt
{

	/**
	 * @brief A single-use countdown that coroutines await.
	 *
	 * Constructed with a count; count_down() lowers it, and the call that brings it
	 * to zero releases every waiter at once. Awaiting a latch whose count is already
	 * zero does not suspend. The latch does not reset: once at zero it stays there,
	 * which is what makes it safe to await from a coroutine that arrives late.
	 *
	 * Awaiting yields std::expected<void, std::error_code>. It holds a value when
	 * the count reached zero, and std::errc::operation_canceled when the wait ended
	 * without that: the awaiting chain was cancelled, or this latch was destroyed
	 * with waiters still parked.
	 *
	 * Every member but the destructor is safe to call from any thread, and
	 * concurrently: of several threads counting down at once, exactly one is the call
	 * that reaches zero, and it alone releases the waiters. Destruction is not, as
	 * ever, and in particular a latch must not be destroyed while a cancellation is
	 * in flight against one of its waiters. Resumption happens on the thread that
	 * reached zero, inside count_down(), which therefore runs waiter bodies;
	 * co_await alt::resume_on(scheduler) in a waiter that needs to be somewhere
	 * specific.
	 *
	 * Neither copyable nor movable: the awaiters inside suspended coroutine frames
	 * hold pointers to it, and no copy or move could fix those up.
	 *
	 * Two things a waiter's body may not do, both shared with async_event. It may not
	 * destroy another waiter of the same latch: the releasing count_down() takes the
	 * whole queue before it resumes anyone, so a frame destroyed from inside one
	 * resumption may be one that call is already committed to resuming. And a
	 * coroutine parked here may not have its frame destroyed from outside, because
	 * the node it is parked on lives in that frame; destroying this latch instead
	 * releases them all, which is the supported teardown.
	 *
	 * Destroying the latch itself from a resumed waiter's body is a different thing
	 * and is allowed: the releasing count_down() publishes the zero and takes the
	 * queue first, and the resumption is the last thing it does, so nothing is left to
	 * be read afterwards. It carries one qualification a waiter cannot check for
	 * itself, the same one async_semaphore::release carries and for the same reason,
	 * that call resuming one multi-node chain exactly as that one does. It is
	 * committed to resuming every waiter it took, so a waiter that destroys the latch
	 * may be followed by siblings that are still resumed -- correctly, each having
	 * been won before anything ran -- into an object that no longer exists. Those
	 * siblings must not touch the latch after their wait returns. Since no waiter can
	 * tell whether it is the last of a chain, in practice a waiter that destroys the
	 * latch must be one whose siblings do not go back to it, which is what a shutdown
	 * looks like: every waiter is leaving.
	 *
	 * That first rule is why count_down() can resume its waiters from one multi-node
	 * chain (detach_all) instead of one detach_one at a time: nothing in the library
	 * lets one waiter own a sibling's frame, because there is no when_all or race
	 * combinator and a task is lazy and owns only the child it awaits. Revisit the
	 * day such a combinator lands: async_event and async_latch would then both need
	 * detach_one.
	 */
	class async_latch
	{
	public:
		/**
		 * Constructs a latch that releases its waiters after @p count calls to
		 * count_down(), counted by their arguments. Zero is a latch that is already
		 * released. The count must not be negative.
		 */
		explicit async_latch(std::ptrdiff_t count);

		/**
		 * @brief Releases every waiter still parked, unsatisfied.
		 *
		 * A backstop rather than the expected path, like waiter_list's own: a waiter
		 * nobody resumes stays suspended forever and its frame is never freed, so
		 * releasing them beats leaking them. Each one sees the released error, which
		 * is how it tells this apart from the count actually reaching zero.
		 */
		~async_latch();

		async_latch(const async_latch&)            = delete;
		async_latch& operator=(const async_latch&) = delete;
		async_latch(async_latch&&)                 = delete;
		async_latch& operator=(async_latch&&)      = delete;

		/**
		 * @brief Lowers the count by @p n, releasing every waiter if that reaches zero.
		 *
		 * Counting down past zero is harmless rather than an error: the count stops at
		 * zero, and only the call that actually brought it there releases anyone, so a
		 * latch is never released twice. @p n must not be negative; zero changes
		 * nothing.
		 *
		 * Waiter bodies run on this thread before this returns when this call reaches
		 * zero, so it must not be called with a lock held that a waiter might take.
		 */
		void count_down(std::ptrdiff_t n = 1);

		/** True when the count has reached zero, so an await would not suspend. */
		[[nodiscard]] bool try_wait() const noexcept;

		/**
		 * @brief One suspended waiter on an async_latch.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's
		 * frame. detail::wait_awaiter holds everything but the two questions this
		 * answers: whether the wait is over, and where to park.
		 */
		class latch_awaiter: public detail::wait_awaiter<latch_awaiter>
		{
		public:
			/** The latch must outlive the suspension. */
			explicit latch_awaiter(async_latch& latch) noexcept: m_latch(&latch)
			{}

			/**
			 * Whether the wait is already over. Also the condition the link is made
			 * under, with the list's mutex held, so it reads one atomic: no lock of its
			 * own, no call back into the list, nothing resumed. See
			 * detail::wait_awaiter for the whole of that contract.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_latch->try_wait();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_latch->m_waiters;
			}

		private:
			async_latch* m_latch;
		};

		/** Awaits the latch, yielding satisfied-or-released. */
		[[nodiscard]] auto operator co_await() noexcept
		{
			return latch_awaiter{*this};
		}

	private:
		/**
		 * Atomic because it is read without the list's mutex by a waiter deciding
		 * whether to wait at all, and under it as the condition on the link, which is
		 * what keeps a count_down that reaches zero between those two steps from
		 * leaving a waiter parked on a released latch. Written only by compare-exchange
		 * so that exactly one caller observes the step onto zero. See count_down.
		 */
		std::atomic<std::ptrdiff_t> m_count;

		detail::waiter_list m_waiters;
	};

}

#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cstddef>

namespace alt
{

	/**
	 * @brief A counting semaphore that coroutines await.
	 *
	 * Holds a count of permits. Awaiting the semaphore takes one, suspending the
	 * coroutine if there is none to take; release() puts permits back, handing them
	 * to parked waiters before the count. try_acquire() is the same acquisition
	 * without the suspension, for a caller that would rather do something else.
	 *
	 * Awaiting yields std::expected<void, std::error_code>. It holds a value when
	 * the wait came away with a permit, and std::errc::operation_canceled when the
	 * wait ended without one: the awaiting chain was cancelled, or this semaphore was
	 * destroyed with waiters still parked. A wait that ends that way has consumed
	 * nothing, so the count is exactly what it would have been had the wait never
	 * happened.
	 *
	 * Acquiring is not an RAII operation: nothing here hands back a guard, and
	 * release() is an independent call that any thread may make, because a
	 * semaphore's acquire and release are frequently asymmetric -- one party admits
	 * work, another retires it. The consequence is that the caller is responsible for
	 * checking the result and for releasing what it acquired. await_resume() is
	 * [[nodiscard]] so that dropping the result is a diagnostic, and that is worth
	 * knowing the exact strength of: Clang and clang-tidy warn on a dropped co_await
	 * result, while GCC does not apply -Wunused-result to a co_await expression at
	 * all. See detail::cancellation_checked::await_resume, where that was measured. So
	 * on a GCC build a caller that ignores the result and carries on without a permit
	 * is diagnosed by nothing.
	 *
	 * Fairness, as far as it goes: permits are handed to parked waiters in the order
	 * they parked, and a release never adds to the count while a parked waiter could
	 * take the permit instead. So a waiter that has parked cannot be overtaken by a
	 * later arrival. What is not ordered is the race to park: a caller that reads an
	 * empty count and a caller that calls try_acquire() a moment later are two
	 * unsynchronized attempts at the same count, and the second may win.
	 *
	 * Every member but the destructor is safe to call from any thread, and
	 * concurrently: of several threads releasing at once, each permit reaches exactly
	 * one waiter or the count, and of several callers reaching for one permit exactly
	 * one takes it. Destruction is not, as ever, and in particular a semaphore must
	 * not be destroyed while a cancellation is in flight against one of its waiters.
	 * Resumption happens on the thread that released the permit, inside release(),
	 * which therefore runs waiter bodies; co_await alt::resume_on(scheduler) in a
	 * waiter that needs to be somewhere specific.
	 *
	 * Neither copyable nor movable: the awaiters inside suspended coroutine frames
	 * hold pointers to it, and no copy or move could fix those up.
	 *
	 * Three things a waiter's body may not do. It may not destroy another waiter of
	 * the same semaphore, because the node a parked waiter is on lives in that
	 * waiter's frame and nothing withdraws it from the queue; see
	 * detail::waiter_list::detach_all for why one detach at a time does not make that
	 * safe. Nor may a coroutine parked here have its frame destroyed from outside, for
	 * the same reason; destroying this semaphore instead releases them all, which is
	 * the supported teardown. And it may not destroy the semaphore itself from inside a
	 * resumption that release(n) performed with permits still to hand out -- see
	 * release().
	 */
	class async_semaphore
	{
	public:
		/**
		 * Constructs a semaphore holding @p initial permits, so the first @p initial
		 * acquisitions do not suspend. The count must not be negative.
		 */
		explicit async_semaphore(std::ptrdiff_t initial);

		/**
		 * @brief Releases every waiter still parked, without a permit.
		 *
		 * A backstop rather than the expected path, like waiter_list's own: a waiter
		 * nobody resumes stays suspended forever and its frame is never freed, so
		 * releasing them beats leaking them. Each one sees the released error, which is
		 * how it tells this apart from actually having been handed a permit.
		 */
		~async_semaphore();

		async_semaphore(const async_semaphore&)            = delete;
		async_semaphore& operator=(const async_semaphore&) = delete;
		async_semaphore(async_semaphore&&)                 = delete;
		async_semaphore& operator=(async_semaphore&&)      = delete;

		/**
		 * @brief Hands out @p n permits, to parked waiters first and the count after.
		 *
		 * Each permit goes to the oldest waiter that can still be won, or, once there is
		 * no such waiter, into the count where the next acquisition will find it. @p n
		 * must not be negative; zero changes nothing. The total number of permits a
		 * semaphore holds must stay within std::ptrdiff_t.
		 *
		 * Waiter bodies run on this thread before this returns, so this must not be
		 * called with a lock held that a waiter might take. One waiter is resumed per
		 * permit, each one before the next permit is handed out, which means a waiter
		 * resumed here can re-await this semaphore and be given one of the permits this
		 * same call has left.
		 *
		 * That last point carries a precondition this cannot enforce. For @p n of one
		 * the resumption is the last thing this function does, so the waiter may do
		 * anything, including destroying this semaphore. For larger @p n this call comes
		 * back to the semaphore after each resumption, to decide where the next permit
		 * goes, so a waiter resumed by release(n > 1) must not destroy the semaphore.
		 */
		void release(std::ptrdiff_t n = 1);

		/**
		 * @brief Takes one permit if there is one, without suspending.
		 *
		 * Returns true when a permit was taken, and then the caller owns it and must
		 * release it. False takes nothing and leaves the count where it was; it never
		 * goes negative.
		 *
		 * Takes no lock, which is what also makes this the awaiter's ready check, where
		 * it is called with the waiting queue's mutex held. See semaphore_awaiter.
		 */
		[[nodiscard]] bool try_acquire() noexcept;

		/**
		 * How many permits are in the count. A permit handed straight to a parked waiter
		 * is not counted here and never was: release() gives it away without it passing
		 * through the count. Never negative, and a snapshot like any other observation of
		 * shared state: a concurrent acquisition may have spent the permit by the time
		 * the caller reads this.
		 */
		[[nodiscard]] std::ptrdiff_t available() const noexcept;

		/**
		 * @brief One suspended waiter on an async_semaphore.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's frame.
		 * detail::wait_awaiter holds everything but the two questions this answers:
		 * whether the wait is over, and where to park.
		 */
		class semaphore_awaiter: public detail::wait_awaiter<semaphore_awaiter>
		{
		public:
			/** The semaphore must outlive the suspension. */
			explicit semaphore_awaiter(async_semaphore& semaphore) noexcept: m_semaphore(&semaphore)
			{}

			/**
			 * @brief Whether the wait is already over, which for a semaphore means taking
			 *        the permit that ends it.
			 *
			 * DELICATE. Read this before changing it.
			 *
			 * The first ready check in the library that consumes what it checks for, which
			 * is sound for exactly the reason detail::wait_awaiter documents: this is
			 * consulted at most twice per wait, once in await_ready and once as the
			 * condition on the link, and the second call happens only if the first returned
			 * false. A wait therefore takes at most one permit, whichever of the two calls
			 * found it, and a permit taken by the second call refuses the link and reports
			 * the wait satisfied without parking. Consult it a third time anywhere and a
			 * single wait could spend two permits.
			 *
			 * The second call runs with the queue's mutex held, so what this must not do is
			 * fixed: no lock of its own, no call back into the list, nothing resumed.
			 * try_acquire() is a compare-exchange loop over one atomic and does none of
			 * those. The tempting fair version, @c available() > 0 && list().empty(),
			 * self-deadlocks on that same mutex.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_semaphore->try_acquire();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_semaphore->m_waiters;
			}

		private:
			async_semaphore* m_semaphore;
		};

		/** Awaits a permit, yielding acquired-or-released. */
		[[nodiscard]] auto operator co_await() noexcept
		{
			return semaphore_awaiter{*this};
		}

	private:
		/**
		 * Atomic because it is both read and taken from without the list's mutex, by a
		 * caller in try_acquire() and by a waiter deciding whether to wait at all, and
		 * again under that mutex as the condition on the link. That second reader is what
		 * keeps a release landing between a waiter's two checks from leaving a permit in
		 * the count with the waiter parked beside it.
		 *
		 * Only ever lowered by a compare-exchange that saw it above zero, so it never
		 * goes negative: a waiter that gets no permit takes nothing, rather than
		 * borrowing one and giving it back.
		 */
		std::atomic<std::ptrdiff_t> m_permits;

		detail::waiter_list m_waiters;
	};

}

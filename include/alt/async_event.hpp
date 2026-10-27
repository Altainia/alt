#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>

namespace alt
{

	/**
	 * @brief A manual-reset event that coroutines await.
	 *
	 * Awaiting an unset event suspends the coroutine; set() releases every waiter
	 * at once and leaves the event set, so a waiter arriving afterwards does not
	 * suspend at all. reset() clears it again. That stickiness is the point: a
	 * result that is already available must not be missed by whoever asks for it
	 * second.
	 *
	 * Awaiting yields std::expected<void, std::error_code>. It holds a value when
	 * the event released the wait, and std::errc::operation_canceled when the wait
	 * ended without that: the awaiting chain was cancelled, or this event was
	 * destroyed with waiters still parked.
	 *
	 * Every member but the destructor is safe to call from any thread, and
	 * concurrently: two threads setting it resume each parked waiter once between
	 * them, not once each. Destruction is not, as ever, and in particular an event
	 * must not be destroyed while a cancellation is in flight against one of its
	 * waiters. Resumption happens on the thread that satisfied the wait, inside
	 * set(), which therefore runs waiter bodies; co_await alt::resume_on(scheduler)
	 * in a waiter that needs to be somewhere specific.
	 *
	 * Neither copyable nor movable: the awaiters inside suspended coroutine frames
	 * hold pointers to it, and no copy or move could fix those up.
	 *
	 * Two things a waiter's body may not do. It may not destroy another waiter of
	 * the same event: set() takes the whole queue before it resumes anyone, so a
	 * frame destroyed from inside one resumption may be one this set() is already
	 * committed to resuming. And a coroutine parked here may not have its frame
	 * destroyed from outside, because the node it is parked on lives in that frame;
	 * destroying this event instead releases them all, which is the supported
	 * teardown.
	 */
	class async_event
	{
	public:
		/** Constructs an event, set or unset. */
		explicit async_event(bool set = false) noexcept;

		/**
		 * @brief Releases every waiter still parked, unsatisfied.
		 *
		 * A backstop rather than the expected path, like waiter_list's own: a waiter
		 * nobody resumes stays suspended forever and its frame is never freed, so
		 * releasing them beats leaking them. Each one sees the released error, which
		 * is how it tells this apart from the event actually being set.
		 */
		~async_event();

		async_event(const async_event&)            = delete;
		async_event& operator=(const async_event&) = delete;
		async_event(async_event&&)                 = delete;
		async_event& operator=(async_event&&)      = delete;

		/**
		 * @brief Sets the event and releases every waiter, satisfied.
		 *
		 * Idempotent: setting an event that is already set releases whatever is
		 * parked, which is nothing, because nothing can park on a set event.
		 *
		 * Waiter bodies run on this thread before this returns, so this must not be
		 * called with a lock held that a waiter might take.
		 */
		void set();

		/** Clears the event, so a waiter arriving afterwards parks again. */
		void reset() noexcept;

		/** True when the event is set. */
		[[nodiscard]] bool is_set() const noexcept;

		/**
		 * @brief One suspended waiter on an async_event.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's
		 * frame. detail::wait_awaiter holds everything but the two questions this
		 * answers: whether the wait is over, and where to park.
		 */
		class event_awaiter: public detail::wait_awaiter<event_awaiter>
		{
		public:
			/** The event must outlive the suspension. */
			explicit event_awaiter(async_event& event) noexcept: m_event(&event)
			{}

			/**
			 * Whether the wait is already over. Also the condition the link is made
			 * under, so it reads one atomic and takes no lock.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_event->is_set();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_event->m_waiters;
			}

		private:
			async_event* m_event;
		};

		/** Awaits the event, yielding satisfied-or-released. */
		[[nodiscard]] auto operator co_await() noexcept
		{
			return event_awaiter{*this};
		}

	private:
		/**
		 * Read without the list's mutex by a waiter deciding whether to wait at all,
		 * and under it as the condition on the link, which is what keeps a set() that
		 * lands between those two steps from leaving a waiter parked on a set event.
		 * See waiter_list::link.
		 */
		std::atomic<bool> m_set;

		detail::waiter_list m_waiters;
	};

}

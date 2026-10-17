#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace alt::detail
{

	/**
	 * @brief The queue every alt scheduler is built on.
	 *
	 * Holds the items, the mutex, the condition variable and the wake protocol, so
	 * that protocol exists once rather than once per scheduler. It already shipped
	 * one lost-wakeup defect when it was written by hand; a second hand-written
	 * copy would be a second chance at the same mistake.
	 *
	 * Two wake signals exist here and they are not interchangeable:
	 *
	 * - wake() is a consumable one-shot latch. It releases one waiter, which
	 *   consumes it. This is what alt::manual_scheduler exposes.
	 * - close() is permanent. It releases every waiter and keeps them released,
	 *   and a waiter afterwards returns empty-handed only once the queue is also
	 *   empty. A pool with several worker threads needs this; a latch consumed by
	 *   one worker leaves the rest parked.
	 *
	 * close() drains rather than discards: a closed queue still hands out the items
	 * it holds, which is what lets a pool's destructor finish its queued work.
	 */
	template<typename Item>
	class work_queue
	{
	public:
		work_queue()                             = default;
		work_queue(const work_queue&)            = delete;
		work_queue& operator=(const work_queue&) = delete;
		work_queue(work_queue&&)                 = delete;
		work_queue& operator=(work_queue&&)      = delete;
		~work_queue()                            = default;

		/**
		 * @brief Queues an item.
		 *
		 * @return True when the queue went from empty to non-empty, which is what an
		 *         edge-triggered wake hook fires on. A caller's correctness can depend
		 *         on this value, so it is part of the contract rather than a hint.
		 */
		bool push(Item w)
		{
			bool was_empty = false;
			{
				const std::scoped_lock lock{m_mutex};
				was_empty = m_queue.empty();
				m_queue.push_back(w);
			}
			m_signal.notify_one();
			return was_empty;
		}

		/** Takes the oldest item without blocking. False when the queue was empty. */
		bool try_pop(Item& out)
		{
			const std::scoped_lock lock{m_mutex};
			if(m_queue.empty())
			{
				return false;
			}
			out = m_queue.front();
			m_queue.pop_front();
			return true;
		}

		/**
		 * @brief Blocks until an item is available, then takes it.
		 *
		 * Returns false without an item when the wait ended for another reason: the
		 * optional stop flag became true, wake() released it, or the queue is closed
		 * and empty. A caller that cares which can ask is_closed().
		 *
		 * The stop flag joins the wait condition rather than being signalled through
		 * this queue, so a caller waiting on something this queue knows nothing about
		 * is released without any state being left behind here.
		 */
		bool pop_or_wait(Item& out, const std::atomic<bool>* stop)
		{
			std::unique_lock lock{m_mutex};

			m_signal.wait(lock, [this, stop] {
				return !m_queue.empty() || m_woken || m_closed || (stop != nullptr && stop->load(std::memory_order_acquire));
			});

			// Consume the latch on every path. Leaving it armed after handing out an
			// item would make a later call return false on a stale signal, which is the
			// defect this header exists to retire.
			m_woken = false;

			if(m_queue.empty())
			{
				return false;
			}

			out = m_queue.front();
			m_queue.pop_front();
			return true;
		}

		/** How many items are queued. */
		[[nodiscard]] std::size_t size() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_queue.size();
		}

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_queue.empty();
		}

		/**
		 * @brief Releases one waiter, which consumes the signal.
		 *
		 * A one-shot latch: it stays armed until a waiter takes it, so a wake nobody
		 * consumes is observable by the next call. Use notify() when the condition
		 * being waited on lives outside this queue.
		 */
		void wake()
		{
			{
				const std::scoped_lock lock{m_mutex};
				m_woken = true;
			}
			m_signal.notify_all();
		}

		/**
		 * @brief Prompts waiters to re-test their condition, arming nothing.
		 *
		 * Acquiring the mutex is what closes the lost-wakeup window: a waiter that has
		 * already evaluated its condition holds the mutex until it is genuinely parked,
		 * so a notification cannot arrive before it parks and then be missed.
		 */
		void notify()
		{
			{
				const std::scoped_lock lock{m_mutex};
			}
			m_signal.notify_all();
		}

		/**
		 * @brief Permanently releases every waiter, draining rather than discarding.
		 *
		 * A closed queue still hands out the items it holds; waiters return
		 * empty-handed only once it is also empty. So a pool's workers finish the
		 * queued work and then exit, which is what makes destruction drain. Idempotent.
		 */
		void close()
		{
			{
				const std::scoped_lock lock{m_mutex};
				m_closed = true;
			}
			m_signal.notify_all();
		}

		/** True once close() has been called. */
		[[nodiscard]] bool is_closed() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_closed;
		}

	private:
		mutable std::mutex      m_mutex;
		std::condition_variable m_signal;
		std::deque<Item>        m_queue;
		bool                    m_woken  = false;
		bool                    m_closed = false;
	};

}

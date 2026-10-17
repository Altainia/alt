#pragma once

#include <alt/detail/work_queue.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <cstddef>
#include <thread>

namespace alt
{

	/** @brief Configuration for an alt::run_loop. Every field is optional. */
	struct run_loop_options
	{
		/**
		 * @brief Invoked when a post makes the queue non-empty, so a host loop can be nudged.
		 *
		 * A host supplies something that wakes its own loop: a posted Win32 message, a
		 * write to a Wayland eventfd. The loop knows nothing about platforms.
		 *
		 * It fires on the empty-to-non-empty transition, not on every post. Per-post
		 * delivery would mean a platform wakeup for every coroutine resumption, and
		 * would make a hook that posts to its own loop recurse without end.
		 *
		 * Invoked on the posting thread, outside the queue's lock. It may not throw,
		 * for the same reason a work item may not. Empty means run() waits on its own
		 * condition variable instead.
		 */
		work_item on_wake{};
	};

	/**
	 * @brief A serial executor for one thread, in either of two modes.
	 *
	 * It is an alt::scheduler, so post() works from any thread. Beyond that it serves
	 * two worlds with one type:
	 *
	 * - It owns its thread: run() drains and blocks when idle until stop().
	 * - It is a guest of a platform loop: run_pending() drains a bounded batch and
	 *   returns, called from a Win32 message handler or a Wayland dispatch.
	 *
	 * **stop() gates run() and nothing else.** A stopped loop still runs items
	 * through drain(), run_pending(), run_one() and run_one_or_wait(), and its
	 * destructor still drains. Were stop() a gate on execution, the ordinary
	 * shutdown of stopping and then destroying would silently discard every queued
	 * coroutine resumption and cancelled-chain teardown.
	 *
	 * Serial across threads, reentrant on one. Two threads must not be inside its
	 * running members at once, which is asserted in debug builds. One thread nesting
	 * them is supported, because that is how a modal flow is written: a work item
	 * that calls sync_wait(awaitable, loop) drives the loop from inside a run()
	 * already below it on the stack. Each nested level consumes stack, and a nested
	 * run_pending() computes its own batch bound.
	 *
	 * Neither copyable nor movable: it owns a thread's loop, and references to it are
	 * held by alt::scheduler_ref and by awaiters inside suspended coroutine frames.
	 */
	class run_loop
	{
	public:
		run_loop() = default;

		/** Constructs a loop with a host wake hook. */
		explicit run_loop(run_loop_options options) noexcept;

		run_loop(const run_loop&)            = delete;
		run_loop& operator=(const run_loop&) = delete;
		run_loop(run_loop&&)                 = delete;
		run_loop& operator=(run_loop&&)      = delete;

		/** Drains whatever is still queued. See the note on stop() above. */
		~run_loop();

		/** Queues an item and nudges the host if a hook was supplied. Any thread. */
		void post(work_item w);

		/** Runs queued items until the queue is empty. Returns how many ran. */
		std::size_t drain();

		/** Drains, blocking when idle, until stop(). */
		void run();

		/** Makes run() return after the current item. Sticky and idempotent. */
		void stop() noexcept;

		/** True once stop() has been called. */
		[[nodiscard]] bool stopped() const noexcept;

		/**
		 * @brief Runs a bounded batch without blocking. Returns how many ran.
		 *
		 * The queue's length is taken once on entry, so an item that posts more work
		 * cannot keep control here. Draining until empty would livelock a host loop.
		 */
		std::size_t run_pending();

		/** Runs the oldest queued item. False when the queue was empty. */
		bool run_one();

		/** Blocks until an item can be run, then runs it. See alt::manual_scheduler. */
		bool run_one_or_wait(const std::atomic<bool>* stop = nullptr);

		/** Prompts a blocked waiter to re-evaluate its condition, arming nothing. */
		void notify_waiters();

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const;

	private:
		/** Debug-only guard for the serial precondition. Permits nesting on one thread. */
		class entry_guard
		{
		public:
			explicit entry_guard(run_loop& loop) noexcept;
			entry_guard(const entry_guard&)            = delete;
			entry_guard& operator=(const entry_guard&) = delete;
			entry_guard(entry_guard&&)                 = delete;
			entry_guard& operator=(entry_guard&&)      = delete;
			~entry_guard() noexcept;

		private:
			run_loop* m_loop;
		};

		detail::work_queue<work_item> m_queue;
		run_loop_options              m_options{};
		std::atomic<bool>             m_stopped{false};
		std::atomic<std::thread::id>  m_owner;
		std::atomic<int>              m_depth{0};
	};

}

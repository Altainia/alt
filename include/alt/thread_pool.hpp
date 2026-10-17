#pragma once

#include <alt/detail/work_queue.hpp>
#include <alt/scheduler.hpp>
#include <cstddef>
#include <expected>
#include <system_error>
#include <thread>
#include <vector>

namespace alt
{

	/**
	 * @brief A fixed set of threads running work items from one queue.
	 *
	 * An alt::scheduler, so alt::resume_on hops onto it and a cancelled chain can
	 * name it as its home. Deliberately plain: one shared queue, no work stealing
	 * and no priorities. Two pools cost nothing to create, so a consumer who must
	 * keep long jobs away from latency-sensitive work makes two rather than asking
	 * one pool to arbitrate.
	 *
	 * **Construction goes through create().** Starting a thread can fail, and a
	 * constructor cannot report that without throwing, so the factory returns
	 * std::expected and the error carries the reason the OS gave.
	 *
	 * The pool is neither copyable nor movable, because it owns threads and
	 * references to it are held by alt::scheduler_ref and by awaiters inside
	 * suspended coroutine frames. That is compatible with std::expected: the factory
	 * constructs in place and the caller binds a reference, whose address is stable.
	 * The non-movability then propagates to the expected itself, so the pool cannot
	 * be moved out of the variable it was created in and those references cannot be
	 * left dangling. The price is that a pool cannot be returned from a function or
	 * stored in a container.
	 *
	 * **Destruction drains.** Queued items may be coroutine resumptions, or a
	 * cancelled chain's teardown posted here as its home scheduler, and discarding
	 * either leaks frames with no diagnostic. The cost is that shutdown takes as long
	 * as the queued work; a loud wait is better than a silent leak.
	 *
	 * The pool must outlive every suspension that targets it, which is
	 * alt::scheduler_ref's existing contract. Draining covers queued items, not
	 * references held elsewhere.
	 */
	class thread_pool
	{
	private:
		/** Makes the constructor unreachable except through create(). */
		struct passkey
		{
			explicit passkey() = default;
		};

	public:
		/**
		 * @brief Starts a pool, or reports why it could not.
		 *
		 * @param threads How many threads to start. Zero means
		 *                std::thread::hardware_concurrency(), and the result is clamped
		 *                to at least one, including when that query returns zero.
		 *
		 * A thread that fails to start fails the whole call: threads already started
		 * are released and joined, and the error is returned. A pool silently running
		 * with fewer threads than asked for is a performance mystery later, and this
		 * factory exists so that can be reported instead of guessed at.
		 */
		[[nodiscard]] static std::expected<thread_pool, std::error_code> create(std::size_t threads = 0);

		/** Called only by create(). The passkey is private, so nothing else can. */
		thread_pool(passkey /*unused*/, std::size_t threads);

		thread_pool()                              = delete;
		thread_pool(const thread_pool&)            = delete;
		thread_pool& operator=(const thread_pool&) = delete;
		thread_pool(thread_pool&&)                 = delete;
		thread_pool& operator=(thread_pool&&)      = delete;

		/** Runs whatever is still queued, then joins. See the note above. */
		~thread_pool();

		/** Queues an item. Safe from any thread, including a pool thread. */
		void post(work_item w);

		/**
		 * @brief Runs queued items on the calling thread. Returns how many ran.
		 *
		 * For quiescing a set of schedulers that post to each other, which no
		 * destructor can do alone: call drain() on each in turn until every one
		 * reports empty, then destroy them.
		 */
		std::size_t drain();

		/** How many threads this pool started. Always at least one. */
		[[nodiscard]] std::size_t thread_count() const noexcept;

		/** True when nothing is queued. In-flight items are not counted. */
		[[nodiscard]] bool empty() const;

	private:
		void worker();

		detail::work_queue<work_item> m_queue;
		std::vector<std::jthread>     m_threads;
	};

}

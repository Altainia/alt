#pragma once

#include <alt/async_semaphore.hpp>
#include <alt/io_backend.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace alt
{
	class thread_pool;
}

namespace alt::detail
{

	/**
	 * @brief The part of a file I/O service that an in-flight operation co-owns.
	 *
	 * The third of the three things an operation block holds on to, and the one that is
	 * least obviously needed. The buffer and the descriptor are the memory and the handle
	 * a backend is still touching; this is the accounting a *resumption* is still going to
	 * touch.
	 *
	 * The reason it has to be refcounted rather than owned by alt::file_io is the shape of
	 * shutdown. Every completion path ends in a post to the completion scheduler, and for
	 * the consumer this component was written for that scheduler is the loop thread --
	 * which is also the thread that destroys the service. A destructor that waited for
	 * those posts to RUN would deadlock on the spot with nothing pumping the loop, so
	 * ~file_io waits only for the backend to go quiet and lets already-posted resumptions
	 * stay queued. Each of those resumptions releases a backpressure permit, and the permit
	 * lives here. If this object died with alt::file_io, that release would touch freed
	 * memory on the one path shutdown is designed to allow. So alt::file_io is a handle to
	 * this rather than the owner of it, and the last operation to finish is what frees it.
	 *
	 * Thread safety: every member function is safe to call from any thread, and
	 * concurrently. The three pointers and the semaphore are fixed at construction; the two
	 * atomics and the quiescence pair carry everything that changes. The data members are
	 * public because this is an engine-internal type with no invariant of its own to
	 * protect beyond what the member functions below already express -- but the two atomics
	 * and the quiescence pair are theirs: reach for enter_backend(), leave_backend() and
	 * wait_until_quiet() rather than touching at_backend, quiet_mutex or quiet by hand,
	 * because the notification is only lost-wakeup-free in the combination they implement.
	 *
	 * Neither copyable nor movable: operation blocks and alt::file_io both hold
	 * std::shared_ptrs to one of these, and the semaphore inside it is immovable anyway.
	 */
	struct io_shared
	{
		/**
		 * @brief Builds the shared state of one file I/O service.
		 *
		 * @param seam          The backend every operation is submitted through. Must
		 *                      outlive this object, which for alt::file_io means it owns it.
		 * @param home          Where every operation's awaiting coroutine is resumed and
		 *                      where every permit is released. Must not be empty, and must
		 *                      outlive every operation; alt::file_io::create is where that
		 *                      is checked rather than asserted on an I/O thread.
		 * @param pool          The borrowed pool the thread-pool arm runs on, or null when
		 *                      the service has no pool arm (a test backend). Not owned.
		 * @param max_in_flight How many operations may be at the backend at once. Must be
		 *                      positive: a zero bound would park the first operation on a
		 *                      permit that can never exist.
		 */
		io_shared(io_backend& seam, const scheduler_ref home, thread_pool* const pool, const std::ptrdiff_t max_in_flight):
		  slots(max_in_flight),
		  backend(&seam),
		  completion(home),
		  workers(pool)
		{
			assert(max_in_flight > 0 && "an alt file I/O service with no in-flight slots");
			assert(home && "an alt file I/O service with no completion scheduler");
		}

		~io_shared() = default;

		io_shared(const io_shared&)            = delete;
		io_shared& operator=(const io_shared&) = delete;
		io_shared(io_shared&&)                 = delete;
		io_shared& operator=(io_shared&&)      = delete;

		/**
		 * @brief The backpressure permits, one per operation that may be at the backend.
		 *
		 * Awaited by the operation's own coroutine before it submits, and released by the
		 * operation block's posted resumption -- never by the backend's callback, because
		 * alt::async_semaphore::release() resumes a parked waiter inline on the releasing
		 * thread and that would put the next operation's submission on an I/O thread. See
		 * alt::detail::io_awaiter, which states the invariant that buys.
		 */
		async_semaphore slots;

		/** The seam. Not owned, and fixed at construction; see the constructor. */
		io_backend* const backend;

		/** Where resumptions and permit releases happen. Not owned; fixed at construction. */
		const scheduler_ref completion;

		/**
		 * The borrowed pool the thread-pool arm runs on, or null.
		 *
		 * Held here rather than inside the backend because the escape hatch and the
		 * operations no kernel interface covers (a directory listing, an arbitrary
		 * callable) run on a pool on every platform, whichever backend is in front of it.
		 */
		thread_pool* const workers;

		/**
		 * How many operations the backend has been handed and not yet completed.
		 *
		 * Raised before each submission and lowered by each completion, so it counts
		 * exactly the window in which a backend callback can still fire. Reach for it
		 * through enter_backend()/leave_backend()/wait_until_quiet().
		 */
		std::atomic<std::size_t> at_backend{0};

		/**
		 * False once the service has begun shutting down and will accept no more
		 * submissions.
		 *
		 * Submitting after that is a precondition violation rather than a reportable
		 * error: the service must outlive every operation started on it, which is the
		 * same contract alt::thread_pool and alt::scheduler_ref already state.
		 */
		std::atomic<bool> accepting{true};

		/** Guards the predicate wait_until_quiet() sleeps on. See wait_until_quiet(). */
		std::mutex quiet_mutex;

		/** Signalled when at_backend reaches zero. See wait_until_quiet(). */
		std::condition_variable quiet;

		/**
		 * @brief An awaitable for one backpressure permit.
		 *
		 * The first of an operation's two suspension points, and a plain
		 * alt::async_semaphore wait, so cancellation while parked here costs nothing and
		 * involves no kernel: the awaiting chain is released with
		 * std::errc::operation_canceled and the operation never reaches the backend at
		 * all. The interesting cancellation window is only the submitted phase.
		 *
		 * The permit the wait comes away with is then handed to alt::detail::io_awaiter,
		 * which releases it. A caller that acquires a permit and does not go on to await an
		 * io_awaiter must release it itself.
		 */
		[[nodiscard]] async_semaphore::semaphore_awaiter acquire_slot() noexcept
		{
			return async_semaphore::semaphore_awaiter{slots};
		}

		/** @brief Counts one more operation at the backend. Called before each submission. */
		void enter_backend() noexcept
		{
			at_backend.fetch_add(1, std::memory_order_acq_rel);
		}

		/**
		 * @brief Counts one fewer, and wakes wait_until_quiet() when none are left.
		 *
		 * Takes quiet_mutex around the notification even though the count is atomic,
		 * because a notify that lands between a waiter's predicate check and its sleep is
		 * lost otherwise.
		 */
		void leave_backend() noexcept
		{
			if(at_backend.fetch_sub(1, std::memory_order_acq_rel) != 1)
			{
				return;
			}

			// The lock is what makes the wakeup reliable rather than the count: a waiter
			// evaluates its predicate holding quiet_mutex, so taking it here orders this
			// notification either before that evaluation or after the waiter is asleep.
			const std::scoped_lock lock{quiet_mutex};
			quiet.notify_all();
		}

		/** @brief Records that no further submission will be accepted. Idempotent. */
		void stop_accepting() noexcept
		{
			accepting.store(false, std::memory_order_release);
		}

		/** @brief Whether submissions are still being accepted. */
		[[nodiscard]] bool accepting_submissions() const noexcept
		{
			return accepting.load(std::memory_order_acquire);
		}

		/**
		 * @brief Blocks until no operation is outstanding at the backend.
		 *
		 * What ~file_io waits for, and the whole of it: when this returns, no backend
		 * callback will fire again and the kernel holds none of the service's memory.
		 * Resumptions those callbacks already posted may still be queued on the completion
		 * scheduler, which is safe precisely because this object is refcounted.
		 *
		 * Must not be called from a thread the backend completes on, nor from a worker of
		 * the borrowed pool, since it would then be waiting for work queued behind itself.
		 */
		void wait_until_quiet()
		{
			std::unique_lock lock{quiet_mutex};
			quiet.wait(lock, [this] { return at_backend.load(std::memory_order_acquire) == 0; });
		}
	};

}

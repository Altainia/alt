#pragma once

#include <alt/async_semaphore.hpp>
#include <alt/io_backend.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <mutex>
#include <system_error>
#include <utility>

namespace alt::detail
{
	class io_permit;
	class io_permit_awaiter;
	class io_awaiter;
	struct io_operation;

	/**
	 * @brief The backpressure permits: an async_semaphore only the permit machinery can touch.
	 *
	 * Awaiting the semaphore directly yields a permit held in a plain wait result, which is
	 * lost for good if the chain is cancelled before the next await, because the cancellation
	 * check destroys the frame ahead of the awaiter that would have released it. Releasing it
	 * directly can return a permit that was never taken. Both halves of that contract are
	 * therefore private here: io_permit_awaiter takes a permit and wraps it in an io_permit,
	 * and io_permit, io_operation and io_awaiter are the only code that puts one back.
	 *
	 * A permit is put back by the operation's own code only: the io_permit destructor for one
	 * never submitted, io_awaiter::start's failure paths, or the operation block's posted
	 * resumption for one that was. Never by the backend's callback, because
	 * async_semaphore::release() resumes a parked waiter inline on the releasing thread and
	 * that would put the next operation's submission on an I/O thread. See
	 * alt::detail::io_awaiter, which states the invariant that buys.
	 */
	class permit_pool
	{
	public:
		/** @brief A pool of @p permits, all free. */
		explicit permit_pool(const std::ptrdiff_t permits): m_semaphore(permits)
		{}

		/** @brief How many permits are free right now. A snapshot: it takes and returns nothing. */
		[[nodiscard]] std::ptrdiff_t available() const noexcept
		{
			return m_semaphore.available();
		}

		/**
		 * @brief How many operations are parked waiting for a permit.
		 *
		 * An upper bound, like waiter_list::size(): an operation already claimed by a cancel
		 * but not yet unlinked still counts. Reached through a throwaway awaiter's list
		 * because the semaphore keeps its queue private; the awaiter is never awaited, so this
		 * takes no permit.
		 */
		[[nodiscard]] std::size_t parked()
		{
			return async_semaphore::semaphore_awaiter{m_semaphore}.list().size();
		}

	private:
		friend class io_permit;
		friend class io_permit_awaiter;
		friend class io_awaiter;
		friend struct io_operation;

		/** @brief The semaphore to wait on. Only io_permit_awaiter, which wraps the permit it gets, calls this. */
		[[nodiscard]] async_semaphore& semaphore() noexcept
		{
			return m_semaphore;
		}

		/** @brief Puts one permit back, resuming a parked waiter inline on the calling thread. Exactly once per permit. */
		void release()
		{
			m_semaphore.release();
		}

		async_semaphore m_semaphore;
	};

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
		 * @param max_in_flight How many operations may be at the backend at once. Must be
		 *                      positive: a zero bound would park the first operation on a
		 *                      permit that can never exist.
		 */
		io_shared(io_backend& seam, const scheduler_ref home, const std::ptrdiff_t max_in_flight):
		  slots(max_in_flight),
		  backend(&seam),
		  completion(home)
		{
			assert(max_in_flight > 0 && "an alt file I/O service with no in-flight slots");
			assert(home && "an alt file I/O service with no completion scheduler");
		}

		~io_shared() = default;

		io_shared(const io_shared&)            = delete;
		io_shared& operator=(const io_shared&) = delete;
		io_shared(io_shared&&)                 = delete;
		io_shared& operator=(io_shared&&)      = delete;

		/** @brief The backpressure permits, one per operation that may be at the backend. */
		permit_pool slots;

		/**
		 * The seam. Not owned, and fixed at construction; see the constructor.
		 *
		 * A RESUMPTION MUST NEVER TOUCH THIS. alt::file_io may own the backend this points
		 * at and destroys it when ~file_io finishes, which can be before a queued
		 * resumption runs. Only the submission and cancel paths, which the destructor's
		 * wait for quiescence orders ahead of that destruction, may dereference it.
		 */
		io_backend* const backend;

		/** Where resumptions and permit releases happen. Not owned; fixed at construction. */
		const scheduler_ref completion;

		/**
		 * How many operations the backend has been handed and not yet completed.
		 *
		 * Raised before each submission and lowered by each completion, so it counts
		 * exactly the window in which a backend callback can still fire. Reach for it
		 * through enter_backend()/leave_backend()/wait_until_quiet().
		 */
		std::atomic<std::size_t> at_backend{0};

		/**
		 * False once the service has begun shutting down.
		 *
		 * Nothing refuses a submission on this flag: it is consulted only by an assert
		 * in the submission path, so in a build with NDEBUG it is stored and never read.
		 * Submitting after shutdown has begun is a precondition violation rather than a
		 * reportable error: the service must outlive every operation started on it, which is the
		 * same contract alt::thread_pool and alt::scheduler_ref already state.
		 */
		std::atomic<bool> accepting{true};

		/** Guards the predicate wait_until_quiet() sleeps on. See wait_until_quiet(). */
		std::mutex quiet_mutex;

		/** Signalled when at_backend reaches zero. See wait_until_quiet(). */
		std::condition_variable quiet;

		/**
		 * @brief How many operations are parked waiting for a permit.
		 *
		 * See permit_pool::parked(), which holds the semaphore this reads; ~file_io asserts
		 * this is zero. There is no member that returns an awaiter on the permits: see permit_pool
		 * for why only alt::detail::acquire_permit() may take one.
		 */
		[[nodiscard]] std::size_t parked_waiters()
		{
			return slots.parked();
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
		 * the pool the backend runs on, since it would then be waiting for work queued
		 * behind itself.
		 */
		void wait_until_quiet()
		{
			std::unique_lock lock{quiet_mutex};
			quiet.wait(lock, [this] { return at_backend.load(std::memory_order_acquire) == 0; });
		}
	};

	/**
	 * @brief One backpressure permit, owned by whoever holds this object.
	 *
	 * A permit has exactly one owner at every moment, and the type is what enforces it. It
	 * can only be obtained by awaiting acquire_permit(), so a permit cannot be held
	 * without having been waited for, and it can only be spent by moving it into an
	 * alt::detail::io_awaiter, so a permit cannot be submitted without being owned.
	 *
	 * That closes the window the earlier two-phase contract could not. Cancellation checks
	 * run ahead of every suspension point and destroy the frame before the next awaiter's
	 * await_suspend is called, so a permit that sat in a plain wait result between the
	 * permit wait and the awaiter was lost for good when a stop landed there. Now the
	 * permit is an object: wherever the frame dies, the frame's objects die with it, and
	 * this one's destructor puts the permit back, whether it sits in the frame's
	 * std::expected, in the io_awaiter temporary of the pending co_await, or in a
	 * constructor's argument when a later member throws. Once the awaiter starts the
	 * operation it calls hand_over(), after which the posted resumption releases the
	 * permit exactly once, on the completion scheduler.
	 *
	 * Movable, not copyable. Not thread-safe: it is touched by the frame that owns it and
	 * by the awaiter that frame is awaiting, never concurrently.
	 */
	class io_permit
	{
	public:
		/** @brief An empty permit: holds nothing, releases nothing. A moved-from permit is this. */
		io_permit() noexcept = default;

		io_permit(const io_permit&)            = delete;
		io_permit& operator=(const io_permit&) = delete;

		/** @brief Takes the permit; the source is left empty. */
		io_permit(io_permit&& other) noexcept: m_owner(std::move(other.m_owner))
		{}

		/** @brief Puts back the permit this one holds, if any, and takes the other's. */
		io_permit& operator=(io_permit&& other) noexcept
		{
			if(this != &other)
			{
				if(m_owner)
				{
					m_owner->slots.release();
				}
				m_owner = std::move(other.m_owner);
			}
			return *this;
		}

		/** @brief Puts the permit back if it is still held. */
		~io_permit()
		{
			if(m_owner)
			{
				m_owner->slots.release();
			}
		}

		/** @brief True while the permit is held and this object would release it. */
		[[nodiscard]] bool holds() const noexcept
		{
			return m_owner != nullptr;
		}

		/**
		 * @brief The operation was submitted: its resumption releases the permit from here on.
		 *
		 * Called by io_awaiter, from the start of its own start(), which then owns every
		 * path including the ones that throw.
		 */
		void hand_over() noexcept
		{
			m_owner.reset();
		}

	private:
		/** Only the awaiter that has just taken a permit from the semaphore may make one. */
		friend class io_permit_awaiter;

		explicit io_permit(std::shared_ptr<io_shared> owner) noexcept: m_owner(std::move(owner))
		{}

		/** A counted reference, so the state the release touches outlives a frame destroyed on any thread. */
		std::shared_ptr<io_shared> m_owner;
	};

	/**
	 * @brief What acquire_permit() returns: awaiting it yields a permit, or why none came.
	 *
	 * The error is std::errc::operation_canceled, as for every alt wait. It is never
	 * delivered to a caller in the current design: a stop abandons the awaiting chain
	 * rather than resuming it, and the state this waits on outlives every operation parked
	 * on it. A caller still tests it, so its code stays total.
	 */
	class io_permit_awaiter
	{
	public:
		/** @brief Waits on @p owner's slots. */
		explicit io_permit_awaiter(std::shared_ptr<io_shared> owner): m_inner{owner->slots.semaphore()}, m_owner(std::move(owner))
		{}

		/** @brief Ready when a permit is free. */
		[[nodiscard]] bool await_ready()
		{
			return m_inner.await_ready();
		}

		/** @brief Parks on the semaphore. Templated over the promise so cancellation reaches it. */
		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h)
		{
			return m_inner.await_suspend(h);
		}

		/** @brief The permit, or std::errc::operation_canceled if the wait ended without one. */
		[[nodiscard]] std::expected<io_permit, std::error_code> await_resume()
		{
			const wait_result waited = m_inner.await_resume();
			if(!waited)
			{
				return std::unexpected(waited.error());
			}
			return io_permit{std::move(m_owner)};
		}

	private:
		async_semaphore::semaphore_awaiter m_inner;
		std::shared_ptr<io_shared>         m_owner;
	};

	/**
	 * @brief Waits for a backpressure permit on @p owner.
	 *
	 * The first of an operation's two suspension points, and the only way to obtain an
	 * io_permit. `auto permit = co_await acquire_permit(owner);` (not const: it is moved from), test it, and move
	 * it into the alt::detail::io_awaiter for the second.
	 */
	[[nodiscard]] inline io_permit_awaiter acquire_permit(std::shared_ptr<io_shared> owner)
	{
		return io_permit_awaiter{std::move(owner)};
	}

}

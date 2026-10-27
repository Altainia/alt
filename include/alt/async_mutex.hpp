#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <expected>
#include <system_error>
#include <utility>

namespace alt
{

	class async_mutex;

	/**
	 * @brief Owns an alt::async_mutex's lock for as long as it lives.
	 *
	 * The only way to hold an alt::async_mutex, and the only way to release one.
	 * There is no public constructor that does not already hold the lock, so a guard
	 * that owns nothing cannot be built: the fails-open state is unrepresentable
	 * rather than merely documented. A wait that ends without the lock yields
	 * std::unexpected, never an empty guard.
	 *
	 * That type-level guarantee is what the family's other primitives do not have,
	 * and it is worth its weight for a measured reason. On GCC 15.2.0 and Clang
	 * 21.1.8, dropping the result of a co_await on this class, or on any of the other
	 * three primitives, is diagnosed by Clang and by clang-tidy and by nothing at all
	 * on GCC. What divides that from a discarded co_await on an alt::task<T>, which
	 * GCC does warn on, was not determined; it is not the result type. See
	 * detail::cancellation_checked::await_resume, where both cases were measured and
	 * where the reasons not to generalize from either are recorded. So on a
	 * GCC build `co_await mutex;` as a statement is diagnosed by nothing -- and it is
	 * still safe here, because the guard inside the discarded std::expected is
	 * destroyed at the end of that full-expression and unlocks. A dropped result is a
	 * critical section of zero length, not a lock held by a caller that believes it
	 * holds nothing.
	 *
	 * Move-only, like std::unique_lock: a moved-from guard owns nothing, reports
	 * owns_lock() false, and does not unlock when it dies, so the lock is released
	 * exactly once however the guard is passed around. Nothing here is thread-safe
	 * against concurrent use of the same guard object, as with any other value; a
	 * guard moved to another thread and released there is fine.
	 *
	 * The guard must not outlive its mutex. Releasing a lock means handing it to the
	 * mutex's next waiter, which a guard whose mutex has already gone cannot do.
	 */
	class async_mutex_lock
	{
	public:
		/** Takes over @p other's lock, leaving @p other owning nothing. */
		async_mutex_lock(async_mutex_lock&& other) noexcept: m_mutex(std::exchange(other.m_mutex, nullptr))
		{}

		/**
		 * Releases this guard's own lock first, if it has one, and then takes over
		 * @p other's. Self-move-assignment leaves the lock where it is.
		 */
		async_mutex_lock& operator=(async_mutex_lock&& other) noexcept;

		/** Releases the lock, if this guard still owns it. */
		~async_mutex_lock();

		async_mutex_lock(const async_mutex_lock&)            = delete;
		async_mutex_lock& operator=(const async_mutex_lock&) = delete;

		/**
		 * @brief Releases the lock now rather than at the end of the scope.
		 *
		 * Idempotent: a guard that owns nothing does nothing here, so calling this and
		 * then letting the guard die releases the lock once. That matters more than it
		 * looks, because releasing a lock hands it to the next waiter: a second release
		 * would let a second coroutine into a critical section the first has not left.
		 *
		 * The next waiter's body runs on this thread before this returns, so this must
		 * not be called with a lock held that a waiter might take. See
		 * alt::async_mutex for the whole of what that hand-off does.
		 */
		void unlock() noexcept;

		/** True while this guard holds the lock. False after a move or an unlock(). */
		[[nodiscard]] bool owns_lock() const noexcept
		{
			return m_mutex != nullptr;
		}

	private:
		// The one constructor that makes a guard, available only to the mutex, which
		// calls it on the two paths that have just acquired the lock. This is the whole
		// of the type-level guarantee above.
		friend class async_mutex;

		/** Adopts the lock on @p mutex, which the caller must already hold. */
		explicit async_mutex_lock(async_mutex& mutex) noexcept: m_mutex(&mutex)
		{}

		/** The mutex whose lock this guard holds, or null when it holds none. */
		async_mutex* m_mutex = nullptr;
	};

	/**
	 * @brief A mutual exclusion lock that coroutines await.
	 *
	 * Awaiting the mutex acquires it, suspending the coroutine if another party holds
	 * it, and yields std::expected<async_mutex_lock, std::error_code>: the guard that
	 * owns the lock, or std::errc::operation_canceled when the wait ended without it,
	 * because the awaiting chain was cancelled or this mutex was destroyed with
	 * waiters still parked. try_lock() is the same acquisition without the
	 * suspension.
	 *
	 * The lock is only ever released through the guard, which releases it on
	 * destruction, so a critical section is a scope:
	 *
	 *     if(const auto held = co_await mutex)
	 *     {
	 *         // the critical section
	 *     }
	 *
	 * Fairness, as far as it goes: a release hands the lock directly to the oldest
	 * parked waiter rather than dropping it and letting everyone re-contend, so a
	 * waiter that has parked cannot be overtaken by a later arrival. What is not
	 * ordered is the race to park: a coroutine that finds the mutex held and a
	 * try_lock() a moment later are two unsynchronized attempts at the same lock, and
	 * the second may win. The one visible cost of handing the lock over directly is
	 * that a coroutine which releases the lock and immediately awaits it again does
	 * not get it straight back: it goes to the back of the queue behind the waiters
	 * that were already there.
	 *
	 * However long the queue, releasing the lock uses a constant amount of stack. The
	 * hand-off drains iteratively, so ten thousand waiters behind one lock are ten
	 * thousand resumptions at one stack depth rather than ten thousand nested frames.
	 *
	 * Every member but the destructor is safe to call from any thread, and
	 * concurrently: of several callers reaching for the lock exactly one takes it.
	 * Destruction is not, as ever, and in particular a mutex must not be destroyed
	 * while a cancellation is in flight against one of its waiters, nor while a guard
	 * that could outlive it still holds the lock. Resumption happens on the thread
	 * that released the lock, inside async_mutex_lock::unlock() or the guard's
	 * destructor, which therefore run waiter bodies; co_await alt::resume_on(scheduler)
	 * in a waiter that needs to be somewhere specific.
	 *
	 * Neither copyable nor movable: the awaiters inside suspended coroutine frames
	 * and the live guards both hold pointers to it, and no copy or move could fix
	 * those up.
	 *
	 * Three things a waiter's body may not do. The first two are shared with
	 * async_event, async_latch and async_semaphore: it may not destroy another waiter
	 * of the same mutex, because the node a parked waiter is on lives in that
	 * waiter's frame and nothing withdraws it from the queue, and a coroutine parked
	 * here may not have its frame destroyed from outside, for the same reason.
	 * Destroying this mutex instead releases them all, which is the supported
	 * teardown -- and a waiter released that way must not await this mutex again,
	 * unlike async_semaphore's, where a re-await during destruction is both supported
	 * and tested. That is a choice rather than a consequence of anything else here:
	 * the ordinary case is benign, and the destructor says what the choice buys, which
	 * is not having to promise that a re-await is never handed the lock inside the
	 * destructor body and does not carry the guard out of it.
	 *
	 * The third is this mutex's own, and it is where it parts company with
	 * async_semaphore, which permits a waiter resumed by release() to destroy the
	 * semaphore: **a waiter resumed by a release of this lock may not destroy this
	 * mutex.** The hand-off drains iteratively rather than recursively, so the loop
	 * that resumed that waiter may read this object again after the waiter's body has
	 * run. The restriction costs nothing real, because a waiter resumed here is
	 * inside the critical section and holds a guard it must release first, and
	 * releasing it is itself a call into this mutex.
	 */
	class async_mutex
	{
	public:
		/** Constructs an unlocked mutex with no waiters. */
		async_mutex() noexcept = default;

		/**
		 * @brief Releases every waiter still parked, without the lock.
		 *
		 * A backstop rather than the expected path, like waiter_list's own: a waiter
		 * nobody resumes stays suspended forever and its frame is never freed, so
		 * releasing them beats leaking them. Each one sees the released error, which is
		 * how it tells this apart from actually having been handed the lock.
		 */
		~async_mutex();

		async_mutex(const async_mutex&)            = delete;
		async_mutex& operator=(const async_mutex&) = delete;
		async_mutex(async_mutex&&)                 = delete;
		async_mutex& operator=(async_mutex&&)      = delete;

		/**
		 * @brief Takes the lock if it is free, without suspending.
		 *
		 * Yields the guard when the lock was taken, and then the caller owns it until
		 * the guard dies. std::errc::device_or_resource_busy when another party holds
		 * it, which takes nothing and leaves the lock exactly where it was. The result
		 * is [[nodiscard]] because dropping it on the floor is the one way to write a
		 * critical section of zero length by accident -- which is safe, the temporary
		 * guard unlocking as the full-expression ends, but is never what the caller
		 * meant.
		 */
		[[nodiscard]] std::expected<async_mutex_lock, std::error_code> try_lock();

		/**
		 * @brief One suspended waiter on an async_mutex.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's frame.
		 * detail::wait_awaiter holds everything but the three questions this answers:
		 * whether the wait is over, where to park, and what the wait yields.
		 */
		class mutex_awaiter: public detail::wait_awaiter<mutex_awaiter>
		{
		public:
			/** The mutex must outlive the suspension. */
			explicit mutex_awaiter(async_mutex& mutex) noexcept: m_mutex(&mutex)
			{}

			/**
			 * @brief Whether the wait is already over, which for a mutex means taking the
			 *        lock that ends it.
			 *
			 * DELICATE. Read this before changing it.
			 *
			 * This takes the lock, so a wait that returns true from here and then reports
			 * itself cancelled would leave the mutex held by nobody and nothing to return
			 * it: every later locker waits forever. Not a lost permit that a caller might
			 * notice and replace, as the same mistake would be in async_semaphore -- a
			 * permanent deadlock. That a satisfied predicate is never reported cancelled is
			 * therefore a property this class depends on absolutely, and it is not
			 * inherited silently: detail::wait_awaiter::await_suspend's rule 4 is where it
			 * is enforced, by recording inside the condition, under the queue's mutex,
			 * which of the two refusals a failed link was. Do not re-derive it here, and do
			 * not read the node's state word after a failed link; that reading is the
			 * defect rule 4 exists to prevent.
			 *
			 * The consumption is sound for the reason detail::wait_awaiter documents: this
			 * is consulted at most twice per wait, once in await_ready and once as the
			 * condition on the link, and the second call happens only if the first returned
			 * false. A wait therefore takes the lock at most once, whichever of the two
			 * calls found it free, and a lock taken by the second call refuses the link and
			 * reports the wait satisfied without parking. Consult it a third time anywhere
			 * and a single wait could acquire twice and release once.
			 *
			 * The second call runs with the queue's mutex held, so what this must not do is
			 * fixed: no lock of its own, no call back into the list, nothing resumed.
			 * try_acquire() is one exchange on one atomic and does none of those. The
			 * tempting fair version, @c !m_held && list().empty(), self-deadlocks on that
			 * same non-recursive mutex.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_mutex->try_acquire();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_mutex->m_waiters;
			}

			/**
			 * @brief The outcome: the guard, or the released error.
			 *
			 * Hides detail::wait_awaiter's own await_resume rather than extending it,
			 * because a mutex's wait yields a value where the other primitives' yield
			 * nothing. The base still decides *whether* the wait succeeded; this only
			 * decides what success is worth.
			 *
			 * The guard is built here and only here on the waiting path, which is what
			 * makes a released wait yield std::unexpected rather than a guard owning
			 * nothing.
			 */
			[[nodiscard]] std::expected<async_mutex_lock, std::error_code> await_resume() const noexcept
			{
				const detail::wait_result result = detail::wait_awaiter<mutex_awaiter>::await_resume();

				if(!result)
				{
					return std::expected<async_mutex_lock, std::error_code>{std::unexpect, result.error()};
				}

				return std::expected<async_mutex_lock, std::error_code>{guard_for(*m_mutex)};
			}

		private:
			async_mutex* m_mutex;
		};

		/** Awaits the lock, yielding the guard or the released error. */
		[[nodiscard]] auto operator co_await() noexcept
		{
			return mutex_awaiter{*this};
		}

	private:
		// The guard is what releases the lock, so it is the only caller of unlock().
		friend class async_mutex_lock;

		/**
		 * Wraps a lock the caller already holds in its guard. Called on the two paths
		 * that have just acquired one, and nowhere else: a guard made anywhere else
		 * would be a second owner of one lock.
		 */
		[[nodiscard]] static async_mutex_lock guard_for(async_mutex& mutex) noexcept
		{
			return async_mutex_lock{mutex};
		}

		/**
		 * @brief Takes the lock if it is free, reporting whether it did.
		 *
		 * Takes no lock of its own, which is what also makes this the awaiter's ready
		 * check, where it is called with the waiting queue's mutex held. See
		 * mutex_awaiter::ready.
		 */
		[[nodiscard]] bool try_acquire() noexcept;

		/** Releases the lock, handing it to the next waiter. See the definition. */
		void unlock() noexcept;

		/**
		 * Whether the lock is held, or is in transit between a release and the waiter
		 * that release woke.
		 *
		 * Atomic because it is both taken from without the queue's mutex, by a caller
		 * in try_lock() and by a waiter deciding whether to wait at all, and again under
		 * that mutex as the condition on the link. That second reader is what keeps a
		 * release landing between a waiter's two checks from leaving the lock free with
		 * the waiter parked beside it.
		 *
		 * It stays true across a hand-off, and that is deliberate: see unlock(), where
		 * leaving it true is what stops an arriving locker from taking a lock that is
		 * already promised to a waiter.
		 */
		std::atomic<bool> m_held{false};

		detail::waiter_list m_waiters;
	};

}

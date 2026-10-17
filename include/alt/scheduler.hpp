#pragma once

#include <alt/detail/work_queue.hpp>
#include <alt/scope.hpp>
#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <utility>

namespace alt
{

	/**
	 * @brief A unit of work a scheduler can run, erased without allocating.
	 *
	 * A function pointer plus an opaque state pointer. The state must outlive the
	 * point at which the item runs, which for a coroutine resume means the frame
	 * itself supplies the storage.
	 *
	 * The callback may not throw, and the type enforces it. There is no error
	 * channel here, so an escaping exception would surface in whichever thread
	 * happened to be draining the queue, unrelated to whoever posted the item, and
	 * would abandon any work that item had queued behind it. A callback that can
	 * fail must handle that itself.
	 */
	struct work_item
	{
		void (*invoke)(void* state) noexcept = nullptr;
		void* state                          = nullptr;
	};

	/**
	 * @brief Satisfied by anything that can run a work_item.
	 *
	 * This single operation is the whole scheduler contract. Resumption
	 * (alt::resume_on) and cancelled-frame destruction are both expressed as
	 * work items, so a scheduler does not need to know about coroutines at all.
	 */
	template<typename S>
	concept scheduler = requires(S& s, work_item w) { s.post(w); };

	/**
	 * @brief A scheduler whose queue a blocked thread can drive itself.
	 *
	 * Required by the driving overload of alt::sync_wait, which pumps rather than
	 * parking so that single-threaded work cannot deadlock.
	 */
	template<typename S>
	concept drivable_scheduler = scheduler<S> && requires(S& s, const std::atomic<bool>* stop) {
		{ s.run_one() } -> std::same_as<bool>;
		{ s.run_one_or_wait(stop) } -> std::same_as<bool>;
		s.notify_waiters();
	};

	/**
	 * @brief A non-owning, type-erased reference to a scheduler.
	 *
	 * Holds a pointer and a post function pointer, so it allocates nothing and is
	 * trivially copyable. The referenced scheduler must outlive every use.
	 * A default-constructed reference is empty and must not be posted to.
	 */
	class scheduler_ref
	{
	public:
		/** Constructs an empty reference. */
		scheduler_ref() noexcept = default;

		/** Constructs a reference to s, which must outlive this object. */
		template<scheduler S>
		explicit scheduler_ref(S& s) noexcept:
		  m_scheduler(std::addressof(s)),
		  m_post(+[](void* target, work_item w) { static_cast<S*>(target)->post(w); })
		{
		}

		/** True when this reference names a scheduler. */
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_scheduler != nullptr;
		}

		/** Posts to the referenced scheduler. Precondition: this reference is not empty. */
		void post(work_item w) const
		{
			assert(m_scheduler != nullptr && "posting to an empty alt::scheduler_ref");
			m_post(m_scheduler, w);
		}

	private:
		void* m_scheduler                = nullptr;
		void (*m_post)(void*, work_item) = nullptr;
	};

	namespace detail
	{

		/** Per-thread trampoline state: the queue of deferred items and the reentrancy flag. */
		struct trampoline_state
		{
			std::deque<work_item> queue;
			bool                  running = false;
		};

		inline trampoline_state& this_thread_trampoline() noexcept
		{
			static thread_local trampoline_state state;
			return state;
		}

	}

	/**
	 * @brief Runs work items immediately, but never nested.
	 *
	 * A post made while this thread is already running a trampoline item is queued
	 * instead of invoked, and the outermost invocation drains the queue before
	 * returning. Stack depth is therefore bounded by a constant however long a
	 * resume chain grows, while a non-reentrant post still runs synchronously.
	 *
	 * The state is per-thread rather than per-instance, so two instances used on
	 * one thread share a drain. That is harmless, because a work item carries
	 * everything it needs.
	 */
	class trampoline_scheduler
	{
	public:
		/** Runs w now, or queues it if this thread is already draining. */
		void post(work_item w) const
		{
			auto& state = detail::this_thread_trampoline();

			if(state.running)
			{
				state.queue.push_back(w);
				return;
			}

			state.running = true;

			// Work items cannot throw (see alt::work_item), so nothing below can leave
			// this flag set. The guard stays because the flag being wrong is unrecoverable
			// for every later post on this thread, and it costs nothing.
			const alt::scope_exit reset{[&state] { state.running = false; }};

			w.invoke(w.state);

			while(!state.queue.empty())
			{
				const work_item next = state.queue.front();
				state.queue.pop_front();
				next.invoke(next.state);
			}
		}
	};

	/**
	 * @brief A scheduler that runs nothing until it is pumped by hand.
	 *
	 * The deterministic scheduler for tests: post work, then decide exactly when it
	 * runs. Thread-safe, so a worker thread may post to a scheduler another thread
	 * pumps. For a scheduler a program would actually run on, see alt::run_loop and
	 * alt::thread_pool, which are built on the same queue.
	 */
	class manual_scheduler
	{
	public:
		/** Queues w. Safe to call from any thread. */
		void post(work_item w)
		{
			m_queue.push(w);
		}

		/** Runs the oldest queued item. Returns false when the queue was empty. */
		bool run_one()
		{
			work_item next{};
			if(!m_queue.try_pop(next))
			{
				return false;
			}
			next.invoke(next.state);
			return true;
		}

		/**
		 * @brief Blocks until an item can be run, then runs it.
		 *
		 * An optional stop flag joins the wait condition, so a caller waiting on
		 * something this scheduler knows nothing about can be released without arming
		 * any state here. That is what the driving alt::sync_wait uses, and it is why
		 * that overload leaves nothing behind in a scheduler the caller goes on using.
		 *
		 * Returns false when the wait ended for another reason: the stop flag became
		 * true, or wake() released it with nothing queued.
		 */
		bool run_one_or_wait(const std::atomic<bool>* stop = nullptr)
		{
			work_item next{};
			if(!m_queue.pop_or_wait(next, stop))
			{
				return false;
			}
			next.invoke(next.state);
			return true;
		}

		/** Runs queued items until the queue is empty. Returns how many ran. */
		std::size_t run_all()
		{
			std::size_t count = 0;
			while(run_one())
			{
				++count;
			}
			return count;
		}

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const
		{
			return m_queue.empty();
		}

		/**
		 * @brief Prompts a blocked run_one_or_wait to re-evaluate its condition.
		 *
		 * Arms nothing, so unlike wake() it cannot be observed by a later call. Use it
		 * when the condition a waiter is waiting on lives outside this scheduler,
		 * which is the case for the driving alt::sync_wait.
		 */
		void notify_waiters()
		{
			m_queue.notify();
		}

		/** Releases a thread blocked in run_one_or_wait without queueing work. */
		void wake()
		{
			m_queue.wake();
		}

	private:
		detail::work_queue<work_item> m_queue;
	};

	namespace detail
	{

		// noexcept by contract, not by accident: see alt::work_item. Resuming one of
		// this library's coroutines cannot throw, because every promise here stores an
		// escaped exception rather than rethrowing it. A foreign coroutine whose promise
		// rethrows would terminate here, which is the honest outcome: the exception's
		// only other destination is an arbitrary pumping thread.
		inline void resume_trampoline(void* address) noexcept
		{
			std::coroutine_handle<>::from_address(address).resume();
		}

		/** Awaiter that suspends the current coroutine and posts its resumption. */
		template<scheduler S>
		class resume_on_awaiter
		{
		public:
			using scheduler_type = S;

			explicit resume_on_awaiter(S& s) noexcept: m_scheduler(std::addressof(s))
			{}

			/**
			 * The scheduler this will resume on.
			 *
			 * The cancellation layer uses this to rebuild the awaiter as one that also
			 * checks the stop state when the scheduler resumes it. It cannot simply wrap
			 * this awaiter for that, because interception has to happen at the
			 * resumption, and this header cannot see a cancellation context: the
			 * cancellation header includes this one, not the other way round.
			 */
			[[nodiscard]] S& scheduler() const noexcept
			{
				return *m_scheduler;
			}

			bool await_ready() const noexcept
			{
				return false;
			}

			void await_suspend(std::coroutine_handle<> h) const
			{
				// The post may run on another thread and resume the coroutine before
				// this call returns, destroying this awaiter along with the frame.
				// It must therefore be the last statement, and nothing may be read
				// from *this afterwards.
				m_scheduler->post(work_item{.invoke = &resume_trampoline, .state = h.address()});
			}

			void await_resume() const noexcept
			{}

		private:
			S* m_scheduler;
		};

	}

	/**
	 * @brief Awaitable that resumes the awaiting coroutine on the given scheduler.
	 *
	 * The idiomatic thread hop: co_await alt::resume_on(pool) to move onto a worker,
	 * co_await alt::resume_on(ui) to come back.
	 *
	 * The scheduler must outlive the suspension.
	 */
	template<scheduler S>
	[[nodiscard]] auto resume_on(S& s) noexcept
	{
		return detail::resume_on_awaiter<S>{s};
	}

}

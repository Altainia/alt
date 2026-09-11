#pragma once

#include <alt/awaitable.hpp>
#include <alt/detail/coroutine.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <coroutine>
#include <exception>
#include <semaphore>
#include <type_traits>
#include <utility>

namespace alt
{

	namespace detail
	{

		/**
		 * The type sync_wait returns for an await result of type R.
		 *
		 * An rvalue reference becomes a value, because the store it refers to dies
		 * when sync_wait returns. An lvalue reference is preserved, so awaiting a
		 * task<T&> still yields a reference to the original object.
		 */
		template<typename R>
		using sync_wait_result_t = std::conditional_t<std::is_rvalue_reference_v<R>, std::remove_reference_t<R>, R>;

		/**
		 * A root coroutine that runs one awaitable, stores the outcome in a store
		 * the caller owns, and then signals a work_item.
		 *
		 * It destroys its own frame at its final suspension point, before
		 * signaling. Signaling first would let the waiting thread wake up and
		 * destroy the frame while this coroutine is still inside await_suspend.
		 */
		class sync_wait_task
		{
		public:
			class promise_type
			{
			public:
				sync_wait_task get_return_object() noexcept
				{
					return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
				}

				std::suspend_always initial_suspend() noexcept
				{
					return {};
				}

				struct final_awaiter
				{
					bool await_ready() const noexcept
					{
						return false;
					}

					void await_suspend(std::coroutine_handle<promise_type> h) const noexcept
					{
						const work_item done = h.promise().m_on_done;
						h.destroy();
						// The frame is gone. Nothing above may be read again.
						done.invoke(done.state);
					}

					void await_resume() const noexcept
					{}
				};

				final_awaiter final_suspend() noexcept
				{
					return {};
				}

				void return_void() noexcept
				{}

				/** Unreachable: the coroutine body catches everything into the store. */
				void unhandled_exception() noexcept
				{
					std::terminate();
				}

				void set_on_done(work_item on_done) noexcept
				{
					m_on_done = on_done;
				}

			private:
				work_item m_on_done{};
			};

			explicit sync_wait_task(std::coroutine_handle<promise_type> handle) noexcept: m_handle(handle)
			{}

			// Movable, not copyable. The frame's get_return_object returns this by
			// value and make_sync_wait_task returns it again, so a real move
			// constructor is safer than relying on elision through a coroutine
			// return object across the whole GCC 13 to Clang 19 range.
			sync_wait_task(sync_wait_task&& other) noexcept: m_handle(std::exchange(other.m_handle, {}))
			{}

			sync_wait_task(const sync_wait_task&)            = delete;
			sync_wait_task& operator=(const sync_wait_task&) = delete;
			sync_wait_task& operator=(sync_wait_task&&)      = delete;

			// No destructor work: the coroutine destroys its own frame at final suspend.
			~sync_wait_task() = default;

			/** Starts the coroutine. It owns and destroys its own frame from here on. */
			void start(work_item on_done) noexcept
			{
				const auto handle = std::exchange(m_handle, {});
				handle.promise().set_on_done(on_done);
				handle.resume();
			}

		private:
			// No initializer here: every constructor above sets m_handle explicitly,
			// including the one that reaches this point via std::exchange, so a
			// default member initializer would never actually be used.
			std::coroutine_handle<promise_type> m_handle;
		};

		template<typename A, typename T>
		// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both `a` and `store` are owned by the caller of sync_wait, which blocks (or pumps) until this coroutine signals completion before returning, so neither reference can dangle.
		sync_wait_task make_sync_wait_task(A&& a, result_store<T>& store)
		{
			try
			{
				if constexpr(std::is_void_v<T>)
				{
					co_await std::forward<A>(a);
					store.set_value();
				}
				else
				{
					store.set_value(co_await std::forward<A>(a));
				}
			}
			catch(...)
			{
				store.set_exception(std::current_exception());
			}
		}

		inline void release_semaphore(void* state) noexcept
		{
			static_cast<std::binary_semaphore*>(state)->release();
		}

		/**
		 * @brief Completion signal for the driving overload: sets a flag and notifies the pump.
		 *
		 * DELICATE. This object lives on the driving sync_wait's stack frame, and the
		 * flag it sets is the pumping thread's only exit condition. The instant the
		 * store lands, that thread may leave its loop, return from sync_wait, and
		 * destroy this object. finish() therefore reads everything it needs into
		 * locals first and touches nothing through `self` afterwards.
		 *
		 * The pump waits on this object's own flag rather than on state inside the
		 * scheduler, so completion arms nothing that could outlive the call. That is
		 * what makes the scheduler safe for the caller to keep using afterwards.
		 */
		template<typename S>
		class drive_signal
		{
		public:
			explicit drive_signal(S& scheduler) noexcept: m_scheduler(std::addressof(scheduler))
			{
			}

			/** The flag a driving pump waits on, so the wait needs no state in the scheduler. */
			[[nodiscard]] const std::atomic<bool>& flag() const noexcept
			{
				return m_finished;
			}

			[[nodiscard]] bool finished() const noexcept
			{
				return m_finished.load(std::memory_order_acquire);
			}

			static void finish(void* state) noexcept
			{
				auto* const self = static_cast<drive_signal*>(state);

				// Read before the store that publishes completion, because *self may be
				// gone the moment it lands.
				S* const scheduler = self->m_scheduler;

				self->m_finished.store(true, std::memory_order_release);

				// A pure notification. The pump's wait condition reads the flag above
				// directly, so nothing is armed in the scheduler and a caller who reuses it
				// cannot observe a leftover signal from this call. The scheduler itself must
				// outlive this, which is the contract the driving overload documents.
				scheduler->notify_waiters();
			}

		private:
			S*                m_scheduler;
			std::atomic<bool> m_finished{false};
		};

	}

	/**
	 * @brief Runs an awaitable to completion, blocking the calling thread.
	 *
	 * Returns its result or rethrows its exception. This is the only blocking
	 * entry point in the library; alt::task deliberately has no get().
	 *
	 * The calling thread parks, so it cannot pump anything while it waits. Work
	 * that needs the calling thread to make progress, which includes anything
	 * driven by an alt::manual_scheduler nothing else pumps, deadlocks here. Use
	 * the overload taking a scheduler for that case.
	 */
	template<awaitable A>
	detail::sync_wait_result_t<await_result_t<A>> sync_wait(A&& a)
	{
		using result_type = await_result_t<A>;
		using stored_type = detail::sync_wait_result_t<result_type>;

		detail::result_store<stored_type> store;
		std::binary_semaphore             done{0};

		auto runner = detail::make_sync_wait_task(std::forward<A>(a), store);
		runner.start(work_item{.invoke = &detail::release_semaphore, .state = &done});
		done.acquire();

		if constexpr(std::is_void_v<stored_type>)
		{
			store.value();
		}
		else
		{
			return std::move(store).value();
		}
	}

	/**
	 * @brief Runs an awaitable to completion, pumping the given scheduler while it waits.
	 *
	 * The calling thread drives the scheduler's queue instead of parking, so
	 * single-threaded work completes rather than deadlocking. This is the
	 * overload to reach for in any test built around an alt::manual_scheduler.
	 */
	template<awaitable A, drivable_scheduler S>
	detail::sync_wait_result_t<await_result_t<A>> sync_wait(A&& a, S& scheduler)
	{
		using result_type = await_result_t<A>;
		using stored_type = detail::sync_wait_result_t<result_type>;

		detail::result_store<stored_type> store;
		detail::drive_signal<S>           signal{scheduler};

		auto runner = detail::make_sync_wait_task(std::forward<A>(a), store);
		runner.start(work_item{.invoke = &detail::drive_signal<S>::finish, .state = &signal});

		while(!signal.finished())
		{
			scheduler.run_one_or_wait(&signal.flag());
		}

		if constexpr(std::is_void_v<stored_type>)
		{
			store.value();
		}
		else
		{
			return std::move(store).value();
		}
	}

}

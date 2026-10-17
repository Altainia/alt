#pragma once

#include <alt/detail/cancellation.hpp>
#include <alt/detail/coroutine.hpp>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

namespace alt
{

	template<typename T = void>
	class task;

	namespace detail
	{

		/** The parts of a task promise that do not depend on how the result is returned. */
		template<typename T>
		class task_promise_base
		{
		public:
			std::suspend_always initial_suspend() noexcept
			{
				return {};
			}

			/** Transfers control straight to the continuation, so a chain unwinds in constant stack. */
			struct final_awaiter
			{
				task_promise_base* self;

				[[nodiscard]] bool await_ready() const noexcept
				{
					return false;
				}

				[[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> /*current*/) const noexcept
				{
					// Transferring back to the continuation is a resumption alt owns, so it
					// observes the stop. Without this a cancelled chain would resume its
					// parent on completion, and the parent's body would run until its own
					// next co_await. See issue #4.
					//
					// DELICATE, in the same way abandon_chain is: the teardown destroys this
					// frame, whose await_suspend is on the stack. Read what is needed first
					// and touch nothing through self afterwards.
					cancellation_context* const context = self->cancellation_context_pointer();

					if(context != nullptr && context->stop_requested())
					{
						abandon_chain(*context);
						return std::noop_coroutine();
					}

					return self->m_continuation ? self->m_continuation : std::noop_coroutine();
				}

				void await_resume() const noexcept
				{}
			};

			final_awaiter final_suspend() noexcept
			{
				return final_awaiter{this};
			}

			void unhandled_exception() noexcept
			{
				m_result.set_exception(std::current_exception());
			}

			/** Injects the cancellation check ahead of every suspension point in this coroutine. */
			template<typename A>
			auto await_transform(A&& a)
			{
				// NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; m_context has an in-class default member initializer and is never uninitialized here.
				return make_cancellation_checked(std::forward<A>(a), m_context);
			}

			void set_continuation(std::coroutine_handle<> continuation) noexcept
			{
				m_continuation = continuation;
			}

			void set_cancellation_context(cancellation_context* context) noexcept
			{
				m_context = context;
			}

			[[nodiscard]] cancellation_context* cancellation_context_pointer() const noexcept
			{
				return m_context;
			}

			void mark_awaited() noexcept
			{
				m_awaited = true;
			}

			[[nodiscard]] bool was_awaited() const noexcept
			{
				return m_awaited;
			}

			[[nodiscard]] result_store<T>& result() noexcept
			{
				return m_result;
			}

		private:
			result_store<T>         m_result;
			std::coroutine_handle<> m_continuation;
			cancellation_context*   m_context = nullptr;
			bool                    m_awaited = false;
		};

		template<typename T>
		class task_promise final: public task_promise_base<T>
		{
		public:
			alt::task<T> get_return_object() noexcept;

			template<typename U = T>
			void return_value(U&& value)
			{
				this->result().set_value(std::forward<U>(value));
			}
		};

		template<>
		class task_promise<void> final: public task_promise_base<void>
		{
		public:
			alt::task<void> get_return_object() noexcept;

			void return_void() noexcept
			{
				result().set_value();
			}
		};

		template<typename T>
		class task_promise<T&> final: public task_promise_base<T&>
		{
		public:
			alt::task<T&> get_return_object() noexcept;

			void return_value(T& value) noexcept
			{
				this->result().set_value(value);
			}
		};

	}

	/**
	 * @brief A lazy, single-consumer coroutine result.
	 *
	 * The coroutine does not start until the task is awaited, and the awaiting
	 * coroutine becomes its continuation. Completion transfers control straight
	 * back to that continuation, so a chain of any depth unwinds in constant
	 * stack space where the compiler turns that transfer into a tail call; see
	 * docs/task.md for the toolchain-dependent caveat.
	 *
	 * The task owns the coroutine frame and destroys it in its destructor. That
	 * ownership is what lets alt::cancellable_task tear a whole chain down by
	 * destroying only its root.
	 *
	 * Move-only, and awaitable exactly once. Awaiting an lvalue yields a reference
	 * to the result; awaiting an rvalue yields an rvalue reference to it, so binding
	 * the await result with auto moves it out. Bind that case with auto rather than
	 * auto&& or decltype(auto), which would keep a reference into a frame the await
	 * temporary destroys at the end of the full-expression; see docs/task.md. T may
	 * be void or a reference type. Arrays are not supported.
	 *
	 * Discarding a task is nearly always a bug, since a lazy task nobody awaits
	 * never runs, hence [[nodiscard]].
	 */
	template<typename T>
	class [[nodiscard]] task
	{
	public:
		using promise_type = detail::task_promise<T>;
		using value_type   = T;

		/** Constructs an empty task that owns no coroutine. */
		task() noexcept = default;

		/** Adopts a coroutine frame. Called by the promise, not by users. */
		explicit task(std::coroutine_handle<promise_type> handle) noexcept: m_handle(handle)
		{}

		task(task&& other) noexcept: m_handle(std::exchange(other.m_handle, {}))
		{}

		task& operator=(task&& other) noexcept
		{
			if(this != std::addressof(other))
			{
				if(m_handle)
				{
					m_handle.destroy();
				}
				m_handle = std::exchange(other.m_handle, {});
			}
			return *this;
		}

		task(const task&)            = delete;
		task& operator=(const task&) = delete;

		~task()
		{
			if(m_handle)
			{
				m_handle.destroy();
			}
		}

		/**
		 * @brief True once the coroutine has run to completion.
		 *
		 * False for an empty, unstarted, or still-running task. A ready task's
		 * result can be taken without suspending.
		 */
		[[nodiscard]] bool is_ready() const noexcept
		{
			return m_handle && m_handle.done();
		}

		/** Awaits the task, yielding a reference to its result. */
		[[nodiscard]] auto operator co_await() const& noexcept
		{
			return task_awaiter<false>{m_handle};
		}

		/**
		 * Awaits the task, yielding an rvalue reference to its result, so binding the
		 * await result with auto moves it out. Binding it to auto&& or decltype(auto)
		 * instead keeps a reference into a frame the await temporary destroys at the
		 * end of the full-expression.
		 */
		[[nodiscard]] auto operator co_await() && noexcept
		{
			return task_awaiter<true>{m_handle};
		}

	private:
		template<bool Move>
		struct task_awaiter
		{
			std::coroutine_handle<promise_type> handle;

			[[nodiscard]] bool await_ready() const noexcept
			{
				assert(handle && "awaiting an empty or moved-from alt::task");
				assert(!handle.promise().was_awaited() && "an alt::task may only be awaited once");
				return handle.done();
			}

			template<typename Promise>
			std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
			{
				handle.promise().set_continuation(awaiting);
				handle.promise().mark_awaited();

				// Relay the cancellation context down the chain when the awaiting
				// coroutine carries one. A plain coroutine carries none, and then
				// nothing is relayed and no check is ever performed below here.
				if constexpr(requires { awaiting.promise().cancellation_context_pointer(); })
				{
					handle.promise().set_cancellation_context(awaiting.promise().cancellation_context_pointer());
				}

				return handle;
			}

			decltype(auto) await_resume() const
			{
				if constexpr(Move)
				{
					return std::move(handle.promise().result()).value();
				}
				else
				{
					return handle.promise().result().value();
				}
			}
		};

		std::coroutine_handle<promise_type> m_handle{};
	};

	namespace detail
	{

		template<typename T>
		alt::task<T> task_promise<T>::get_return_object() noexcept
		{
			return alt::task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
		}

		inline alt::task<void> task_promise<void>::get_return_object() noexcept
		{
			return alt::task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
		}

		template<typename T>
		alt::task<T&> task_promise<T&>::get_return_object() noexcept
		{
			return alt::task<T&>{std::coroutine_handle<task_promise<T&>>::from_promise(*this)};
		}

	}

}

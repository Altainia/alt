#pragma once

#include <alt/awaitable.hpp>
#include <alt/scheduler.hpp>
#include <coroutine>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace alt::detail
{

	inline void destroy_trampoline(void* address) noexcept
	{
		std::coroutine_handle<>::from_address(address).destroy();
	}

	/**
	 * @brief The cancellation state of one await chain.
	 *
	 * Lives in the root coroutine's frame, so nothing here is reference counted.
	 * Every alt::task in the chain holds a pointer to the same object, relayed
	 * downwards as each child is started.
	 *
	 * The std::stop_token is the exception: the standard library reference counts
	 * the stop state, which is what lets an owner request a stop after the frame
	 * holding this object is gone.
	 */
	struct cancellation_context
	{
		/** The root of the chain. Destroying this destroys every frame below it. */
		std::coroutine_handle<> root;

		/** The cancellation signal. An empty token can never be stopped. */
		std::stop_token token;

		/** Where to destroy the chain. Empty means destroy in place. */
		scheduler_ref home;

		/** True once cancellation has been requested. */
		[[nodiscard]] bool stop_requested() const noexcept
		{
			return token.stop_requested();
		}
	};

	/**
	 * @brief Tears down the await chain rooted at ctx.
	 *
	 * DELICATE. Read this before changing it.
	 *
	 * When there is no home scheduler, root.destroy() destroys the root frame,
	 * which runs the destructor of the await temporary in scope there, which owns
	 * the child alt::task, which destroys the child frame, and so on down to the
	 * leaf. The leaf's await_suspend is on the stack at that moment, so this
	 * function's own arguments and every object reachable through ctx are gone the
	 * instant destroy() returns. Both values it needs are therefore copied to the
	 * stack first, and the caller must touch nothing afterwards.
	 *
	 * Marked noexcept deliberately. If posting to the home scheduler threw, the
	 * exception would propagate into a coroutine that cancellation has already
	 * decided must not run again, so terminating is preferable to resuming it.
	 */
	inline void abandon_chain(cancellation_context& ctx) noexcept
	{
		const std::coroutine_handle<> root = ctx.root;
		const scheduler_ref           home = ctx.home;

		if(home)
		{
			// The frames stay suspended until the post runs, which is what keeps
			// UI-affine state from being destroyed on a worker thread.
			home.post(work_item{.invoke = &destroy_trampoline, .state = root.address()});
			return;
		}

		root.destroy();
	}

	/** The type Inner::await_suspend declares for a coroutine whose promise is Promise. */
	template<typename Inner, typename Promise>
	using raw_suspend_result_t = decltype(std::declval<Inner&>().await_suspend(std::declval<std::coroutine_handle<Promise>>()));

	/** True when T is some std::coroutine_handle specialization. */
	template<typename T>
	inline constexpr bool is_coroutine_handle = false;

	template<typename Promise>
	inline constexpr bool is_coroutine_handle<std::coroutine_handle<Promise>> = true;

	/**
	 * The type the cancellation wrapper declares for its own await_suspend.
	 *
	 * A handle-returning inner awaiter is widened to the erased
	 * std::coroutine_handle<>. This matters: [expr.await] lets an awaiter return
	 * std::coroutine_handle<Z> for any Z, but std::noop_coroutine_handle converts
	 * only to the erased handle, so the cancelled path could not return it
	 * through a typed return type. A typed handle from the inner awaiter converts
	 * to the erased one, so widening costs nothing and resumes identically.
	 */
	template<typename Inner, typename Promise>
	using suspend_result_t = std::conditional_t<is_coroutine_handle<raw_suspend_result_t<Inner, Promise>>, std::coroutine_handle<>, raw_suspend_result_t<Inner, Promise>>;

	/**
	 * @brief Wraps an awaiter so that cancellation is observed before it starts.
	 *
	 * The check happens on entry to the suspension point rather than on
	 * resumption. That ordering is what makes destruction safe: the inner awaiter
	 * has not yet registered a callback, started a thread, or queued anything, so
	 * nothing is in flight when the frames are destroyed.
	 */
	template<typename Inner>
	class cancellation_checked
	{
	public:
		cancellation_checked(Inner inner, cancellation_context* context):
		  m_inner(std::move(inner)),
		  m_context(context)
		{
		}

		/**
		 * Forces a suspension when cancellation has been requested, so that
		 * await_suspend gets the chance to destroy. Otherwise this delegates, so an
		 * already-ready awaiter still costs nothing.
		 */
		bool await_ready()
		{
			if(m_context != nullptr && m_context->stop_requested())
			{
				return false;
			}
			return m_inner.await_ready();
		}

		template<typename Promise>
		suspend_result_t<Inner, Promise> await_suspend(std::coroutine_handle<Promise> h)
		{
			using result = suspend_result_t<Inner, Promise>;

			if(m_context != nullptr && m_context->stop_requested())
			{
				abandon_chain(*m_context);

				// *this, m_inner and m_context may all be destroyed now. Nothing below
				// may read them. The return value must also not resume this frame,
				// which is why the bool case returns true and the handle case returns
				// a no-op handle rather than the inner awaiter's answer.
				if constexpr(std::is_void_v<result>)
				{
					return;
				}
				else if constexpr(std::is_same_v<result, bool>)
				{
					return true;
				}
				else
				{
					return std::noop_coroutine();
				}
			}

			return m_inner.await_suspend(h);
		}

		decltype(auto) await_resume()
		{
			return m_inner.await_resume();
		}

	private:
		Inner                 m_inner;
		cancellation_context* m_context;
	};

	/** Owns the wrapped awaitable so a temporary survives until the co_await completes. */
	template<typename A>
	class cancellation_checked_awaitable
	{
	public:
		cancellation_checked_awaitable(A&& a, cancellation_context* context):
		  m_awaitable(std::forward<A>(a)),
		  m_context(context)
		{
		}

		auto operator co_await() &&
		{
			return cancellation_checked<awaiter_t<A>>{alt::get_awaiter(std::forward<A>(m_awaitable)), m_context};
		}

	private:
		A                     m_awaitable;
		cancellation_context* m_context;
	};

	/** Wraps any awaitable in the cancellation check. A null context disables it. */
	template<typename A>
	auto make_cancellation_checked(A&& a, cancellation_context* context)
	{
		return cancellation_checked_awaitable<A>{std::forward<A>(a), context};
	}

}

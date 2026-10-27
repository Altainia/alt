#pragma once

#include <alt/awaitable.hpp>
#include <alt/scheduler.hpp>
#include <cassert>
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
	inline void abandon_chain(const cancellation_context& ctx) noexcept
	{
		// Before the copies, and before anything is destroyed: destroying an empty
		// handle is undefined behavior, so the precondition is worth stating here the
		// way scheduler_ref::post states its own.
		assert(ctx.root && "abandoning an alt cancellation context with no root frame");

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
		cancellation_checked(Inner&& inner, cancellation_context* context):
		  m_inner(std::forward<Inner>(inner)),
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

		/**
		 * Marked [[nodiscard]], and the width of that is deliberate.
		 *
		 * It does not merely carry the inner awaiter's own attribute. Every await inside
		 * an alt::task or alt::cancellable_task is wrapped in this class, so this makes
		 * **every** wrapped value-returning await nodiscard, whatever the awaitable:
		 * discarding the result of co_await on an alt::task<int> now warns too, not only
		 * on an async primitive's satisfied-or-released std::expected. That is the
		 * intent. Awaiting a value and dropping it is a defect, and the primitives added
		 * in phase 5 lean on the diagnostic -- alt::async_event, alt::async_latch and
		 * alt::async_semaphore most of all, since for those three it is the only thing
		 * standing between a released waiter and a body that proceeds as though the wait
		 * had succeeded. alt::async_mutex leans on its guard's type instead: a dropped
		 * result there is a critical section of zero length, not a lock held in error.
		 * Void-returning awaits are unaffected, having no result to discard.
		 *
		 * Which compilers act on it was measured rather than assumed, and the
		 * measurement does not reduce to a rule. On GCC 15.2.0 and Clang 21.1.8, with
		 * -Wall -Wextra:
		 *
		 * - A discarded co_await on alt::async_event, alt::async_latch,
		 *   alt::async_semaphore or alt::async_mutex is diagnosed by Clang and by
		 *   clang-tidy, and by nothing at all on GCC. For those four the attribute is
		 *   unenforced under GCC.
		 * - A discarded co_await on an alt::task<T> is diagnosed by GCC as well as by
		 *   Clang, measured for both T = int and T = std::expected<void,
		 *   std::error_code>.
		 *
		 * What divides those two cases on GCC was not determined. It is demonstrably not
		 * the result type: the primitives' result is std::expected<void,
		 * std::error_code>, which is also the alt::task case GCC does warn on. So do not
		 * generalize from either line above to an awaitable not named in it, and in
		 * particular do not read the first line as a statement about co_await in
		 * general. Three earlier versions of this comment each stated a rule from a
		 * sample and each rule was false.
		 *
		 * And because the widening reaches every wrapped await, a downstream build with
		 * warnings-as-errors may see new diagnostics on code that compiled clean before
		 * this -- on GCC as well as on Clang, the alt::task case being the common one.
		 */
		[[nodiscard]] decltype(auto) await_resume()
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
			// Deliberately not awaiter_t<A>, which strips the reference that
			// get_awaiter returns for an awaiter lvalue. Instantiating the wrapper on
			// the unstripped type keeps a reference a reference, so await_suspend and
			// await_resume run on the caller's object rather than on a copy of it.
			// m_awaitable outlives the awaiter, so the reference cannot dangle, and
			// the rvalue case stops paying a move.
			using inner_type = decltype(alt::get_awaiter(std::forward<A>(m_awaitable)));
			return cancellation_checked<inner_type>{alt::get_awaiter(std::forward<A>(m_awaitable)), m_context};
		}

	private:
		A                     m_awaitable;
		cancellation_context* m_context;
	};

	/** True for alt::detail::resume_on_awaiter, whose resumption the cancellation layer intercepts. */
	template<typename T>
	inline constexpr bool is_resume_on_awaiter = false;

	template<typename S>
	inline constexpr bool is_resume_on_awaiter<resume_on_awaiter<S>> = true;

	/**
	 * @brief A resume_on that observes the stop state when the scheduler resumes it.
	 *
	 * The generic wrapper below can only check on the way into a suspension point.
	 * That leaves the state this component exists to be cancelled in, suspended,
	 * unprotected: a task parked here would resume and run body code until its next
	 * co_await. See issue #4.
	 *
	 * Intercepting a resumption requires knowing how the resumption happens, so this
	 * cannot be done generically. It is done for the resumptions alt owns, of which
	 * this is one, and foreign awaitables remain checked on entry only.
	 *
	 * The posted work item carries this awaiter rather than the frame handle, because
	 * it needs both the handle and the context at resumption time. The awaiter lives
	 * in the coroutine frame, which stays alive while suspended, so that is safe.
	 */
	template<typename S>
	class checked_resume_on
	{
	public:
		checked_resume_on(S& scheduler, cancellation_context* context) noexcept:
		  m_scheduler(std::addressof(scheduler)),
		  m_context(context)
		{}

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h)
		{
			// On entry, as before: nothing is started for a chain already cancelled.
			if(m_context != nullptr && m_context->stop_requested())
			{
				abandon_chain(*m_context);
				return;
			}

			m_handle = h;

			// The post may run on another thread and resume or abandon before this call
			// returns, destroying this awaiter along with the frame. It must be the last
			// statement, and nothing may be read from *this afterwards.
			m_scheduler->post(alt::work_item{.invoke = &resume_or_abandon, .state = this});
		}

		void await_resume() const noexcept
		{}

	private:
		static void resume_or_abandon(void* state) noexcept
		{
			const auto* const self = static_cast<const checked_resume_on*>(state);

			// Copied out before anything is destroyed, for the same reason
			// abandon_chain copies: the teardown frees the frame this awaiter lives in.
			const cancellation_context* const context = self->m_context;
			const std::coroutine_handle<>     handle  = self->m_handle;

			if(context != nullptr && context->stop_requested())
			{
				abandon_chain(*context);
				return;
			}

			handle.resume();
		}

		S*                      m_scheduler;
		std::coroutine_handle<> m_handle;
		cancellation_context*   m_context;
	};

	/** Wraps any awaitable in the cancellation check. A null context disables it. */
	template<typename A>
	auto make_cancellation_checked(A&& a, cancellation_context* context)
	{
		if constexpr(is_resume_on_awaiter<std::remove_cvref_t<A>>)
		{
			// Rebuilt rather than wrapped, so the stop state is also checked when the
			// scheduler resumes the frame rather than only on the way in.
			using scheduler_type = typename std::remove_cvref_t<A>::scheduler_type;
			return checked_resume_on<scheduler_type>{a.scheduler(), context};
		}
		else
		{
			return cancellation_checked_awaitable<A>{std::forward<A>(a), context};
		}
	}

}

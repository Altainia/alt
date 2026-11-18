#pragma once

#include <alt/detail/cancellation.hpp>
#include <cassert>
#include <coroutine>
#include <stop_token>

namespace alt
{

	class cancellation_ref;

	template<typename Promise>
	[[nodiscard]] cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise> h) noexcept;

	/**
	 * @brief A pointer-sized, non-owning view of the cancellation state of an await chain.
	 *
	 * Lets a hand-written awaiter read the chain's stop token. The library's own
	 * wrapper only checks for cancellation on entry to a suspension point, so an
	 * awaiter that is resumed later has no other way to observe a stop requested in
	 * between.
	 *
	 * A view is empty when there is no chain to observe: it was default constructed,
	 * the awaiting coroutine's promise does not relay cancellation, or it is an
	 * alt::task that was not awaited from a chain rooted at an alt::cancellable_task.
	 * An empty view reports no stop requested and returns a token that can never be
	 * stopped. A non-empty view over a token that cannot be stopped is a different
	 * thing: it is not empty, but stop_possible() on its token is false.
	 *
	 * The view does not extend the life of the chain. It is valid for as long as the
	 * coroutine that obtained it stays suspended, and must not be used once that
	 * coroutine has been resumed or destroyed. Obtain one with alt::get_cancellation_ref.
	 */
	class cancellation_ref
	{
	public:
		/** Constructs an empty view. */
		cancellation_ref() noexcept = default;

		/** True when the view observes a chain, false when it is empty. */
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_context != nullptr;
		}

		/**
		 * The chain's stop token. An empty view, and a chain started without a
		 * stoppable token, both yield a token for which stop_possible() is false.
		 */
		[[nodiscard]] std::stop_token get_stop_token() const noexcept
		{
			return m_context != nullptr ? m_context->token : std::stop_token{};
		}

		/** True once a stop has been requested on the chain. Always false for an empty view. */
		[[nodiscard]] bool stop_requested() const noexcept
		{
			return m_context != nullptr && m_context->stop_requested();
		}

		/**
		 * @brief Tears the whole chain down: destroys the root frame, and with it every
		 *        frame below it, or posts that destruction to the chain's home scheduler.
		 *
		 * DELICATE. This destroys the frame the calling awaiter lives in. It must be the
		 * awaiter's last statement, and nothing may be read through *this, the awaiter,
		 * or any other view of the chain afterwards: all of it may already be gone when
		 * this returns. Return from await_suspend immediately, in a way that does not
		 * resume the coroutine (void, or true for a bool-returning one).
		 *
		 * This is not a general destroy-this-chain button. It is for an operation that
		 * stopped because the chain's own token says so. An operation that stopped for a
		 * reason the token knows nothing about must resume with a released result
		 * instead, as alt's own six waits do, so the awaiting body can see why.
		 *
		 * @pre The view is not empty.
		 * @pre stop_requested() is true.
		 */
		void abandon() const noexcept
		{
			assert(m_context != nullptr && "abandoning an empty alt::cancellation_ref");
			assert(m_context->stop_requested() && "abandoning an alt chain with no stop requested");

			detail::abandon_chain(*m_context);
		}

	private:
		template<typename Promise>
		friend cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise> h) noexcept;

		explicit cancellation_ref(detail::cancellation_context* context) noexcept:
		  m_context(context)
		{}

		detail::cancellation_context* m_context = nullptr;
	};

	/**
	 * @brief Builds a cancellation_ref from the handle an await_suspend receives.
	 *
	 * The view is non-empty when Promise relays cancellation and its context pointer
	 * is non-null. Inside an alt::cancellable_task that is always so; inside an
	 * alt::task it is so only when the task was awaited from a chain rooted at an
	 * alt::cancellable_task.
	 *
	 * An erased std::coroutine_handle<> always yields an empty view, because it has
	 * no promise() to ask. An awaiter must therefore declare its await_suspend as a
	 * template over the promise type to receive a non-empty view:
	 *
	 * @code
	 * template<typename Promise>
	 * void await_suspend(std::coroutine_handle<Promise> h);
	 * @endcode
	 *
	 * @pre h is not empty.
	 */
	template<typename Promise>
	[[nodiscard]] cancellation_ref get_cancellation_ref(std::coroutine_handle<Promise> h) noexcept
	{
		assert(h && "getting a cancellation_ref from an empty coroutine handle");

		if constexpr(detail::relays_cancellation<Promise>)
		{
			return cancellation_ref{h.promise().cancellation_context_pointer()};
		}
		else
		{
			return cancellation_ref{};
		}
	}

}

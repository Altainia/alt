#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <expected>
#include <type_traits>
#include <utility>

namespace alt
{

	/**
	 * @brief Satisfied by a type usable directly as a coroutine awaiter.
	 *
	 * Requires the three await member functions. await_suspend is probed with an
	 * erased std::coroutine_handle<>, so an awaiter that only accepts a typed
	 * handle does not satisfy this concept even though co_await would accept it.
	 * That restriction keeps the concept usable; the library's own internal
	 * awaiters take typed handles and are not required to satisfy it.
	 */
	template<typename T>
	concept awaiter = requires(T& a, std::coroutine_handle<> h) {
		{ a.await_ready() } -> std::convertible_to<bool>;
		a.await_suspend(h);
		a.await_resume();
	};

	namespace detail
	{

		template<typename T>
		concept has_member_co_await = requires(T&& t) { std::forward<T>(t).operator co_await(); };

		template<typename T>
		concept has_free_co_await = requires(T&& t) { operator co_await(std::forward<T>(t)); };

	}

	/**
	 * @brief Resolves an awaitable to its awaiter.
	 *
	 * Applies a member operator co_await, then a free one, and otherwise returns
	 * the argument unchanged on the assumption that it is already an awaiter. The
	 * result may be a reference into the argument, so the argument must outlive it.
	 */
	template<typename A>
	constexpr decltype(auto) get_awaiter(A&& a)
	{
		if constexpr(detail::has_member_co_await<A>)
		{
			return std::forward<A>(a).operator co_await();
		}
		else if constexpr(detail::has_free_co_await<A>)
		{
			return operator co_await(std::forward<A>(a));
		}
		else
		{
			return std::forward<A>(a);
		}
	}

	namespace detail
	{

		/** The awaiter type that alt::get_awaiter yields for A, with references stripped. */
		template<typename A>
		using awaiter_t = std::remove_reference_t<decltype(alt::get_awaiter(std::declval<A&&>()))>;

	}

	/**
	 * @brief Satisfied by anything co_await can be applied to outside a promise context.
	 *
	 * A promise type's await_transform can make an expression awaitable in one
	 * coroutine and not in another, and no concept can observe that. This concept
	 * therefore describes context-free awaitables only. alt::task uses
	 * await_transform, so this limitation is real rather than theoretical.
	 */
	template<typename T>
	concept awaitable = awaiter<detail::awaiter_t<T>>;

	/** @brief The type produced by awaiting A. */
	template<awaitable A>
	using await_result_t = decltype(std::declval<detail::awaiter_t<A>&>().await_resume());

	/** @brief Satisfied by an awaitable whose result is exactly R. */
	template<typename T, typename R>
	concept awaitable_of = awaitable<T> && std::same_as<await_result_t<T>, R>;

	namespace detail
	{

		/** The std::expected type that as_expected yields for a result of type R. */
		template<typename R>
		using expected_result_t = std::expected<std::conditional_t<std::is_void_v<R>, void, std::decay_t<R>>, std::exception_ptr>;

		/** Awaiter that converts a thrown exception from await_resume into an unexpected value. */
		template<typename Inner>
		class expected_awaiter
		{
		public:
			using result_type   = decltype(std::declval<Inner&>().await_resume());
			using expected_type = expected_result_t<result_type>;

			explicit expected_awaiter(Inner&& inner): m_inner(std::forward<Inner>(inner))
			{}

			bool await_ready()
			{
				return m_inner.await_ready();
			}

			template<typename Promise>
			decltype(auto) await_suspend(std::coroutine_handle<Promise> h)
			{
				return m_inner.await_suspend(h);
			}

			expected_type await_resume()
			{
				try
				{
					if constexpr(std::is_void_v<result_type>)
					{
						m_inner.await_resume();
						return expected_type{};
					}
					else
					{
						return expected_type{m_inner.await_resume()};
					}
				}
				catch(...)
				{
					return expected_type{std::unexpect, std::current_exception()};
				}
			}

		private:
			Inner m_inner;
		};

		/** Owns the wrapped awaitable so a temporary survives until the co_await completes. */
		template<typename A>
		class expected_awaitable
		{
		public:
			explicit expected_awaitable(A&& a): m_awaitable(std::forward<A>(a))
			{}

			auto operator co_await() &&
			{
				// Deliberately not awaiter_t<A>: see the identical note in
				// cancellation_checked_awaitable. Stripping the reference here would run
				// the awaiter on a copy and leave the caller's object untouched.
				using inner_type = decltype(alt::get_awaiter(std::forward<A>(m_awaitable)));
				return expected_awaiter<inner_type>{alt::get_awaiter(std::forward<A>(m_awaitable))};
			}

		private:
			A m_awaitable;
		};

	}

	/**
	 * @brief Adapts an awaitable so that awaiting it yields std::expected instead of throwing.
	 *
	 * The result is std::expected<std::decay_t<R>, std::exception_ptr>, or
	 * std::expected<void, std::exception_ptr> when R is void. A reference result is
	 * decayed to a value.
	 *
	 * Only exceptions thrown by await_resume are captured, which is where
	 * alt::task reports the result of its coroutine body. An exception thrown by
	 * await_suspend, meaning the awaited operation failed to start, still
	 * propagates at the await expression.
	 */
	template<awaitable A>
	[[nodiscard]] auto as_expected(A&& a)
	{
		return detail::expected_awaitable<A>{std::forward<A>(a)};
	}

}

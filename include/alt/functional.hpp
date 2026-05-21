#pragma once

#include <alt/concepts.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>

namespace alt
{

	// ---------------------------------------------------------------------------
	// detail
	// ---------------------------------------------------------------------------

	namespace detail
	{

		/**
		 * True when F can be applied to Arg (directly or via Arg()) yielding a
		 * bool-convertible result.
		 */
		template<typename F, typename Arg>
		concept f_applicable =
		  (std::invocable<F, Arg> && std::convertible_to<std::invoke_result_t<F, Arg>, bool>) || (std::invocable<Arg> && std::invocable<F, std::invoke_result_t<Arg>> && std::convertible_to<std::invoke_result_t<F, std::invoke_result_t<Arg>>, bool>);

		/**
		 * Evaluate a single bool_condition to bool. Invocables are called; other
		 * values are converted directly. When both apply (e.g. a function pointer),
		 * invocation takes priority.
		 */
		template<bool_condition T>
		constexpr bool eval_condition(T&& t)
		{
			if constexpr(std::invocable<T>)
			{
				return static_cast<bool>(std::invoke(std::forward<T>(t)));
			}
			else
			{
				return static_cast<bool>(std::forward<T>(t));
			}
		}

		/**
		 * Apply F to arg. Uses F(arg) if that yields a bool-convertible result;
		 * otherwise invokes arg with no parameters first and passes the result to F.
		 * F is taken by lvalue reference because it may be called once per argument
		 * in a fold expression.
		 */
		template<typename F, typename Arg>
		  requires f_applicable<F, Arg>
		constexpr bool eval_with(F& f, Arg&& arg)
		{
			if constexpr(requires { { std::invoke(f, std::declval<Arg>()) } -> std::convertible_to<bool>; })
			{
				return static_cast<bool>(std::invoke(f, std::forward<Arg>(arg)));
			}
			else
			{
				return static_cast<bool>(std::invoke(f, std::invoke(std::forward<Arg>(arg))));
			}
		}

	} // namespace detail

	// ---------------------------------------------------------------------------
	// any_of
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if at least one condition evaluates to true.
	 *
	 * Each argument must satisfy @ref bool_condition. Nullary invocables are
	 * called; other values are converted directly to bool. Evaluation
	 * short-circuits on the first true condition.
	 */
	template<bool_condition... Args>
	[[nodiscard]] constexpr bool any_of(Args&&... args)
	{
		return (detail::eval_condition(std::forward<Args>(args)) || ...);
	}

	/**
	 * @brief Returns true if @p f returns true for at least one argument.
	 *
	 * For each argument, @p f is invoked with the argument directly if possible,
	 * or with the result of invoking the argument with no parameters otherwise.
	 * Evaluation short-circuits on the first true result.
	 *
	 * @note @p f must not satisfy @ref bool_condition. A nullary callable that
	 * satisfies bool_condition is always treated as a condition by the other
	 * overload, not as a transformer here. This eliminates ambiguity for nullary
	 * callables.
	 */
	template<typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool any_of(F&& f, Args&&... args)
	{
		return (detail::eval_with(f, std::forward<Args>(args)) || ...);
	}

	// ---------------------------------------------------------------------------
	// all_of
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if every condition evaluates to true.
	 *
	 * Vacuously true for an empty argument list. Evaluation short-circuits on
	 * the first false condition.
	 */
	template<bool_condition... Args>
	[[nodiscard]] constexpr bool all_of(Args&&... args)
	{
		return (detail::eval_condition(std::forward<Args>(args)) && ...);
	}

	/**
	 * @brief Returns true if @p f returns true for every argument.
	 *
	 * Vacuously true when no arguments follow @p f. Evaluation short-circuits on
	 * the first false result.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool all_of(F&& f, Args&&... args)
	{
		return (detail::eval_with(f, std::forward<Args>(args)) && ...);
	}

	// ---------------------------------------------------------------------------
	// not_all_of
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if at least one condition evaluates to false.
	 *
	 * Equivalent to !all_of(args...). Evaluation short-circuits on the first
	 * false condition.
	 */
	template<bool_condition... Args>
	[[nodiscard]] constexpr bool not_all_of(Args&&... args)
	{
		return !(detail::eval_condition(std::forward<Args>(args)) && ...);
	}

	/**
	 * @brief Returns true if @p f returns false for at least one argument.
	 *
	 * Evaluation short-circuits on the first false result.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool not_all_of(F&& f, Args&&... args)
	{
		return !(detail::eval_with(f, std::forward<Args>(args)) && ...);
	}

	// ---------------------------------------------------------------------------
	// none_of
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if no condition evaluates to true.
	 *
	 * Vacuously true for an empty argument list. Evaluation short-circuits on
	 * the first true condition.
	 */
	template<bool_condition... Args>
	[[nodiscard]] constexpr bool none_of(Args&&... args)
	{
		return (!detail::eval_condition(std::forward<Args>(args)) && ...);
	}

	/**
	 * @brief Returns true if @p f returns false for every argument.
	 *
	 * Vacuously true when no arguments follow @p f. Evaluation short-circuits on
	 * the first true result.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool none_of(F&& f, Args&&... args)
	{
		return (!detail::eval_with(f, std::forward<Args>(args)) && ...);
	}

	// ---------------------------------------------------------------------------
	// at_least
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if at least @p N conditions evaluate to true.
	 *
	 * @tparam N Minimum number of conditions that must be true. Always true
	 *           when N is zero regardless of the arguments.
	 *
	 * Evaluation short-circuits once N true conditions have been seen.
	 */
	template<std::size_t N, bool_condition... Args>
	[[nodiscard]] constexpr bool at_least(Args&&... args)
	{
		if constexpr(N == 0)
		{
			return true;
		}
		else if constexpr(sizeof...(Args) == 0)
		{
			return false;
		}
		else
		{
			std::size_t count{0};
			return ((count += static_cast<std::size_t>(detail::eval_condition(std::forward<Args>(args))),
			         count >= N) ||
			        ...);
		}
	}

	/**
	 * @brief Returns true if @p f returns true for at least @p N arguments.
	 *
	 * @tparam N Minimum number of arguments for which @p f must return true.
	 *           Always true when N is zero regardless of the arguments.
	 *
	 * Evaluation short-circuits once N true results have been seen.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<std::size_t N, typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool at_least(F&& f, Args&&... args)
	{
		if constexpr(N == 0)
		{
			return true;
		}
		else if constexpr(sizeof...(Args) == 0)
		{
			return false;
		}
		else
		{
			std::size_t count{0};
			return ((count += static_cast<std::size_t>(detail::eval_with(f, std::forward<Args>(args))),
			         count >= N) ||
			        ...);
		}
	}

	// ---------------------------------------------------------------------------
	// at_most
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if at most @p N conditions evaluate to true.
	 *
	 * @tparam N Maximum number of conditions that may be true.
	 *
	 * Vacuously true for an empty argument list. Evaluation short-circuits once
	 * more than N true conditions have been seen.
	 */
	template<std::size_t N, bool_condition... Args>
	[[nodiscard]] constexpr bool at_most(Args&&... args)
	{
		std::size_t count{0};
		return !((count += static_cast<std::size_t>(detail::eval_condition(std::forward<Args>(args))),
		          count > N) ||
		         ...);
	}

	/**
	 * @brief Returns true if @p f returns true for at most @p N arguments.
	 *
	 * @tparam N Maximum number of arguments for which @p f may return true.
	 *
	 * Vacuously true when no arguments follow @p f. Evaluation short-circuits
	 * once more than N true results have been seen.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<std::size_t N, typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool at_most(F&& f, Args&&... args)
	{
		std::size_t count{0};
		return !((count += static_cast<std::size_t>(detail::eval_with(f, std::forward<Args>(args))),
		          count > N) ||
		         ...);
	}

	// ---------------------------------------------------------------------------
	// exactly
	// ---------------------------------------------------------------------------

	/**
	 * @brief Returns true if exactly @p N conditions evaluate to true.
	 *
	 * @tparam N Exact number of conditions that must be true.
	 *
	 * Evaluation short-circuits once more than N true conditions have been seen.
	 */
	template<std::size_t N, bool_condition... Args>
	[[nodiscard]] constexpr bool exactly(Args&&... args)
	{
		if constexpr(sizeof...(Args) == 0)
		{
			return N == 0;
		}
		else
		{
			std::size_t count{0};
			bool const  over = ((count += static_cast<std::size_t>(detail::eval_condition(std::forward<Args>(args))),
                          count > N) ||
                         ...);
			return !over && count == N;
		}
	}

	/**
	 * @brief Returns true if @p f returns true for exactly @p N arguments.
	 *
	 * @tparam N Exact number of arguments for which @p f must return true.
	 *
	 * Evaluation short-circuits once more than N true results have been seen.
	 *
	 * @note @p f must not satisfy @ref bool_condition. See @ref any_of for
	 * details on the disambiguation rule.
	 */
	template<std::size_t N, typename F, typename... Args>
	  requires(!bool_condition<F>) && (detail::f_applicable<F, Args> && ...)
	[[nodiscard]] constexpr bool exactly(F&& f, Args&&... args)
	{
		if constexpr(sizeof...(Args) == 0)
		{
			return N == 0;
		}
		else
		{
			std::size_t count{0};
			bool const  over = ((count += static_cast<std::size_t>(detail::eval_with(f, std::forward<Args>(args))),
                          count > N) ||
                         ...);
			return !over && count == N;
		}
	}

} // namespace alt

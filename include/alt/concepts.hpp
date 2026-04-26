#pragma once

#include <concepts>
#include <type_traits>

namespace alt
{

	/** Satisfied by any enumeration type (scoped or unscoped). */
	template<typename T>
	concept any_enum = std::is_enum_v<T>;

	/** Satisfied by scoped enumeration types only (enum class or enum struct). */
	template<typename T>
	concept scoped_enum = std::is_scoped_enum_v<T>;

	/**
	 * @brief Satisfied when T is directly usable as a boolean condition.
	 *
	 * T must be either (1) implicitly convertible to bool and not a callable
	 * type, or (2) a nullary invocable whose result is implicitly convertible
	 * to bool. Callable objects and function pointers with parameters are
	 * excluded from (1) so they remain available as transformers for the
	 * callable overloads of the functional combinators.
	 */
	template<typename T>
	concept bool_condition =
	  (std::convertible_to<T, bool> && !requires { &std::remove_cvref_t<T>::operator(); } && !(std::is_pointer_v<std::remove_cvref_t<T>> && std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<T>>>)) || (std::invocable<T> && std::convertible_to<std::invoke_result_t<T>, bool>);

} // namespace alt

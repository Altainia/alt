#pragma once

#include <concepts>
#include <cstddef>
#include <ranges>
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
	 * @brief Satisfied by enumeration types usable as a bit set.
	 *
	 * Any enumeration qualifies, scoped or unscoped, provided its underlying type has
	 * an unsigned counterpart to hold the bits in. That excludes an underlying type of
	 * bool, for which std::make_unsigned has no answer.
	 *
	 * This is the constraint used by alt::flags. Scoping is deliberately not required:
	 * it is a property of the enumeration itself and nothing alt::flags depends on.
	 */
	template<typename T>
	concept flag_enum = any_enum<T> && !std::same_as<std::underlying_type_t<T>, bool>;

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

	namespace detail
	{

		/** Satisfied by the element types that meaningfully represent a raw byte. */
		template<typename T>
		concept byte_like = std::same_as<T, char> || std::same_as<T, signed char> ||
		                    std::same_as<T, unsigned char> || std::same_as<T, char8_t> ||
		                    std::same_as<T, std::byte>;

		/** True for raw arrays whose element type carries a NUL-termination convention. */
		template<typename T>
		inline constexpr bool is_character_array = false;

		template<std::size_t N>
		inline constexpr bool is_character_array<char[N]> = true;

		template<std::size_t N>
		inline constexpr bool is_character_array<char8_t[N]> = true;

	} // namespace detail

	/**
	 * @brief Satisfied by an input range of raw bytes.
	 *
	 * The element type must be one of @c char, @c signed @c char, @c unsigned @c char,
	 * @c char8_t, or @c std::byte. Contiguity is not required, so a lazy view of bytes
	 * qualifies just as a @c std::vector or @c std::span does.
	 *
	 * Raw arrays of @c char and @c char8_t are deliberately excluded. Those are the
	 * element types that carry a NUL-termination convention, and a string literal is
	 * such an array: consuming one whole would process the terminator as data. Arrays
	 * of the other byte types carry no such convention and are accepted. A caller with
	 * a string literal should pass it as a @c std::string_view, which drops the
	 * terminator; a caller with binary data in a @c char buffer should pass a
	 * @c std::span.
	 */
	template<typename T>
	concept byte_range =
	  std::ranges::input_range<T> && detail::byte_like<std::remove_cv_t<std::ranges::range_value_t<T>>> &&
	  !detail::is_character_array<std::remove_cv_t<std::remove_reference_t<T>>>;

} // namespace alt

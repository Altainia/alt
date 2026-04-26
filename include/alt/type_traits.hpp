#pragma once

#include <type_traits>

namespace alt
{

	/**
	 * @brief Trait that yields the underlying integer type of an enum or integral type.
	 *
	 * For enum types, `type` is `std::underlying_type_t<T>`.
	 * For integral types, `type` is `T` unchanged.
	 * Instantiating with any other type is a compile error.
	 */
	template<typename T>
	struct underlying_int;

	template<typename T>
	  requires std::is_enum_v<T>
	struct underlying_int<T>
	{
		using type = std::underlying_type_t<T>;
	};

	template<typename T>
	  requires std::is_integral_v<T>
	struct underlying_int<T>
	{
		using type = T;
	};

	/** @brief Alias for `underlying_int<T>::type`. */
	template<typename T>
	using underlying_int_t = typename underlying_int<T>::type;

} // namespace alt

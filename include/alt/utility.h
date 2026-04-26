#pragma once

#include <alt/type_traits.h>

#include <type_traits>

namespace alt
{

	/**
	 * @brief Returns the underlying integer value of an enum or integral type.
	 *
	 * For enum types, casts to `underlying_int_t<T>`.
	 * For integral types, returns the value unchanged.
	 */
	template<typename T>
	  requires(std::is_enum_v<T> || std::is_integral_v<T>)
	constexpr auto to_underlying(T value) noexcept -> underlying_int_t<T>
	{
		if constexpr(std::is_enum_v<T>)
			return static_cast<underlying_int_t<T>>(value);
		else
			return value;
	}

} // namespace alt

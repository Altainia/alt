#pragma once

#include <type_traits>

namespace alt
{

	/** Satisfied by any enumeration type (scoped or unscoped). */
	template<typename T>
	concept any_enum = std::is_enum_v<T>;

	/** Satisfied by scoped enumeration types only (enum class or enum struct). */
	template<typename T>
	concept scoped_enum = std::is_scoped_enum_v<T>;

} // namespace alt

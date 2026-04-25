#pragma once

#include <string_view>

#define ALT_VERSION_MAJOR 0
#define ALT_VERSION_MINOR 1
#define ALT_VERSION_PATCH 0

namespace alt
{
	inline constexpr int version_major = ALT_VERSION_MAJOR;
	inline constexpr int version_minor = ALT_VERSION_MINOR;
	inline constexpr int version_patch = ALT_VERSION_PATCH;

	std::string_view version() noexcept;
} // namespace alt
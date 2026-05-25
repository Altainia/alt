#pragma once

#include <string_view>

// NOLINT(cppcoreguidelines-macro-usage,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum): version macros must be macros for preprocessor-time version checks
#define ALT_VERSION_MAJOR 1 // NOLINT(cppcoreguidelines-macro-usage,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define ALT_VERSION_MINOR 2 // NOLINT(cppcoreguidelines-macro-usage,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define ALT_VERSION_PATCH 0 // NOLINT(cppcoreguidelines-macro-usage,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)

namespace alt
{
	inline constexpr int version_major = ALT_VERSION_MAJOR;
	inline constexpr int version_minor = ALT_VERSION_MINOR;
	inline constexpr int version_patch = ALT_VERSION_PATCH;

	std::string_view version() noexcept;
} // namespace alt
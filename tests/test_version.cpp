#include <gtest/gtest.h>

#include <alt/version.hpp>

TEST(Version, Macros)
{
	EXPECT_EQ(ALT_VERSION_MAJOR, 1);
	EXPECT_EQ(ALT_VERSION_MINOR, 6);
	EXPECT_EQ(ALT_VERSION_PATCH, 0);
}

TEST(Version, Constexpr)
{
	static_assert(alt::version_major == 1);
	static_assert(alt::version_minor == 6);
	static_assert(alt::version_patch == 0);
}

TEST(Version, String)
{
	EXPECT_EQ(alt::version(), "1.6.0");
}

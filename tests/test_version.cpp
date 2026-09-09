#include <gtest/gtest.h>

#include <alt/version.hpp>

TEST(Version, Macros)
{
	EXPECT_EQ(ALT_VERSION_MAJOR, 1);
	EXPECT_EQ(ALT_VERSION_MINOR, 4);
	EXPECT_EQ(ALT_VERSION_PATCH, 1);
}

TEST(Version, Constexpr)
{
	static_assert(alt::version_major == 1);
	static_assert(alt::version_minor == 4);
	static_assert(alt::version_patch == 1);
}

TEST(Version, String)
{
	EXPECT_EQ(alt::version(), "1.4.1");
}

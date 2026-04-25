#include <gtest/gtest.h>
#include <alt/version.hpp>

TEST(Version, Macros) {
    EXPECT_EQ(ALT_VERSION_MAJOR, 0);
    EXPECT_EQ(ALT_VERSION_MINOR, 1);
    EXPECT_EQ(ALT_VERSION_PATCH, 0);
}

TEST(Version, Constexpr) {
    static_assert(alt::version_major == 0);
    static_assert(alt::version_minor == 1);
    static_assert(alt::version_patch == 0);
}

TEST(Version, String) {
    EXPECT_EQ(alt::version(), "0.1.0");
}

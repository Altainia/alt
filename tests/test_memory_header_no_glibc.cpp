// Regression guard for the MSVC failure of PR #65: <alt/memory.hpp> once tested
// "defined(__GLIBC__) && __GLIBC_PREREQ(2, 25)" in a single #if. The preprocessor parses
// the whole expression before evaluating it, so where glibc's function-like macro
// __GLIBC_PREREQ does not exist (MSVC, musl) the header failed to compile with C7780 and
// C1017, even though the && would never have reached it.
//
// What this simulates: a toolchain that has neither __GLIBC__ nor __GLIBC_PREREQ. The
// #undefs below fake that on a glibc host. <features.h> has an include guard, so
// including it again does not restore them. It is a stand-in for a non-glibc toolchain,
// not a test of one: it exercises the header's preprocessor logic, and clear_memory then
// takes the volatile-loop arm that such a toolchain would use.
//
// Everything else is included first so that nothing after the #undefs depends on the
// macros. Reserved implementation macros are undefined here deliberately; this file is
// the only place that does.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

#undef __GLIBC__
#undef __GLIBC_PREREQ

#include <alt/memory.hpp>

#ifdef ALT_DETAIL_HAS_EXPLICIT_BZERO
#	error "<alt/memory.hpp> leaked ALT_DETAIL_HAS_EXPLICIT_BZERO into the including translation unit"
#endif

TEST(MemoryHeaderNoGlibc, ClearMemoryZeroesWithoutGlibcMacros)
{
	std::array<unsigned char, 16> buf;
	buf.fill(0xAB);
	alt::clear_memory(buf.data(), buf.size());
	for(unsigned char const b: buf)
		EXPECT_EQ(b, 0u);
}

// Regression guard for issue #58: <alt/memory.hpp> includes <windows.h> on Windows for
// SecureZeroMemory, and <windows.h> defines the function-like macros min and max. The
// header must clean up after itself, so that no Win32 min or max macro reaches the
// including translation unit.
//
// What this guard can and cannot show:
//  - On MSVC it is the test that verifies the cleanup works. Without the cleanup the
//    std::max and std::numeric_limits<>::max() uses below fail to compile with C4003 (not
//    enough arguments for function-like macro invocation 'max'). That failure is inferred
//    from PR #57's MSVC build log, not observed by running this test against the old header.
//  - On Linux and macOS it is vacuous. <windows.h> is never included there, so it
//    cannot fail whatever this header does.
//
// Ordering is deliberate. <gtest/gtest.h> must come after <alt/memory.hpp> and after the
// macro-sensitive code (the #error check and the helpers below), because gtest includes
// <windows.h> on Windows, which defines min and max itself and would mask a leak from
// the header under test. Do not move the gtest include up: the suite would stay green
// everywhere, since this guard is vacuous off MSVC, while it silently stopped guarding.
// .clang-format sorts includes within each contiguous block only (SortIncludes with
// IncludeBlocks: Regroup), so the gtest include is held apart from the first block by the
// code between them. Keep non-include code between the blocks; adjacent includes would be
// merged and re-sorted. <alt/memory.hpp> need not be first in its block, only before gtest.
#include <algorithm>
#include <alt/memory.hpp>
#include <limits>

#if defined(min) || defined(max)
#	error "<alt/memory.hpp> leaked the min or max macro into the including translation unit"
#endif

namespace
{

	int larger_of(int a, int b)
	{
		return std::max(a, b);
	}

	int largest_int()
	{
		return std::numeric_limits<int>::max();
	}

} // namespace

#include <gtest/gtest.h>

TEST(MemoryHeaderHygiene, MinAndMaxStillUsableAfterIncludingMemoryHeader)
{
	EXPECT_EQ(larger_of(3, 7), 7);
	EXPECT_EQ(largest_int(), std::numeric_limits<int>::max());
}

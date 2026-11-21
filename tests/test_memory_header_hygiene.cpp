// Regression guard for issue #58: <alt/memory.hpp> once included <windows.h>, which
// defines the function-like macros min and max in every translation unit that
// included it.
//
// What this guard can and cannot show:
//  - On MSVC it is a real guard. With the old include, the std::max and
//    std::numeric_limits<>::max() uses below fail to compile with C4003 (not enough
//    arguments for function-like macro invocation 'max'). That pre-fix failure is
//    inferred from PR #57's MSVC build log, not observed by running this test.
//  - On Linux and macOS it is vacuous. <windows.h> is never included there, so it
//    cannot fail whatever this header does.
//
// <alt/memory.hpp> must stay the first include, and the macro-sensitive code must
// precede <gtest/gtest.h>, which itself includes <windows.h> on Windows and would
// otherwise mask the header under test.
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

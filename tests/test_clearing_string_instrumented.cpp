// Regression guard for issue #66: a clearing string's destructor zeroed its whole capacity
// even when the buffer was on the heap, and MSVC's STL poisons a heap string's unused
// capacity for AddressSanitizer. The write was invisible only because clear_memory's
// Windows arm is SecureZeroMemory, which ASan does not instrument; the portable volatile
// loop, compiled into the caller, is instrumented byte by byte and reported
// container-overflow from four tests in tests/test_clearing_string.cpp.
//
// What this compiles: <alt/memory.hpp> with _WIN32 hidden, so clear_memory takes the
// volatile-loop arm, which is the one AddressSanitizer can see. Two things keep that
// from being an ODR hazard, or a test that silently runs the other arm:
//
// - clear_memory is renamed for this translation unit, so the loop-arm definition is a
//   different inline function from the one every other translation unit links against;
// - every string here uses an allocator type local to this file, so each member of the
//   std::basic_string specialization instantiated here is unique to this file too.
//
// Only an MSVC debug-asan build discriminates: libstdc++ does not annotate strings, and
// elsewhere the tests still check the clearing behavior. _WIN32 is a reserved macro and
// is hidden here deliberately; this file is the only place that does.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#	pragma push_macro("_WIN32")
#	undef _WIN32
#	define ALT_TEST_HID_WIN32 1 // NOLINT(cppcoreguidelines-macro-usage)
#endif
#define clear_memory clear_memory_instrumented_arm // NOLINT(cppcoreguidelines-macro-usage)
#include <alt/memory.hpp>
#undef clear_memory
#ifdef ALT_TEST_HID_WIN32
#	pragma pop_macro("_WIN32")
#	undef ALT_TEST_HID_WIN32
#endif

namespace
{
	/// Records the bytes of the last block released, after clearing_allocator zeroed it.
	template<typename T>
	struct released_block_spy
	{
		using value_type                 = T;
		std::vector<std::byte>* released = nullptr;

		explicit released_block_spy(std::vector<std::byte>& sink): released(&sink)
		{}

		template<typename U>
		released_block_spy(released_block_spy<U> const& other): released(other.released)
		{}

		T* allocate(std::size_t n)
		{
			return std::allocator<T>{}.allocate(n);
		}

		void deallocate(T* p, std::size_t n)
		{
			auto const* raw = reinterpret_cast<std::byte const*>(p);
			released->assign(raw, raw + (n * sizeof(T)));
			std::allocator<T>{}.deallocate(p, n);
		}
	};

	template<typename T, typename U>
	bool operator==(released_block_spy<T> const& a, released_block_spy<U> const& b) noexcept
	{
		return a.released == b.released;
	}

	using spy_string = std::basic_string<char, std::char_traits<char>, alt::clearing_allocator<char, released_block_spy<char>>>;

	bool all_zero(std::vector<std::byte> const& bytes)
	{
		return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
	}

	constexpr std::string_view long_secret = "a secret far too long for any small-string buffer to hold";
} // namespace

TEST(ClearingStringInstrumented, AHeapStringIsDestroyedWithoutTouchingPoisonedCapacity)
{
	std::vector<std::byte> released;
	{
		spy_string s(long_secret, alt::clearing_allocator<char, released_block_spy<char>>(released_block_spy<char>(released)));
		s.reserve(s.size() * 4); // leave capacity the STL poisons beyond size() + 1
		ASSERT_GT(s.capacity(), s.size());
	}
	ASSERT_FALSE(released.empty()) << "test pre-condition: the string must have been on the heap";
	EXPECT_TRUE(all_zero(released)) << "the released block still held bytes";
}

TEST(ClearingStringInstrumented, StaleHeapBytesPastAShortenedSizeAreClearedWhenTheBlockIsReleased)
{
	// The destructor zeroes only size() + 1 of a heap buffer; the stale tail of a string
	// shortened in place is left to clearing_allocator::deallocate, which must clear it.
	std::vector<std::byte> released;
	{
		spy_string s(long_secret, alt::clearing_allocator<char, released_block_spy<char>>(released_block_spy<char>(released)));
		s.resize(4); // shortens in place: the old characters stay past size()
		ASSERT_GE(s.capacity(), long_secret.size());
	}
	ASSERT_FALSE(released.empty());
	EXPECT_TRUE(all_zero(released)) << "stale characters past the shortened size survived the release";
}

TEST(ClearingStringInstrumented, MoveAndGrowthReleaseNothingUncleared)
{
	std::vector<std::byte> released;
	auto const             alloc = alt::clearing_allocator<char, released_block_spy<char>>(released_block_spy<char>(released));
	spy_string             source(long_secret, alloc);
	spy_string             target(std::move(source));
	EXPECT_EQ(target, spy_string(long_secret, alloc));

	released.clear();
	target.reserve(target.capacity() * 2); // moves to a larger block and releases the old one
	ASSERT_FALSE(released.empty());
	EXPECT_TRUE(all_zero(released)) << "the block released by growth still held bytes";
}

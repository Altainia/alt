#include <gtest/gtest.h>

#include <alt/concepts.hpp>
#include <array>
#include <cstddef>
#include <deque>
#include <list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

	// A lazy, non-contiguous range of bytes. byte_range must accept it so a view
	// can feed a hasher directly without being materialized first.
	constexpr auto lazy_bytes() noexcept
	{
		return std::views::iota(0, 8) | std::views::transform([](int i) noexcept {
			       return static_cast<unsigned char>(i);
		       });
	}

} // namespace

// Contiguous byte containers and views.
static_assert(alt::byte_range<std::string>);
static_assert(alt::byte_range<std::string_view>);
static_assert(alt::byte_range<std::vector<unsigned char>>);
static_assert(alt::byte_range<std::vector<std::byte>>);
static_assert(alt::byte_range<std::array<char, 4>>);
static_assert(alt::byte_range<std::span<const std::byte>>);
static_assert(alt::byte_range<std::u8string>);

// Non-contiguous ranges are still byte ranges.
static_assert(alt::byte_range<std::deque<unsigned char>>);
static_assert(alt::byte_range<std::list<std::byte>>);
static_assert(alt::byte_range<decltype(lazy_bytes())>);

// Every byte-like element type is accepted.
static_assert(alt::byte_range<std::vector<char>>);
static_assert(alt::byte_range<std::vector<signed char>>);
static_assert(alt::byte_range<std::vector<char8_t>>);

// Raw arrays of char and char8_t are excluded: they carry a NUL-termination
// convention, so hashing one whole would consume a byte past the string.
static_assert(!alt::byte_range<char[4]>);
static_assert(!alt::byte_range<const char (&)[4]>);
static_assert(!alt::byte_range<char8_t[4]>);

// Raw arrays of the other byte types carry no such convention and are accepted.
static_assert(alt::byte_range<unsigned char[4]>);
static_assert(alt::byte_range<std::byte[4]>);

// Non-byte ranges and non-ranges are rejected.
static_assert(!alt::byte_range<std::vector<int>>);
static_assert(!alt::byte_range<std::vector<char16_t>>);
static_assert(!alt::byte_range<std::wstring>);
static_assert(!alt::byte_range<int>);
static_assert(!alt::byte_range<std::byte>);

TEST(byte_range, accepts_a_lazy_view_of_bytes)
{
	const auto view = lazy_bytes();
	EXPECT_EQ(std::ranges::distance(view), 8);
	static_assert(alt::byte_range<decltype(view)>);
}

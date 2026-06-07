#include <gtest/gtest.h>

#include <alt/transcode.hpp>
#include <bit>

namespace detail = alt::ranges::views::detail;

TEST(TranscodeDetail, CodeUnitConcept)
{
	static_assert(detail::code_unit<char>);
	static_assert(detail::code_unit<char8_t>);
	static_assert(detail::code_unit<char16_t>);
	static_assert(detail::code_unit<char32_t>);
	static_assert(!detail::code_unit<wchar_t>);
	static_assert(!detail::code_unit<int>);
	SUCCEED();
}

TEST(TranscodeDetail, MaxUnits)
{
	static_assert(detail::max_units<char8_t> == 4);
	static_assert(detail::max_units<char16_t> == 2);
	static_assert(detail::max_units<char32_t> == 1);
	SUCCEED();
}

TEST(TranscodeDetail, OrderUnitByteSwaps)
{
	// 8-bit: always identity.
	EXPECT_EQ(detail::order_unit<char8_t>(char8_t{0x41}, std::endian::big), char8_t{0x41});

	// Native order: identity.
	EXPECT_EQ(detail::order_unit<char32_t>(char32_t{0x41}, std::endian::native), char32_t{0x41});

	// Non-native order: byte-swapped.
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ(detail::order_unit<char32_t>(char32_t{0x41}, other), std::byteswap(char32_t{0x41}));
	EXPECT_EQ(detail::order_unit<char16_t>(char16_t{0x41}, other), std::byteswap(char16_t{0x41}));
}

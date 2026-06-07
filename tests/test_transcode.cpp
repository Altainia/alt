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

TEST(TranscodeDetail, EncodeUtf8)
{
	detail::unit_buffer<char8_t> buf{};
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], char8_t{0x41});

	// U+00E9 (é) -> C3 A9
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'é', buf)), 2u);
	EXPECT_EQ(buf[0], char8_t{0xC3});
	EXPECT_EQ(buf[1], char8_t{0xA9});

	// U+20AC (€) -> E2 82 AC
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'€', buf)), 3u);
	EXPECT_EQ(buf[0], char8_t{0xE2});
	EXPECT_EQ(buf[1], char8_t{0x82});
	EXPECT_EQ(buf[2], char8_t{0xAC});

	// U+1F600 (😀) -> F0 9F 98 80
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'\U0001F600', buf)), 4u);
	EXPECT_EQ(buf[0], char8_t{0xF0});
	EXPECT_EQ(buf[1], char8_t{0x9F});
	EXPECT_EQ(buf[2], char8_t{0x98});
	EXPECT_EQ(buf[3], char8_t{0x80});
}

TEST(TranscodeDetail, EncodeUtf16)
{
	detail::unit_buffer<char16_t> buf{};
	// BMP
	EXPECT_EQ((detail::encode_one<char16_t, std::endian::native>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], char16_t{0x0041});

	// Supplementary: U+1F600 -> D83D DE00
	EXPECT_EQ((detail::encode_one<char16_t, std::endian::native>(U'\U0001F600', buf)), 2u);
	EXPECT_EQ(buf[0], char16_t{0xD83D});
	EXPECT_EQ(buf[1], char16_t{0xDE00});

	// Big-endian output byte-swaps each unit.
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ((detail::encode_one<char16_t, other>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], std::byteswap(char16_t{0x0041}));
}

TEST(TranscodeDetail, EncodeUtf32)
{
	detail::unit_buffer<char32_t> buf{};
	EXPECT_EQ((detail::encode_one<char32_t, std::endian::native>(U'\U0001F600', buf)), 1u);
	EXPECT_EQ(buf[0], char32_t{0x1F600});

	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ((detail::encode_one<char32_t, other>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], std::byteswap(char32_t{0x41}));
}

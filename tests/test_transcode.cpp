#include <gtest/gtest.h>

#include <alt/transcode.hpp>
#include <bit>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace detail = alt::ranges::views::detail;

template<std::endian SrcE = std::endian::native, typename R>
char32_t decode_first(const R& units)
{
	auto first = std::ranges::begin(units);
	auto last  = std::ranges::end(units);
	return detail::decode_one<SrcE>(first, last);
}

template<detail::code_unit C>
constexpr C ordered(C value, std::endian order)
{
	return detail::order_unit<C>(value, order);
}

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

// Encodes one code point and returns the produced target units as a string, so
// each case can be asserted against a whole expected sequence in one comparison.
template<detail::code_unit C, std::endian E = std::endian::native>
std::basic_string<C> encode_units(char32_t cp)
{
	detail::unit_buffer<C> buf{};
	const std::size_t      count = detail::encode_one<C, E>(cp, buf);
	return std::basic_string<C>(buf.data(), count);
}

TEST(TranscodeDetail, EncodeUtf8)
{
	EXPECT_EQ(encode_units<char8_t>(U'A'), (std::u8string{0x41}));                            // 1 byte
	EXPECT_EQ(encode_units<char8_t>(U'é'), (std::u8string{0xC3, 0xA9}));                      // 2 bytes
	EXPECT_EQ(encode_units<char8_t>(U'€'), (std::u8string{0xE2, 0x82, 0xAC}));                // 3 bytes
	EXPECT_EQ(encode_units<char8_t>(U'\U0001F600'), (std::u8string{0xF0, 0x9F, 0x98, 0x80})); // 4 bytes
}

TEST(TranscodeDetail, EncodeUtf16)
{
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ(encode_units<char16_t>(U'A'), (std::u16string{0x0041}));                  // BMP
	EXPECT_EQ(encode_units<char16_t>(U'\U0001F600'), (std::u16string{0xD83D, 0xDE00})); // surrogate pair
	// Big-endian output byte-swaps each unit.
	EXPECT_EQ((encode_units<char16_t, other>(U'A')), (std::u16string{std::byteswap(char16_t{0x0041})}));
}

TEST(TranscodeDetail, EncodeUtf32)
{
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ(encode_units<char32_t>(U'\U0001F600'), (std::u32string{0x1F600}));
	// Big-endian output byte-swaps the unit.
	EXPECT_EQ((encode_units<char32_t, other>(U'A')), (std::u32string{std::byteswap(char32_t{0x41})}));
}

TEST(TranscodeDetail, DecodeUtf8Valid)
{
	EXPECT_EQ(decode_first(std::u8string{u8"A"}), U'A');
	EXPECT_EQ(decode_first(std::u8string{u8"é"}), U'é');
	EXPECT_EQ(decode_first(std::u8string{u8"€"}), U'€');
	EXPECT_EQ(decode_first(std::u8string{u8"\U0001F600"}), U'\U0001F600');
}

TEST(TranscodeDetail, DecodeUtf8Invalid)
{
	// Lone continuation byte.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0x80}), detail::replacement_character);
	// Invalid lead byte.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xFF}), detail::replacement_character);
	// Truncated 2-byte sequence (lead with no continuation).
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xC3}), detail::replacement_character);
	// Overlong encoding of U+0000 (C0 80).
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xC0, 0x80}), detail::replacement_character);
	// Surrogate U+D800 encoded as UTF-8 (ED A0 80) -> rejected.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xED, 0xA0, 0x80}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeUtf16Valid)
{
	EXPECT_EQ(decode_first(std::u16string{u"A"}), U'A');
	EXPECT_EQ(decode_first(std::u16string{u"\U0001F600"}), U'\U0001F600'); // surrogate pair
}

TEST(TranscodeDetail, DecodeUtf16Invalid)
{
	// Lone high surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xD83D}), detail::replacement_character);
	// Lone low surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xDE00}), detail::replacement_character);
	// High surrogate followed by non-low-surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xD83D, 0x0041}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeUtf32Invalid)
{
	// Beyond U+10FFFF.
	EXPECT_EQ(decode_first(std::vector<char32_t>{0x110000}), detail::replacement_character);
	// Surrogate scalar.
	EXPECT_EQ(decode_first(std::vector<char32_t>{0xD800}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeRespectsSourceEndian)
{
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	// A big-endian-stored 'A' decodes back to U'A'.
	std::vector<char32_t> swapped{std::byteswap(char32_t{0x41})};
	EXPECT_EQ(decode_first<other>(swapped), U'A');
}

TEST(Transcode, Utf8ToUtf32Native)
{
	std::string s1 = "Hello world";
	auto        s2 = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	static_assert(std::same_as<decltype(s2), std::u32string>);
	EXPECT_EQ(s2, U"Hello world");
}

TEST(Transcode, NamespaceAliasesNameSameThing)
{
	std::string s1 = "hi";
	auto        a  = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	auto        b  = s1 | alt::views::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	auto        c  = s1 | alt::ranges::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	auto        d  = s1 | alt::ranges::views::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(a, b);
	EXPECT_EQ(a, c);
	EXPECT_EQ(a, d);
}

TEST(Transcode, SpecRoundTrips)
{
	std::string s1 = "Hello world";

	auto s2 = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	auto s3 = s1 | alt::transcode<char32_t, std::endian::big>() | std::ranges::to<std::basic_string>();
	auto s4 = s3 | alt::transcode<std::endian::big, char16_t>() | std::ranges::to<std::basic_string>();
	auto s5 = s3 | alt::transcode<std::endian::big, char16_t, std::endian::big>() | std::ranges::to<std::basic_string>();

	static_assert(std::same_as<decltype(s2), std::u32string>);
	static_assert(std::same_as<decltype(s4), std::u16string>);

	// s2: native UTF-32.
	EXPECT_EQ(s2, U"Hello world");

	// s3: big-endian UTF-32 (each unit byte-swapped on a little-endian host).
	std::u32string expected_s3;
	for(char32_t c: std::u32string(U"Hello world"))
	{
		expected_s3.push_back(ordered(c, std::endian::big));
	}
	EXPECT_EQ(s3, expected_s3);

	// s4: native UTF-16 recovered from big-endian UTF-32.
	EXPECT_EQ(s4, u"Hello world");

	// s5: big-endian UTF-16.
	std::u16string expected_s5;
	for(char16_t c: std::u16string(u"Hello world"))
	{
		expected_s5.push_back(ordered(c, std::endian::big));
	}
	EXPECT_EQ(s5, expected_s5);
}

TEST(Transcode, AllDirectionsBmpAndSupplementary)
{
	// Mix of ASCII, BMP (€ U+20AC), and supplementary (😀 U+1F600).
	std::string utf8 = "A€\U0001F600";

	auto u32 = utf8 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(u32, U"A€\U0001F600");

	auto u16 = u32 | alt::transcode<char16_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(u16, u"A€\U0001F600");

	auto back8 = u16 | alt::transcode<char8_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(back8, u8"A€\U0001F600");

	// char target is treated as UTF-8.
	auto back_char = u32 | alt::transcode<char>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(back_char, "A€\U0001F600");
}

TEST(Transcode, InvalidInputBecomesReplacement)
{
	// Invalid UTF-8 byte in the middle.
	std::vector<char8_t> bad8{u8'A', 0xFF, u8'B'};
	auto                 out           = bad8 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	const std::u32string expected_bad8 = {U'A', detail::replacement_character, U'B'};
	EXPECT_EQ(out, expected_bad8);

	// Lone high surrogate in UTF-16.
	std::vector<char16_t> bad16{u'A', 0xD83D, u'B'};
	auto                  out2           = bad16 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	const std::u32string  expected_bad16 = {U'A', detail::replacement_character, U'B'};
	EXPECT_EQ(out2, expected_bad16);

	// Out-of-range UTF-32 scalar.
	std::vector<char32_t> bad32{U'A', 0x110000, U'B'};
	auto                  out3           = bad32 | alt::transcode<char16_t>() | std::ranges::to<std::basic_string>();
	const std::u16string  expected_bad32 = {u'A', static_cast<char16_t>(detail::replacement_character), u'B'};
	EXPECT_EQ(out3, expected_bad32);
}

TEST(Transcode, EmptyInput)
{
	std::string empty;
	auto        out = empty | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_TRUE(out.empty());
}

TEST(Transcode, ComposesWithOtherAdaptors)
{
	std::string s1     = "Hello world";
	auto        first5 = s1 | alt::transcode<char32_t>() | std::views::take(5) | std::ranges::to<std::basic_string>();
	EXPECT_EQ(first5, U"Hello");

	// Source produced by another adaptor (rvalue range).
	auto filtered = std::string("aXbXc") | std::views::filter([](char c) { return c != 'X'; }) | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(filtered, U"abc");
}

TEST(Transcode, ModelsInputRange)
{
	using view_t = decltype(std::declval<std::string&>() | alt::transcode<char32_t>());
	static_assert(std::ranges::input_range<view_t>);
	static_assert(std::ranges::view<std::remove_cvref_t<view_t>>);
	SUCCEED();
}

TEST(Transcode, ConstexprEvaluation)
{
	constexpr auto first_unit = [] {
		std::u8string_view s    = u8"\U0001F600"; // 4 UTF-8 bytes -> 1 UTF-32 unit
		auto               view = s | alt::transcode<char32_t>();
		return *view.begin();
	}();
	static_assert(first_unit == U'\U0001F600');
	SUCCEED();
}

TEST(Transcode, ClosureComposesWithAdaptor)
{
	std::string s1 = "Hello world";
	// Compose the adaptor with another adaptor before applying it to a range.
	auto adaptor = alt::transcode<char32_t>() | std::views::take(5);
	auto first5  = s1 | adaptor | std::ranges::to<std::basic_string>();
	EXPECT_EQ(first5, U"Hello");
}

TEST(Transcode, BoundaryCodePoints)
{
	// Scalars straddling the encoding-length and surrogate boundaries.
	const std::u32string cps{
	  U'\U0000FFFF', U'\U00010000', // BMP/3-byte <-> supplementary/4-byte boundary
	  U'\U0000D7FF',
	  U'\U0000E000',  // just below / just above the surrogate range
	  U'\U0010FFFF'}; // maximum scalar value

	const auto u8  = cps | alt::transcode<char8_t>() | std::ranges::to<std::basic_string>();
	const auto u16 = cps | alt::transcode<char16_t>() | std::ranges::to<std::basic_string>();

	EXPECT_EQ(u8 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>(), cps);
	EXPECT_EQ(u16 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>(), cps);
}

TEST(Transcode, Utf32UpperSurrogateBoundaryBecomesReplacement)
{
	// U+DFFF is the top of the surrogate range (the existing test covers U+D800).
	const std::vector<char32_t> bad{U'A', 0xDFFF, U'B'};
	const std::u32string        expected{U'A', detail::replacement_character, U'B'};
	const auto                  out = bad | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(out, expected);
}

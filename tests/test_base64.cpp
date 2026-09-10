#include <gtest/gtest.h>

#include <alt/base64.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// --- RFC 4648 section 10 test vectors --------------------------------------
//
// The seven vectors the specification itself publishes. They cover all three
// tail remainders: "foo"/"foobar" need no padding, "f"/"foob" pad with "==",
// and "fo"/"fooba" pad with "=". Asserted at compile time so a regression
// breaks the build rather than the test run.

static_assert(alt::base64_encode("") == "");
static_assert(alt::base64_encode("f") == "Zg==");
static_assert(alt::base64_encode("fo") == "Zm8=");
static_assert(alt::base64_encode("foo") == "Zm9v");
static_assert(alt::base64_encode("foob") == "Zm9vYg==");
static_assert(alt::base64_encode("fooba") == "Zm9vYmE=");
static_assert(alt::base64_encode("foobar") == "Zm9vYmFy");

TEST(base64_encode, matches_the_rfc_4648_vectors)
{
	EXPECT_EQ(alt::base64_encode(std::string_view{""}), "");
	EXPECT_EQ(alt::base64_encode(std::string_view{"f"}), "Zg==");
	EXPECT_EQ(alt::base64_encode(std::string_view{"fo"}), "Zm8=");
	EXPECT_EQ(alt::base64_encode(std::string_view{"foo"}), "Zm9v");
	EXPECT_EQ(alt::base64_encode(std::string_view{"foob"}), "Zm9vYg==");
	EXPECT_EQ(alt::base64_encode(std::string_view{"fooba"}), "Zm9vYmE=");
	EXPECT_EQ(alt::base64_encode(std::string_view{"foobar"}), "Zm9vYmFy");
}

// --- decoding the same vectors ---------------------------------------------

namespace
{

	/** The decoded bytes of @p text, or a failure if it is not valid base64. */
	template<alt::base64_alphabet Alphabet = alt::base64_standard_alphabet>
	std::string decoded(std::string_view text)
	{
		const auto bytes = alt::base64_decode<Alphabet>(text);
		EXPECT_TRUE(bytes.has_value());
		std::string out;
		for(const std::byte b: bytes.value())
		{
			out.push_back(static_cast<char>(b));
		}
		return out;
	}

} // namespace

TEST(base64_decode, recovers_the_rfc_4648_vectors)
{
	EXPECT_EQ(decoded(""), "");
	EXPECT_EQ(decoded("Zg=="), "f");
	EXPECT_EQ(decoded("Zm8="), "fo");
	EXPECT_EQ(decoded("Zm9v"), "foo");
	EXPECT_EQ(decoded("Zm9vYg=="), "foob");
	EXPECT_EQ(decoded("Zm9vYmE="), "fooba");
	EXPECT_EQ(decoded("Zm9vYmFy"), "foobar");
}

// --- rejecting malformed input ---------------------------------------------

namespace
{

	/** The reason @p text was rejected, or nothing if it decoded successfully. */
	template<alt::base64_alphabet Alphabet = alt::base64_standard_alphabet>
	std::optional<alt::base64_error> rejection(std::string_view text)
	{
		const auto result = alt::base64_decode<Alphabet>(text);
		if(result.has_value())
		{
			return std::nullopt;
		}
		return result.error();
	}

	using enum alt::base64_error;

} // namespace

TEST(base64_decode, rejects_characters_outside_the_alphabet)
{
	// RFC 4648 section 3.3 requires this by default: non-alphabet characters are a
	// covert channel, so being liberal in what we accept is a security decision.
	EXPECT_EQ(rejection("Zm9!"), invalid_character);
	EXPECT_EQ(rejection("Zm 9v"), invalid_character);
	EXPECT_EQ(rejection("Zm9v\n"), invalid_character);
	EXPECT_EQ(rejection("Zm9v\r\nZm9v"), invalid_character);
	EXPECT_EQ(rejection("Zm9\xff"), invalid_character);
}

TEST(base64_decode, rejects_the_url_safe_characters_under_the_standard_alphabet)
{
	EXPECT_EQ(rejection("-w=="), invalid_character);
	EXPECT_EQ(rejection("_w=="), invalid_character);
}

TEST(base64_decode, rejects_lengths_that_encode_no_whole_number_of_bytes)
{
	// One character carries six bits, too few for even a single byte.
	EXPECT_EQ(rejection("Z"), invalid_length);
	EXPECT_EQ(rejection("Zm9vZ"), invalid_length);
}

TEST(base64_decode, rejects_a_final_quantum_that_was_never_padded)
{
	EXPECT_EQ(rejection("Zg"), missing_padding);
	EXPECT_EQ(rejection("Zg="), missing_padding);
	EXPECT_EQ(rejection("Zm8"), missing_padding);
	EXPECT_EQ(rejection("Zm9vYg"), missing_padding);
}

TEST(base64_decode, rejects_padding_that_is_not_at_the_end)
{
	EXPECT_EQ(rejection("=m9v"), unexpected_padding);
	EXPECT_EQ(rejection("Z=9v"), unexpected_padding);
	EXPECT_EQ(rejection("Zm=v"), unexpected_padding);
	EXPECT_EQ(rejection("Zg==Zg=="), unexpected_padding);
	EXPECT_EQ(rejection("Zm8=Zm9v"), unexpected_padding);

	// RFC 4648 section 3.3 calls out "===" specifically as excess padding. The pad
	// sits where the quantum's second data character must be, so that is the fault
	// reported, not the length.
	EXPECT_EQ(rejection("Z==="), unexpected_padding);
}

TEST(base64_decode, rejects_a_final_quantum_whose_discarded_bits_are_not_zero)
{
	// RFC 4648 section 3.5: "Zg==" and "Zh==" would both yield "f" under a decoder
	// that ignored the pad bits, so the encoding would not be canonical.
	EXPECT_EQ(alt::base64_decode("Zg=="), alt::base64_decode("Zg=="));
	EXPECT_EQ(rejection("Zh=="), non_canonical_bits);
	EXPECT_EQ(rejection("Zm9="), non_canonical_bits);
	EXPECT_EQ(rejection("ZZ=="), non_canonical_bits);
}

TEST(base64_decode, accepts_every_canonical_single_byte_encoding)
{
	// A one-byte quantum uses two characters, and only the sixteen second characters
	// whose low four bits are zero are canonical. Each must decode, and each of the
	// other forty-eight must not.
	for(std::size_t sextet = 0; sextet < 64; ++sextet)
	{
		const std::string text =
		  std::string{"A"} + alt::base64_standard_alphabet::characters[sextet] + "==";
		if(sextet % 16 == 0)
		{
			EXPECT_EQ(rejection(text), std::nullopt) << text;
		}
		else
		{
			EXPECT_EQ(rejection(text), non_canonical_bits) << text;
		}
	}
}

// --- alphabets -------------------------------------------------------------
//
// The URL-safe alphabet of RFC 4648 section 5 is identical to the standard one
// but for sextets 62 and 63. These two inputs are the ones that produce nothing
// else: 0xFB 0xEF 0xBE is four repetitions of 62, and 0xFF 0xFF 0xFF of 63.

namespace
{

	constexpr std::array<std::byte, 3> all_62{std::byte{0xFB}, std::byte{0xEF}, std::byte{0xBE}};
	constexpr std::array<std::byte, 3> all_63{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};

} // namespace

TEST(base64_alphabets, differ_only_in_the_last_two_characters)
{
	EXPECT_EQ(alt::base64_encode(all_62), "++++");
	EXPECT_EQ(alt::base64_encode(all_63), "////");
	EXPECT_EQ(alt::base64_encode<alt::base64_url_alphabet>(all_62), "----");
	EXPECT_EQ(alt::base64_encode<alt::base64_url_alphabet>(all_63), "____");

	EXPECT_EQ(alt::base64_url_alphabet::characters.substr(0, 62),
	          alt::base64_standard_alphabet::characters.substr(0, 62));
}

TEST(base64_alphabets, url_safe_round_trips_through_its_own_decoder)
{
	EXPECT_EQ(decoded<alt::base64_url_alphabet>("Zm9vYmFy"), "foobar");
	EXPECT_EQ(decoded<alt::base64_url_alphabet>("Zg=="), "f");
	EXPECT_EQ(rejection<alt::base64_url_alphabet>("+w=="), invalid_character);
	EXPECT_EQ(rejection<alt::base64_url_alphabet>("/w=="), invalid_character);
}

TEST(base64_alphabets, unpadded_omits_padding_on_both_sides)
{
	using unpadded = alt::base64_url_unpadded_alphabet;

	EXPECT_EQ(alt::base64_encode<unpadded>(std::string_view{""}), "");
	EXPECT_EQ(alt::base64_encode<unpadded>(std::string_view{"f"}), "Zg");
	EXPECT_EQ(alt::base64_encode<unpadded>(std::string_view{"fo"}), "Zm8");
	EXPECT_EQ(alt::base64_encode<unpadded>(std::string_view{"foo"}), "Zm9v");

	EXPECT_EQ(decoded<unpadded>("Zg"), "f");
	EXPECT_EQ(decoded<unpadded>("Zm8"), "fo");
	EXPECT_EQ(decoded<unpadded>("Zm9v"), "foo");

	// The pad character is simply not in this alphabet, so it is a stray character.
	EXPECT_EQ(rejection<unpadded>("Zg=="), invalid_character);
}

TEST(base64_alphabets, unpadded_still_rejects_a_lone_trailing_character)
{
	// Without padding to complete the quantum, a one-character tail is the only
	// thing marking the length as impossible.
	using unpadded = alt::base64_url_unpadded_alphabet;

	EXPECT_EQ(rejection<unpadded>("Z"), invalid_length);
	EXPECT_EQ(rejection<unpadded>("Zm9vZ"), invalid_length);
	EXPECT_EQ(rejection<unpadded>("Zh"), non_canonical_bits);
}

// A type is only an alphabet if it can actually describe one unambiguously.

namespace
{

	struct short_alphabet
	{
		static constexpr std::string_view name       = "short";
		static constexpr std::string_view characters = "ABC";
		static constexpr char             padding    = '=';
	};

	struct ambiguous_alphabet
	{
		static constexpr std::string_view name = "ambiguous";
		static constexpr std::string_view characters =
		  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+=";
		static constexpr char padding = '=';
	};

} // namespace

static_assert(alt::base64_alphabet<alt::base64_standard_alphabet>);
static_assert(alt::base64_alphabet<alt::base64_url_alphabet>);
static_assert(alt::base64_alphabet<alt::base64_url_unpadded_alphabet>);
static_assert(!alt::base64_alphabet<short_alphabet>);
static_assert(!alt::base64_alphabet<ambiguous_alphabet>);
static_assert(!alt::base64_alphabet<int>);

// --- sizes -----------------------------------------------------------------

static_assert(alt::base64_encoded_size(0) == 0);
static_assert(alt::base64_encoded_size(1) == 4);
static_assert(alt::base64_encoded_size(2) == 4);
static_assert(alt::base64_encoded_size(3) == 4);
static_assert(alt::base64_encoded_size(4) == 8);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(0) == 0);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(1) == 2);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(2) == 3);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(3) == 4);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(4) == 6);

TEST(base64_encoded_size, predicts_the_length_every_encoder_actually_produces)
{
	std::vector<std::byte> bytes;
	for(std::size_t length = 0; length < 64; ++length)
	{
		EXPECT_EQ(alt::base64_encode(bytes).size(), alt::base64_encoded_size(length)) << length;
		EXPECT_EQ(alt::base64_encode<alt::base64_url_unpadded_alphabet>(bytes).size(),
		          alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(length))
		  << length;
		bytes.push_back(static_cast<std::byte>(length));
	}
}

// --- round trips -----------------------------------------------------------

namespace
{

	/** A byte sequence of @p length covering the whole value range as it grows. */
	std::vector<std::byte> pattern(std::size_t length)
	{
		std::vector<std::byte> bytes;
		bytes.reserve(length);
		for(std::size_t i = 0; i < length; ++i)
		{
			bytes.push_back(static_cast<std::byte>((i * 7) ^ (i >> 3)));
		}
		return bytes;
	}

	/** Encodes then decodes @p bytes under @p Alphabet and returns what came back. */
	template<alt::base64_alphabet Alphabet>
	std::vector<std::byte> round_trip(const std::vector<std::byte>& bytes)
	{
		const std::string text    = alt::base64_encode<Alphabet>(bytes);
		const auto        decoded = alt::base64_decode<Alphabet>(text);
		EXPECT_TRUE(decoded.has_value()) << text;
		return decoded.value_or(std::vector<std::byte>{});
	}

} // namespace

TEST(base64_round_trip, recovers_every_input_under_every_alphabet)
{
	// Lengths 0 through 64 cross every quantum boundary many times over, so each
	// alphabet meets all three tail remainders repeatedly.
	for(std::size_t length = 0; length <= 64; ++length)
	{
		const std::vector<std::byte> bytes = pattern(length);
		EXPECT_EQ(round_trip<alt::base64_standard_alphabet>(bytes), bytes) << length;
		EXPECT_EQ(round_trip<alt::base64_url_alphabet>(bytes), bytes) << length;
		EXPECT_EQ(round_trip<alt::base64_url_unpadded_alphabet>(bytes), bytes) << length;
	}
}

TEST(base64_round_trip, handles_every_single_byte_value)
{
	for(int value = 0; value <= 0xFF; ++value)
	{
		const std::vector<std::byte> bytes{static_cast<std::byte>(value)};
		EXPECT_EQ(round_trip<alt::base64_standard_alphabet>(bytes), bytes) << value;
	}
}

// --- the views ------------------------------------------------------------

static_assert(std::same_as<std::ranges::range_value_t<decltype(std::string_view{} | alt::views::base64_encode())>, char>);

static_assert(std::same_as<std::ranges::range_value_t<decltype(std::string_view{} | alt::views::base64_decode())>,
                           std::expected<std::byte, alt::base64_error>>);

static_assert(
  std::same_as<std::ranges::range_value_t<decltype(std::string_view{} |
                                                   alt::views::base64_decode<alt::base64_standard_alphabet,
                                                                             alt::base64_throw_errors>())>,
               std::byte>);

TEST(base64_views, encode_composes_with_other_adaptors)
{
	auto        view = std::string_view{"foobar"} | alt::views::base64_encode() | std::views::take(4);
	std::string out;
	for(const char c: view)
	{
		out.push_back(c);
	}
	EXPECT_EQ(out, "Zm9v");
}

TEST(base64_views, encode_accepts_a_non_contiguous_range)
{
	const std::deque<std::byte> bytes{std::byte{'f'}, std::byte{'o'}, std::byte{'o'}, std::byte{'b'}, std::byte{'a'}, std::byte{'r'}};
	EXPECT_EQ(alt::base64_encode(bytes), "Zm9vYmFy");
}

TEST(base64_views, encode_accepts_a_lazy_range_of_unknown_size)
{
	const std::vector<std::byte> bytes = pattern(10);
	auto                         lazy  = bytes | std::views::filter([](std::byte) { return true; });

	static_assert(!std::ranges::sized_range<decltype(lazy)>);
	EXPECT_EQ(alt::base64_encode(lazy), alt::base64_encode(bytes));
}

TEST(base64_views, decode_surfaces_the_failure_as_a_final_element)
{
	auto                             view = std::string_view{"Zm9vYm9!"} | alt::views::base64_decode();
	std::vector<std::byte>           bytes;
	std::optional<alt::base64_error> failure;
	for(const auto& element: view)
	{
		if(element.has_value())
		{
			bytes.push_back(*element);
			continue;
		}
		failure = element.error();
	}

	// The three bytes of the first, well-formed quantum arrive before the failure,
	// and nothing follows it.
	EXPECT_EQ(bytes.size(), 3u);
	EXPECT_EQ(failure, invalid_character);
}

TEST(base64_views, decode_throws_under_the_throwing_policy)
{
	auto view = std::string_view{"Zm9!"} |
	            alt::views::base64_decode<alt::base64_standard_alphabet, alt::base64_throw_errors>();

	EXPECT_THROW(
	  {
		  for([[maybe_unused]] const std::byte b: view)
		  {
		  }
	  },
	  alt::base64_exception);

	try
	{
		for([[maybe_unused]] const std::byte b: view)
		{
		}
		FAIL() << "expected base64_exception";
	}
	catch(const alt::base64_exception& e)
	{
		EXPECT_EQ(e.error(), invalid_character);
		EXPECT_EQ(std::string_view{e.what()}, alt::base64_error_message(invalid_character));
	}
}

TEST(base64_views, decode_does_not_throw_on_well_formed_input)
{
	auto view = std::string_view{"Zm9vYmFy"} |
	            alt::views::base64_decode<alt::base64_standard_alphabet, alt::base64_throw_errors>();

	std::string out;
	for(const std::byte b: view)
	{
		out.push_back(static_cast<char>(b));
	}
	EXPECT_EQ(out, "foobar");
}

TEST(base64_views, decode_composes_with_a_filter_to_accept_mime_line_breaks)
{
	// RFC 4648 section 3.1: MIME wraps encoded data at 76 characters. Strict
	// decoding rejects the breaks, so a MIME caller strips them upstream.
	constexpr std::string_view wrapped = "Zm9v\r\nYmFy";
	EXPECT_EQ(rejection(wrapped), invalid_character);

	auto stripped = wrapped | std::views::filter([](char c) { return c != '\n' && c != '\r'; });

	std::string out;
	for(const auto& element: stripped | alt::views::base64_decode())
	{
		out.push_back(static_cast<char>(element.value()));
	}
	EXPECT_EQ(out, "foobar");
}

// --- constant expressions --------------------------------------------------

namespace
{

	constexpr bool decodes_in_a_constant_expression()
	{
		const auto bytes = alt::base64_decode("Zm9vYmFy");
		return bytes.has_value() && bytes->size() == 6 && bytes->front() == std::byte{'f'} &&
		       bytes->back() == std::byte{'r'};
	}

	constexpr bool rejects_in_a_constant_expression()
	{
		return alt::base64_decode("Zh==").error() == alt::base64_error::non_canonical_bits;
	}

} // namespace

static_assert(decodes_in_a_constant_expression());
static_assert(rejects_in_a_constant_expression());

// --- error messages --------------------------------------------------------

TEST(base64_error_message, describes_every_reason_distinctly)
{
	constexpr std::array<alt::base64_error, 5> all{invalid_character, invalid_length, unexpected_padding, missing_padding, non_canonical_bits};

	std::set<std::string_view> messages;
	for(const alt::base64_error error: all)
	{
		const std::string_view message = alt::base64_error_message(error);
		EXPECT_FALSE(message.empty());
		messages.insert(message);
	}
	EXPECT_EQ(messages.size(), all.size()) << "two reasons share a description";
}

TEST(base64_error_message, falls_back_for_a_value_that_names_no_reason)
{
	// Seven is representable by the enumeration but names nothing, so the fallback
	// is reachable without the undefined behavior a wildly out-of-range cast would
	// invoke.
	EXPECT_EQ(alt::base64_error_message(static_cast<alt::base64_error>(7)), "unknown base64 error");
}

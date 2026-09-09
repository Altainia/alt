#include <gtest/gtest.h>

#include <algorithm>
#include <alt/detail/digest.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{

	// Two distinct algorithms of identical digest size. SHA-256 and SHA-512/256 are a
	// real instance of this, which is why a digest is keyed on the algorithm rather
	// than on its length.
	struct fake_a
	{
		static constexpr std::size_t      digest_size = 4;
		static constexpr std::string_view name        = "FAKE-A";
	};

	struct fake_b
	{
		static constexpr std::size_t      digest_size = 4;
		static constexpr std::string_view name        = "FAKE-B";
	};

	using digest_a = alt::digest<fake_a>;
	using digest_b = alt::digest<fake_b>;

	constexpr digest_a sample{std::array<std::uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF}};

} // namespace

// --- identity --------------------------------------------------------------

static_assert(!std::same_as<digest_a, digest_b>);

// Digests of different algorithms do not compare, even at the same size.
static_assert(!std::equality_comparable_with<digest_a, digest_b>);
static_assert(std::equality_comparable<digest_a>);

static_assert(sample.size() == 4);
static_assert(sample.algorithm_name() == "FAKE-A");

// --- bytes -----------------------------------------------------------------

static_assert(sample[0] == 0xDE);
static_assert(sample[3] == 0xEF);
static_assert(sample.bytes().size() == 4);
static_assert(std::ranges::equal(sample, std::array<std::uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF}));

// A default-constructed digest is all zeros.
static_assert(digest_a{} == digest_a{std::array<std::uint8_t, 4>{0, 0, 0, 0}});

// --- hex output ------------------------------------------------------------

static_assert(std::ranges::equal(sample.to_hex(), std::string_view{"deadbeef"}));
static_assert(std::ranges::equal(digest_a{}.to_hex(), std::string_view{"00000000"}));

// --- hex parsing -----------------------------------------------------------

static_assert(digest_a::from_hex("deadbeef").value() == sample);

// Uppercase and mixed case parse; output is always lowercase.
static_assert(digest_a::from_hex("DEADBEEF").value() == sample);
static_assert(digest_a::from_hex("DeAdBeEf").value() == sample);

static_assert(digest_a::from_hex("deadbee").error() == alt::hex_error::invalid_length);
static_assert(digest_a::from_hex("deadbeef0").error() == alt::hex_error::invalid_length);
static_assert(digest_a::from_hex("").error() == alt::hex_error::invalid_length);
static_assert(digest_a::from_hex("deadbeeg").error() == alt::hex_error::invalid_character);
static_assert(digest_a::from_hex("dead beef").error() == alt::hex_error::invalid_length);
static_assert(digest_a::from_hex("dead-eef").error() == alt::hex_error::invalid_character);

// --- ordering --------------------------------------------------------------

static_assert(digest_a{} < sample);
static_assert(digest_a{std::array<std::uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEE}} < sample);
static_assert(sample <= sample);

TEST(digest, round_trips_through_hex)
{
	const auto text   = sample.to_string();
	const auto parsed = digest_a::from_hex(text);

	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(*parsed, sample);
	EXPECT_EQ(text, "deadbeef");
}

TEST(digest, formats_as_lowercase_hex)
{
	EXPECT_EQ(std::format("{}", sample), "deadbeef");
}

TEST(digest, is_usable_as_an_unordered_container_key)
{
	std::unordered_set<digest_a> seen;
	seen.insert(sample);
	seen.insert(sample);
	seen.insert(digest_a{});

	EXPECT_EQ(seen.size(), 2u);
	EXPECT_TRUE(seen.contains(sample));
}

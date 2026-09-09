#include <gtest/gtest.h>

#include <alt/sha1.hpp>
#include <concepts>
#include <cstddef>
#include <deque>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sha_test_support.hpp"

namespace
{

	constexpr std::string_view multi_block =
	  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	constexpr alt::sha1_digest expect(std::string_view hex)
	{
		return alt::sha1_digest::from_hex(hex).value();
	}

} // namespace

// --- FIPS 180-4 example vectors --------------------------------------------
//
// Asserted at compile time, so a regression breaks the build rather than the
// test run. A string literal binds to the string_view overload, which drops the
// terminating NUL; hashing it whole would produce a different digest entirely.

static_assert(alt::sha1("abc") == expect("a9993e364706816aba3e25717850c26c9cd0d89d"));
static_assert(alt::sha1("") == expect("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
static_assert(alt::sha1(multi_block) == expect("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

// --- digest identity -------------------------------------------------------

static_assert(alt::sha1_digest::size() == 20);
static_assert(alt::sha1_digest::algorithm_name() == "SHA-1");
static_assert(std::same_as<decltype(alt::sha1("abc")), alt::sha1_digest>);

TEST(sha1, hashes_the_one_million_character_vector)
{
	const std::string message(1000000, 'a');
	EXPECT_EQ(alt::sha1(message).to_string(), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(sha1, matches_reference_digests_at_every_padding_boundary)
{
	sha_test::check_boundaries(sha_vectors::sha1, [](const auto& m) { return alt::sha1(m); });
}

TEST(sha1, streaming_matches_one_shot_at_every_split)
{
	for(const std::size_t length: {0u, 1u, 55u, 56u, 64u, 65u, 128u, 200u})
	{
		sha_test::check_streaming_equivalence<alt::sha1_hasher>(
		  length, [](const auto& m) { return alt::sha1(m); });
	}
}

TEST(sha1, accepts_contiguous_and_lazy_ranges_alike)
{
	const auto expected = expect("a9993e364706816aba3e25717850c26c9cd0d89d");
	const std::vector<std::byte> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
	const std::deque<unsigned char> not_contiguous{'a', 'b', 'c'};
	const auto lazy = std::views::iota(0, 3) | std::views::transform([](int i) {
		                  return static_cast<unsigned char>('a' + i);
	                  });

	EXPECT_EQ(alt::sha1(std::string_view{"abc"}), expected);
	EXPECT_EQ(alt::sha1(bytes), expected);
	EXPECT_EQ(alt::sha1(std::span<const std::byte>{bytes}), expected);
	EXPECT_EQ(alt::sha1(not_contiguous), expected);
	EXPECT_EQ(alt::sha1(lazy), expected);
}

TEST(sha1, update_is_chainable_and_finish_leaves_the_hasher_usable)
{
	alt::sha1_hasher hasher;
	hasher.update(std::string_view{"a"}).update(std::string_view{"b"});

	const auto after_two = hasher.finish();
	EXPECT_EQ(after_two, alt::sha1("ab"));

	// finish() padded a copy, so the hasher still holds "ab" and can continue.
	hasher.update(std::string_view{"c"});
	EXPECT_EQ(hasher.finish(), alt::sha1("abc"));
}

TEST(sha1, reset_returns_the_hasher_to_its_initial_state)
{
	alt::sha1_hasher hasher;
	hasher.update(std::string_view{"discarded"});
	hasher.reset();
	hasher.update(std::string_view{"abc"});

	EXPECT_EQ(hasher.finish(), alt::sha1("abc"));
	EXPECT_EQ(alt::sha1_hasher{}.finish(), alt::sha1(""));
}

#include <gtest/gtest.h>

#include <alt/sha2.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sha_test_support.hpp"

namespace
{

	// FIPS 180-4 uses a 448-bit example message for the 512-bit-block algorithms and
	// an 896-bit one for the 1024-bit-block algorithms.
	constexpr std::string_view multi_block_512 =
	  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	constexpr std::string_view multi_block_1024 =
	  "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
	  "mnopqrstnopqrstu";

} // namespace

// --- FIPS 180-4 example vectors, asserted at compile time ------------------

static_assert(alt::sha224("abc") ==
              alt::sha224_digest::from_hex("23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7").value());

static_assert(alt::sha224(multi_block_512) ==
              alt::sha224_digest::from_hex("75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525").value());
static_assert(alt::sha224("") ==
              alt::sha224_digest::from_hex("d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f").value());

static_assert(
  alt::sha256("abc") ==
  alt::sha256_digest::from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").value());
static_assert(
  alt::sha256(multi_block_512) ==
  alt::sha256_digest::from_hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1").value());
static_assert(
  alt::sha256("") ==
  alt::sha256_digest::from_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855").value());

static_assert(alt::sha384("abc") ==
              alt::sha384_digest::from_hex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                                           "8086072ba1e7cc2358baeca134c825a7")
                .value());
static_assert(alt::sha384(multi_block_1024) ==
              alt::sha384_digest::from_hex("09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712"
                                           "fcc7c71a557e2db966c3e9fa91746039")
                .value());

static_assert(alt::sha512("abc") ==
              alt::sha512_digest::from_hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")
                .value());
static_assert(alt::sha512(multi_block_1024) ==
              alt::sha512_digest::from_hex("8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                                           "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909")
                .value());
static_assert(alt::sha512("") ==
              alt::sha512_digest::from_hex("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                                           "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e")
                .value());

static_assert(
  alt::sha512_224("abc") ==
  alt::sha512_224_digest::from_hex("4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa").value());
static_assert(
  alt::sha512_224(multi_block_1024) ==
  alt::sha512_224_digest::from_hex("23fec5bb94d60b23308192640b0c453335d664734fe40e7268674af9").value());

static_assert(alt::sha512_256("abc") ==
              alt::sha512_256_digest::from_hex(
                "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23")
                .value());
static_assert(alt::sha512_256(multi_block_1024) ==
              alt::sha512_256_digest::from_hex(
                "3928e184fb8690f840da3988121d31be65cb9d3ef83ee6146feac861e19b563a")
                .value());

// --- SHA-512/t initial values, section 5.3.6 -------------------------------
//
// The initial values are derived by running the standard's own IV generation
// function at compile time, not transcribed. These assertions check that
// derivation against the values FIPS 180-4 publishes in sections 5.3.6.1 and
// 5.3.6.2, so a mistake in the derivation cannot pass unnoticed.

static_assert(alt::sha512_224_algorithm::initial_value ==
              std::array<std::uint64_t, 8>{0x8C3D37C819544DA2ULL, 0x73E1996689DCD4D6ULL, 0x1DFAB7AE32FF9C82ULL, 0x679DD514582F9FCFULL, 0x0F6D2B697BD44DA8ULL, 0x77E36F7304C48942ULL, 0x3F9D85A86A1D36C8ULL, 0x1112E6AD91D692A1ULL});

static_assert(alt::sha512_256_algorithm::initial_value ==
              std::array<std::uint64_t, 8>{0x22312194FC2BF72CULL, 0x9F555FA3C84C64C2ULL, 0x2393B86B6F53B151ULL, 0x963877195940EABDULL, 0x96283EE2A88EFFE3ULL, 0xBE5E1E2553863992ULL, 0x2B0199FC2C85B8AAULL, 0x0EB72DDC81C52CA2ULL});

// --- digest identity -------------------------------------------------------

static_assert(alt::sha224_digest::size_bytes == 28);
static_assert(alt::sha256_digest::size_bytes == 32);
static_assert(alt::sha384_digest::size_bytes == 48);
static_assert(alt::sha512_digest::size_bytes == 64);
static_assert(alt::sha512_224_digest::size_bytes == 28);
static_assert(alt::sha512_256_digest::size_bytes == 32);

static_assert(alt::sha256_algorithm::name == "SHA-256");
static_assert(alt::sha512_256_algorithm::name == "SHA-512/256");

// Equal-length digests from different algorithms are different types.
static_assert(!std::same_as<alt::sha256_digest, alt::sha512_256_digest>);
static_assert(!std::equality_comparable_with<alt::sha256_digest, alt::sha512_256_digest>);
static_assert(!std::equality_comparable_with<alt::sha224_digest, alt::sha512_224_digest>);

TEST(sha2, hashes_the_one_million_character_vector)
{
	const std::string message(1000000, 'a');

	EXPECT_EQ(alt::sha224(message).to_string(), "20794655980c91d8bbb4c1ea97618a4bf03f42581948b2ee4ee7ad67");
	EXPECT_EQ(alt::sha256(message).to_string(),
	          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	EXPECT_EQ(alt::sha384(message).to_string(),
	          "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985");
	EXPECT_EQ(alt::sha512(message).to_string(),
	          "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31b"
	          "eb009c5c2c49aa2e4eadb217ad8cc09b");
	EXPECT_EQ(alt::sha512_224(message).to_string(), "37ab331d76f0d36de422bd0edeb22a28accd487b7a8453ae965dd287");
	EXPECT_EQ(alt::sha512_256(message).to_string(),
	          "9a59a052930187a97038cae692f30708aa6491923ef5194394dc68d56c74fb21");
}

TEST(sha2, matches_reference_digests_at_every_padding_boundary)
{
	sha_test::check_boundaries(sha_vectors::sha224, [](const auto& m) { return alt::sha224(m); });
	sha_test::check_boundaries(sha_vectors::sha256, [](const auto& m) { return alt::sha256(m); });
	sha_test::check_boundaries(sha_vectors::sha384, [](const auto& m) { return alt::sha384(m); });
	sha_test::check_boundaries(sha_vectors::sha512, [](const auto& m) { return alt::sha512(m); });
	sha_test::check_boundaries(sha_vectors::sha512_224, [](const auto& m) { return alt::sha512_224(m); });
	sha_test::check_boundaries(sha_vectors::sha512_256, [](const auto& m) { return alt::sha512_256(m); });
}

TEST(sha2, streaming_matches_one_shot_at_every_split_for_the_512_bit_block_family)
{
	for(const std::size_t length: {0u, 1u, 55u, 56u, 64u, 65u, 128u, 200u})
	{
		sha_test::check_streaming_equivalence<alt::sha256_hasher>(
		  length, [](const auto& m) { return alt::sha256(m); });
	}
}

TEST(sha2, streaming_matches_one_shot_at_every_split_for_the_1024_bit_block_family)
{
	for(const std::size_t length: {0u, 1u, 111u, 112u, 128u, 129u, 256u, 300u})
	{
		sha_test::check_streaming_equivalence<alt::sha512_hasher>(
		  length, [](const auto& m) { return alt::sha512(m); });
	}
}

TEST(sha2, accepts_contiguous_and_lazy_ranges_alike)
{
	const auto expected =
	  alt::sha256_digest::from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").value();
	const std::vector<std::byte>    bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
	const std::deque<unsigned char> not_contiguous{'a', 'b', 'c'};
	const auto                      lazy = std::views::iota(0, 3) | std::views::transform([](int i) {
                      return static_cast<unsigned char>('a' + i);
                    });

	EXPECT_EQ(alt::sha256(bytes), expected);
	EXPECT_EQ(alt::sha256(std::span<const std::byte>{bytes}), expected);
	EXPECT_EQ(alt::sha256(not_contiguous), expected);
	EXPECT_EQ(alt::sha256(lazy), expected);
}

TEST(sha2, a_truncated_variant_is_not_a_prefix_of_its_untruncated_parent)
{
	// FIPS 180-4 section 6.5: SHA-384 is SHA-512 with a different initial value and a
	// truncated output. The truncation itself is a plain prefix, which this pins.
	const std::string message(500, 'x');

	const auto full      = alt::sha512(message);
	const auto truncated = alt::sha384(message);

	EXPECT_NE(std::string_view{truncated.to_string()}, std::string_view{full.to_string()}.substr(0, 96))
	  << "different initial values must produce unrelated digests";
	EXPECT_EQ(truncated.size(), 48u);
}

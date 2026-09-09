#include <gtest/gtest.h>

#include <alt/detail/hash.hpp>
#include <array>
#include <cstdint>

// The FIPS 180-4 message length ceilings are unreachable by a real message: the
// smaller of them, 2^61 bytes, is roughly seventy-three years of hashing at a
// gigabyte per second. They are not, however, untestable. The counter and the
// length encoding are factored out of the streaming path precisely so a test can
// set a byte count directly and inspect the encoded padding field.
//
// What this file verifies is the length encoding, not a digest of a hypothetical
// exabyte message, which remains uncomputable.

namespace
{

	using alt::detail::byte_counter;
	using alt::detail::encode_bit_length;

	constexpr std::array<std::uint8_t, 8> encode64(std::uint64_t bytes) noexcept
	{
		return encode_bit_length(byte_counter<8>{bytes});
	}

	constexpr std::array<std::uint8_t, 16> encode128(std::uint64_t low, std::uint64_t high) noexcept
	{
		return encode_bit_length(byte_counter<16>{low, high});
	}

	constexpr std::array<std::uint8_t, 8>  zeros64{};
	constexpr std::array<std::uint8_t, 16> zeros128{};

} // namespace

// --- 64-bit length field (SHA-1, SHA-224, SHA-256) -------------------------

static_assert(encode64(0) == zeros64);

// One byte is eight bits, big-endian in the trailing field.
static_assert(encode64(1) == std::array<std::uint8_t, 8>{0, 0, 0, 0, 0, 0, 0, 0x08});

// The "abc" vector: three bytes, 24 bits, as printed in FIPS 180-4 section 5.1.1.
static_assert(encode64(3) == std::array<std::uint8_t, 8>{0, 0, 0, 0, 0, 0, 0, 0x18});

// The largest message the standard defines for this family: 2^61 - 1 bytes,
// one bit under the 2^64-bit ceiling.
static_assert(encode64((std::uint64_t{1} << 61) - 1) ==
              std::array<std::uint8_t, 8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8});

// The first length outside the standard's domain. The field wraps; behavior here
// is a precondition violation, pinned by assertion so it is known rather than
// inferred.
static_assert(encode64(std::uint64_t{1} << 61) == zeros64);

// --- 128-bit length field (SHA-384, SHA-512, SHA-512/224, SHA-512/256) -----

static_assert(encode128(0, 0) == zeros128);

static_assert(encode128(1, 0) ==
              std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08});

// 2^64 bytes: the point a 64-bit byte counter would have wrapped at, which lies
// inside the domain this family defines. The 128-bit counter carries instead.
static_assert(encode128(0, 1) ==
              std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0, 0, 0});

// The largest message the standard defines for this family: 2^125 - 1 bytes.
static_assert(encode128(~std::uint64_t{0}, (std::uint64_t{1} << 61) - 1) ==
              std::array<std::uint8_t, 16>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8});

// The first length outside it.
static_assert(encode128(0, std::uint64_t{1} << 61) == zeros128);

// --- carry propagation -----------------------------------------------------

constexpr byte_counter<16> advanced_across_the_word_boundary() noexcept
{
	byte_counter<16> counter{~std::uint64_t{0}, 0};
	counter += 1;
	return counter;
}

static_assert(advanced_across_the_word_boundary().low() == 0);
static_assert(advanced_across_the_word_boundary().high() == 1);

constexpr byte_counter<8> accumulated(std::uint64_t times, std::uint64_t step) noexcept
{
	byte_counter<8> counter{};
	for(std::uint64_t i = 0; i < times; ++i)
	{
		counter += step;
	}
	return counter;
}

static_assert(accumulated(10, 64).low() == 640);

TEST(byte_counter, accumulates_across_many_updates)
{
	byte_counter<16> counter{};
	for(int i = 0; i < 1000; ++i)
	{
		counter += 1024;
	}
	EXPECT_EQ(counter.low(), 1024u * 1000u);
	EXPECT_EQ(counter.high(), 0u);
}

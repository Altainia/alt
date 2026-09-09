#pragma once

#include <alt/concepts.hpp>
#include <alt/detail/digest.hpp>
#include <alt/detail/hash.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace alt
{

	/**
	 * @brief SHA-1, as specified by FIPS 180-4 sections 4.1.1, 4.2.1, 5.3.1, and 6.1.
	 *
	 * @warning SHA-1 is deprecated. NIST is transitioning away from it for all
	 *          applications by 31 December 2030 and intends to publish FIPS 180-5
	 *          removing its specification. It is not collision resistant and must not
	 *          be used where collision resistance matters. It remains available here
	 *          because protocols that specify it as a non-security checksum still
	 *          require it, notably the WebSocket handshake of RFC 6455 section 4.2.2.
	 *          For new work, prefer @c alt::sha256 or @c alt::sha512.
	 */
	struct sha1_algorithm
	{
		/** Word type of the state and message schedule. */
		using word_type = std::uint32_t;

		/** The five-word chaining state. */
		using state_type = std::array<word_type, 5>;

		/** Bytes per compression block. */
		static constexpr std::size_t block_size = 64;

		/** Bytes of trailing message-length field; also selects the byte-counter width. */
		static constexpr std::size_t length_size = 8;

		/** Bytes of digest produced. */
		static constexpr std::size_t digest_size = 20;

		/** Human-readable algorithm name. */
		static constexpr std::string_view name = "SHA-1";

		/** Initial hash value, FIPS 180-4 section 5.3.1. */
		static constexpr state_type initial_value{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};

		/**
		 * @brief Compresses one 64-byte block into @p state, FIPS 180-4 section 6.1.2.
		 *
		 * @param state Chaining state, updated in place.
		 * @param block The block to absorb.
		 */
		static constexpr void compress(state_type&                               state,
		                               std::span<const std::uint8_t, block_size> block) noexcept
		{
			// Step 1: prepare the message schedule.
			std::array<word_type, 80> w{};
			for(std::size_t t = 0; t < 16; ++t)
			{
				w[t] = detail::read_be<word_type>(block, t * 4);
			}
			for(std::size_t t = 16; t < 80; ++t)
			{
				w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
			}

			// Step 2: initialize the working variables.
			word_type a = state[0];
			word_type b = state[1];
			word_type c = state[2];
			word_type d = state[3];
			word_type e = state[4];

			// Step 3: eighty rounds, with f and K taken from the round's quarter.
			for(std::size_t t = 0; t < 80; ++t)
			{
				word_type f = 0;
				word_type k = 0;
				if(t < 20)
				{
					f = (b & c) | (~b & d); // Ch
					k = 0x5a827999u;
				}
				else if(t < 40)
				{
					f = b ^ c ^ d; // Parity
					k = 0x6ed9eba1u;
				}
				else if(t < 60)
				{
					f = (b & c) | (b & d) | (c & d); // Maj
					k = 0x8f1bbcdcu;
				}
				else
				{
					f = b ^ c ^ d; // Parity
					k = 0xca62c1d6u;
				}

				const word_type temp = std::rotl(a, 5) + f + e + k + w[t];
				e                    = d;
				d                    = c;
				c                    = std::rotl(b, 30);
				b                    = a;
				a                    = temp;
			}

			// Step 4: the intermediate hash value.
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
		}
	};

	/** A SHA-1 digest: 20 bytes, distinct from every other algorithm's digest type. */
	using sha1_digest = digest<sha1_algorithm>;

	/** An incremental SHA-1 hasher. */
	using sha1_hasher = hasher<sha1_algorithm>;

	/**
	 * @brief Returns the SHA-1 digest of @p input.
	 *
	 * @param input Any range of bytes, contiguous or lazy.
	 */
	[[nodiscard]] constexpr sha1_digest sha1(byte_range auto&& input)
	{
		return detail::one_shot<sha1_algorithm>(input);
	}

	/**
	 * @brief Returns the SHA-1 digest of the characters of @p input.
	 *
	 * This overload is what a string literal binds to, and it hashes exactly the
	 * characters of the string, excluding the terminating NUL. Binary data held in a
	 * @c char buffer must be passed as a @c std::span instead: converting it to a
	 * @c std::string_view would stop at the first NUL.
	 */
	[[nodiscard]] constexpr sha1_digest sha1(std::string_view input)
	{
		return detail::one_shot<sha1_algorithm>(input);
	}

} // namespace alt

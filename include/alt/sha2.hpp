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
	namespace detail
	{

		/**
		 * @brief The machinery shared by SHA-224 and SHA-256, FIPS 180-4 sections
		 *        4.1.2, 4.2.2, and 6.2.2.
		 *
		 * The two algorithms differ only in initial value and digest length, so
		 * everything else lives here and each derives.
		 */
		struct sha256_core
		{
			using word_type  = std::uint32_t;
			using state_type = std::array<word_type, 8>;

			static constexpr std::size_t block_size  = 64;
			static constexpr std::size_t length_size = 8;

			/** Round constants: the first 32 bits of the cube roots of the first 64 primes. */
			static constexpr std::array<word_type, 64> k{
			  0x428a2f98u,
			  0x71374491u,
			  0xb5c0fbcfu,
			  0xe9b5dba5u,
			  0x3956c25bu,
			  0x59f111f1u,
			  0x923f82a4u,
			  0xab1c5ed5u,
			  0xd807aa98u,
			  0x12835b01u,
			  0x243185beu,
			  0x550c7dc3u,
			  0x72be5d74u,
			  0x80deb1feu,
			  0x9bdc06a7u,
			  0xc19bf174u,
			  0xe49b69c1u,
			  0xefbe4786u,
			  0x0fc19dc6u,
			  0x240ca1ccu,
			  0x2de92c6fu,
			  0x4a7484aau,
			  0x5cb0a9dcu,
			  0x76f988dau,
			  0x983e5152u,
			  0xa831c66du,
			  0xb00327c8u,
			  0xbf597fc7u,
			  0xc6e00bf3u,
			  0xd5a79147u,
			  0x06ca6351u,
			  0x14292967u,
			  0x27b70a85u,
			  0x2e1b2138u,
			  0x4d2c6dfcu,
			  0x53380d13u,
			  0x650a7354u,
			  0x766a0abbu,
			  0x81c2c92eu,
			  0x92722c85u,
			  0xa2bfe8a1u,
			  0xa81a664bu,
			  0xc24b8b70u,
			  0xc76c51a3u,
			  0xd192e819u,
			  0xd6990624u,
			  0xf40e3585u,
			  0x106aa070u,
			  0x19a4c116u,
			  0x1e376c08u,
			  0x2748774cu,
			  0x34b0bcb5u,
			  0x391c0cb3u,
			  0x4ed8aa4au,
			  0x5b9cca4fu,
			  0x682e6ff3u,
			  0x748f82eeu,
			  0x78a5636fu,
			  0x84c87814u,
			  0x8cc70208u,
			  0x90befffau,
			  0xa4506cebu,
			  0xbef9a3f7u,
			  0xc67178f2u};

			/** Compresses one 64-byte block into @p state. */
			static constexpr void compress(state_type&                               state,
			                               std::span<const std::uint8_t, block_size> block) noexcept
			{
				std::array<word_type, 64> w{};
				for(std::size_t t = 0; t < 16; ++t)
				{
					w[t] = read_be<word_type>(block, t * 4);
				}
				for(std::size_t t = 16; t < 64; ++t)
				{
					const word_type s0 =
					  std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
					const word_type s1 =
					  std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
					w[t] = w[t - 16] + s0 + w[t - 7] + s1;
				}

				word_type a = state[0];
				word_type b = state[1];
				word_type c = state[2];
				word_type d = state[3];
				word_type e = state[4];
				word_type f = state[5];
				word_type g = state[6];
				word_type h = state[7];

				for(std::size_t t = 0; t < 64; ++t)
				{
					const word_type sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const word_type ch     = (e & f) ^ (~e & g);
					const word_type t1     = h + sigma1 + ch + k[t] + w[t];

					const word_type sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const word_type maj    = (a & b) ^ (a & c) ^ (b & c);
					const word_type t2     = sigma0 + maj;

					h = g;
					g = f;
					f = e;
					e = d + t1;
					d = c;
					c = b;
					b = a;
					a = t1 + t2;
				}

				state[0] += a;
				state[1] += b;
				state[2] += c;
				state[3] += d;
				state[4] += e;
				state[5] += f;
				state[6] += g;
				state[7] += h;
			}
		};

		/**
		 * @brief The machinery shared by SHA-384, SHA-512, SHA-512/224, and SHA-512/256,
		 *        FIPS 180-4 sections 4.1.3, 4.2.3, and 6.4.2.
		 */
		struct sha512_core
		{
			using word_type  = std::uint64_t;
			using state_type = std::array<word_type, 8>;

			static constexpr std::size_t block_size  = 128;
			static constexpr std::size_t length_size = 16;

			/** Round constants: the first 64 bits of the cube roots of the first 80 primes. */
			static constexpr std::array<word_type, 80> k{
			  0x428a2f98d728ae22ULL,
			  0x7137449123ef65cdULL,
			  0xb5c0fbcfec4d3b2fULL,
			  0xe9b5dba58189dbbcULL,
			  0x3956c25bf348b538ULL,
			  0x59f111f1b605d019ULL,
			  0x923f82a4af194f9bULL,
			  0xab1c5ed5da6d8118ULL,
			  0xd807aa98a3030242ULL,
			  0x12835b0145706fbeULL,
			  0x243185be4ee4b28cULL,
			  0x550c7dc3d5ffb4e2ULL,
			  0x72be5d74f27b896fULL,
			  0x80deb1fe3b1696b1ULL,
			  0x9bdc06a725c71235ULL,
			  0xc19bf174cf692694ULL,
			  0xe49b69c19ef14ad2ULL,
			  0xefbe4786384f25e3ULL,
			  0x0fc19dc68b8cd5b5ULL,
			  0x240ca1cc77ac9c65ULL,
			  0x2de92c6f592b0275ULL,
			  0x4a7484aa6ea6e483ULL,
			  0x5cb0a9dcbd41fbd4ULL,
			  0x76f988da831153b5ULL,
			  0x983e5152ee66dfabULL,
			  0xa831c66d2db43210ULL,
			  0xb00327c898fb213fULL,
			  0xbf597fc7beef0ee4ULL,
			  0xc6e00bf33da88fc2ULL,
			  0xd5a79147930aa725ULL,
			  0x06ca6351e003826fULL,
			  0x142929670a0e6e70ULL,
			  0x27b70a8546d22ffcULL,
			  0x2e1b21385c26c926ULL,
			  0x4d2c6dfc5ac42aedULL,
			  0x53380d139d95b3dfULL,
			  0x650a73548baf63deULL,
			  0x766a0abb3c77b2a8ULL,
			  0x81c2c92e47edaee6ULL,
			  0x92722c851482353bULL,
			  0xa2bfe8a14cf10364ULL,
			  0xa81a664bbc423001ULL,
			  0xc24b8b70d0f89791ULL,
			  0xc76c51a30654be30ULL,
			  0xd192e819d6ef5218ULL,
			  0xd69906245565a910ULL,
			  0xf40e35855771202aULL,
			  0x106aa07032bbd1b8ULL,
			  0x19a4c116b8d2d0c8ULL,
			  0x1e376c085141ab53ULL,
			  0x2748774cdf8eeb99ULL,
			  0x34b0bcb5e19b48a8ULL,
			  0x391c0cb3c5c95a63ULL,
			  0x4ed8aa4ae3418acbULL,
			  0x5b9cca4f7763e373ULL,
			  0x682e6ff3d6b2b8a3ULL,
			  0x748f82ee5defb2fcULL,
			  0x78a5636f43172f60ULL,
			  0x84c87814a1f0ab72ULL,
			  0x8cc702081a6439ecULL,
			  0x90befffa23631e28ULL,
			  0xa4506cebde82bde9ULL,
			  0xbef9a3f7b2c67915ULL,
			  0xc67178f2e372532bULL,
			  0xca273eceea26619cULL,
			  0xd186b8c721c0c207ULL,
			  0xeada7dd6cde0eb1eULL,
			  0xf57d4f7fee6ed178ULL,
			  0x06f067aa72176fbaULL,
			  0x0a637dc5a2c898a6ULL,
			  0x113f9804bef90daeULL,
			  0x1b710b35131c471bULL,
			  0x28db77f523047d84ULL,
			  0x32caab7b40c72493ULL,
			  0x3c9ebe0a15c9bebcULL,
			  0x431d67c49c100d4cULL,
			  0x4cc5d4becb3e42b6ULL,
			  0x597f299cfc657e2aULL,
			  0x5fcb6fab3ad6faecULL,
			  0x6c44198c4a475817ULL};

			/** Compresses one 128-byte block into @p state. */
			static constexpr void compress(state_type&                               state,
			                               std::span<const std::uint8_t, block_size> block) noexcept
			{
				std::array<word_type, 80> w{};
				for(std::size_t t = 0; t < 16; ++t)
				{
					w[t] = read_be<word_type>(block, t * 8);
				}
				for(std::size_t t = 16; t < 80; ++t)
				{
					const word_type s0 =
					  std::rotr(w[t - 15], 1) ^ std::rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
					const word_type s1 =
					  std::rotr(w[t - 2], 19) ^ std::rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
					w[t] = w[t - 16] + s0 + w[t - 7] + s1;
				}

				word_type a = state[0];
				word_type b = state[1];
				word_type c = state[2];
				word_type d = state[3];
				word_type e = state[4];
				word_type f = state[5];
				word_type g = state[6];
				word_type h = state[7];

				for(std::size_t t = 0; t < 80; ++t)
				{
					const word_type sigma1 = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
					const word_type ch     = (e & f) ^ (~e & g);
					const word_type t1     = h + sigma1 + ch + k[t] + w[t];

					const word_type sigma0 = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
					const word_type maj    = (a & b) ^ (a & c) ^ (b & c);
					const word_type t2     = sigma0 + maj;

					h = g;
					g = f;
					f = e;
					e = d + t1;
					d = c;
					c = b;
					b = a;
					a = t1 + t2;
				}

				state[0] += a;
				state[1] += b;
				state[2] += c;
				state[3] += d;
				state[4] += e;
				state[5] += f;
				state[6] += g;
				state[7] += h;
			}
		};

		/** Initial hash value for SHA-512, FIPS 180-4 section 5.3.5. */
		inline constexpr sha512_core::state_type sha512_initial_value{
		  0x6a09e667f3bcc908ULL,
		  0xbb67ae8584caa73bULL,
		  0x3c6ef372fe94f82bULL,
		  0xa54ff53a5f1d36f1ULL,
		  0x510e527fade682d1ULL,
		  0x9b05688c2b3e6c1fULL,
		  0x1f83d9abfb41bd6bULL,
		  0x5be0cd19137e2179ULL};

		/**
		 * @brief The SHA-512/t IV generation function of FIPS 180-4 section 5.3.6.
		 *
		 * XORs the SHA-512 initial value with a5a5..a5, then runs SHA-512 over the
		 * algorithm's own name using that as the initial value. The result is the
		 * initial value for SHA-512/t.
		 *
		 * Deriving the values rather than transcribing them means the published
		 * constants in sections 5.3.6.1 and 5.3.6.2 serve as an independent check on
		 * this implementation, which is what test_sha2.cpp asserts.
		 *
		 * @param label The algorithm name, e.g. @c "SHA-512/256". Must be short enough
		 *              to pad into a single block, which every approved value is.
		 */
		[[nodiscard]] constexpr sha512_core::state_type sha512_t_initial_value(std::string_view label) noexcept
		{
			sha512_core::state_type state = sha512_initial_value;
			for(auto& word: state)
			{
				word ^= 0xa5a5a5a5a5a5a5a5ULL;
			}

			std::array<std::uint8_t, sha512_core::block_size> block{};
			for(std::size_t i = 0; i < label.size(); ++i)
			{
				block[i] = static_cast<std::uint8_t>(label[i]);
			}
			block[label.size()] = 0x80;
			write_be<std::uint64_t>(block, sha512_core::block_size - 8, label.size() * 8);

			sha512_core::compress(state,
			                      std::span<const std::uint8_t, sha512_core::block_size>{block});
			return state;
		}

	} // namespace detail

	/** SHA-224, FIPS 180-4 sections 5.3.2 and 6.3. SHA-256 truncated, with its own initial value. */
	struct sha224_algorithm: detail::sha256_core
	{
		static constexpr std::size_t      digest_size = 28;
		static constexpr std::string_view name        = "SHA-224";

		/** Initial hash value, FIPS 180-4 section 5.3.2. */
		static constexpr state_type initial_value{0xc1059ed8u, 0x367cd507u, 0x3070dd17u, 0xf70e5939u, 0xffc00b31u, 0x68581511u, 0x64f98fa7u, 0xbefa4fa4u};
	};

	/** SHA-256, FIPS 180-4 sections 5.3.3 and 6.2. */
	struct sha256_algorithm: detail::sha256_core
	{
		static constexpr std::size_t      digest_size = 32;
		static constexpr std::string_view name        = "SHA-256";

		/** Initial hash value, FIPS 180-4 section 5.3.3. */
		static constexpr state_type initial_value{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	};

	/** SHA-384, FIPS 180-4 sections 5.3.4 and 6.5. SHA-512 truncated, with its own initial value. */
	struct sha384_algorithm: detail::sha512_core
	{
		static constexpr std::size_t      digest_size = 48;
		static constexpr std::string_view name        = "SHA-384";

		/** Initial hash value, FIPS 180-4 section 5.3.4. */
		static constexpr state_type initial_value{
		  0xcbbb9d5dc1059ed8ULL,
		  0x629a292a367cd507ULL,
		  0x9159015a3070dd17ULL,
		  0x152fecd8f70e5939ULL,
		  0x67332667ffc00b31ULL,
		  0x8eb44a8768581511ULL,
		  0xdb0c2e0d64f98fa7ULL,
		  0x47b5481dbefa4fa4ULL};
	};

	/** SHA-512, FIPS 180-4 sections 5.3.5 and 6.4. */
	struct sha512_algorithm: detail::sha512_core
	{
		static constexpr std::size_t      digest_size = 64;
		static constexpr std::string_view name        = "SHA-512";

		/** Initial hash value, FIPS 180-4 section 5.3.5. */
		static constexpr state_type initial_value = detail::sha512_initial_value;
	};

	/** SHA-512/224, FIPS 180-4 sections 5.3.6.1 and 6.6. */
	struct sha512_224_algorithm: detail::sha512_core
	{
		static constexpr std::size_t      digest_size = 28;
		static constexpr std::string_view name        = "SHA-512/224";

		/** Initial hash value, derived by the section 5.3.6 IV generation function. */
		static constexpr state_type initial_value = detail::sha512_t_initial_value("SHA-512/224");
	};

	/** SHA-512/256, FIPS 180-4 sections 5.3.6.2 and 6.7. */
	struct sha512_256_algorithm: detail::sha512_core
	{
		static constexpr std::size_t      digest_size = 32;
		static constexpr std::string_view name        = "SHA-512/256";

		/** Initial hash value, derived by the section 5.3.6 IV generation function. */
		static constexpr state_type initial_value = detail::sha512_t_initial_value("SHA-512/256");
	};

	/** A SHA-224 digest: 28 bytes, distinct from a SHA-512/224 digest of the same length. */
	using sha224_digest = digest<sha224_algorithm>;
	/** A SHA-256 digest: 32 bytes, distinct from a SHA-512/256 digest of the same length. */
	using sha256_digest = digest<sha256_algorithm>;
	/** A SHA-384 digest: 48 bytes. */
	using sha384_digest = digest<sha384_algorithm>;
	/** A SHA-512 digest: 64 bytes. */
	using sha512_digest = digest<sha512_algorithm>;
	/** A SHA-512/224 digest: 28 bytes, distinct from a SHA-224 digest of the same length. */
	using sha512_224_digest = digest<sha512_224_algorithm>;
	/** A SHA-512/256 digest: 32 bytes, distinct from a SHA-256 digest of the same length. */
	using sha512_256_digest = digest<sha512_256_algorithm>;

	/** An incremental SHA-224 hasher. */
	using sha224_hasher = hasher<sha224_algorithm>;
	/** An incremental SHA-256 hasher. */
	using sha256_hasher = hasher<sha256_algorithm>;
	/** An incremental SHA-384 hasher. */
	using sha384_hasher = hasher<sha384_algorithm>;
	/** An incremental SHA-512 hasher. */
	using sha512_hasher = hasher<sha512_algorithm>;
	/** An incremental SHA-512/224 hasher. */
	using sha512_224_hasher = hasher<sha512_224_algorithm>;
	/** An incremental SHA-512/256 hasher. */
	using sha512_256_hasher = hasher<sha512_256_algorithm>;

	/** Returns the SHA-224 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha224_digest sha224(byte_range auto&& input)
	{
		return detail::one_shot<sha224_algorithm>(input);
	}

	/** Returns the SHA-224 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha224_digest sha224(std::string_view input)
	{
		return detail::one_shot<sha224_algorithm>(input);
	}

	/** Returns the SHA-256 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha256_digest sha256(byte_range auto&& input)
	{
		return detail::one_shot<sha256_algorithm>(input);
	}

	/** Returns the SHA-256 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha256_digest sha256(std::string_view input)
	{
		return detail::one_shot<sha256_algorithm>(input);
	}

	/** Returns the SHA-384 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha384_digest sha384(byte_range auto&& input)
	{
		return detail::one_shot<sha384_algorithm>(input);
	}

	/** Returns the SHA-384 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha384_digest sha384(std::string_view input)
	{
		return detail::one_shot<sha384_algorithm>(input);
	}

	/** Returns the SHA-512 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha512_digest sha512(byte_range auto&& input)
	{
		return detail::one_shot<sha512_algorithm>(input);
	}

	/** Returns the SHA-512 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha512_digest sha512(std::string_view input)
	{
		return detail::one_shot<sha512_algorithm>(input);
	}

	/** Returns the SHA-512/224 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha512_224_digest sha512_224(byte_range auto&& input)
	{
		return detail::one_shot<sha512_224_algorithm>(input);
	}

	/** Returns the SHA-512/224 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha512_224_digest sha512_224(std::string_view input)
	{
		return detail::one_shot<sha512_224_algorithm>(input);
	}

	/** Returns the SHA-512/256 digest of @p input, any range of bytes. */
	[[nodiscard]] constexpr sha512_256_digest sha512_256(byte_range auto&& input)
	{
		return detail::one_shot<sha512_256_algorithm>(input);
	}

	/** Returns the SHA-512/256 digest of the characters of @p input, excluding any terminating NUL. */
	[[nodiscard]] constexpr sha512_256_digest sha512_256(std::string_view input)
	{
		return detail::one_shot<sha512_256_algorithm>(input);
	}

} // namespace alt

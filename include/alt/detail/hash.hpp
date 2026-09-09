#pragma once

#include <algorithm>
#include <alt/concepts.hpp>
#include <alt/detail/digest.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>

namespace alt
{
	namespace detail
	{

		/**
		 * @brief Counts the bytes fed to a hasher, in the width its length field needs.
		 *
		 * FIPS 180-4 bounds each algorithm's message length by the width of the length
		 * field appended during padding: 64 bits for SHA-1, SHA-224, and SHA-256, and
		 * 128 bits for the SHA-512 family. The counter is sized to match so that the
		 * standard, rather than this implementation, is always the binding limit.
		 *
		 * For the 64-bit field a single word already overshoots the standard's domain,
		 * which ends at 2^61 bytes, so no carry is tracked. For the 128-bit field a
		 * single word would wrap at 2^64 bytes, well inside a domain that extends to
		 * 2^125 bytes, so a second word carries.
		 *
		 * @tparam LengthSize Size of the trailing length field in bytes: 8 or 16.
		 */
		template<std::size_t LengthSize>
		class byte_counter
		{
			static_assert(LengthSize == 8 || LengthSize == 16,
			              "FIPS 180-4 defines only 64-bit and 128-bit length fields");

			std::uint64_t m_low{};
			std::uint64_t m_high{};

		public:
			/** Constructs a counter at zero. */
			constexpr byte_counter() noexcept = default;

			/**
			 * @brief Constructs a counter at an explicit value.
			 *
			 * @param low  Low 64 bits of the byte count.
			 * @param high High 64 bits. Ignored when @p LengthSize is 8.
			 */
			constexpr explicit byte_counter(std::uint64_t low, std::uint64_t high = 0) noexcept:
			  m_low(low), m_high(high)
			{}

			/** Adds @p bytes to the count, carrying into the high word where one exists. */
			constexpr byte_counter& operator+=(std::uint64_t bytes) noexcept
			{
				m_low += bytes;
				if constexpr(LengthSize == 16)
				{
					if(m_low < bytes) // wrapped
					{
						++m_high;
					}
				}
				return *this;
			}

			/** Returns the low 64 bits of the byte count. */
			[[nodiscard]] constexpr std::uint64_t low() const noexcept
			{
				return m_low;
			}

			/** Returns the high 64 bits of the byte count; always zero when @p LengthSize is 8. */
			[[nodiscard]] constexpr std::uint64_t high() const noexcept
			{
				return m_high;
			}

			friend constexpr bool operator==(const byte_counter&, const byte_counter&) noexcept = default;
		};

		/** Writes @p value big-endian into the 8 bytes of @p field starting at @p offset. */
		constexpr void write_be64(std::array<std::uint8_t, 8>& field, std::uint64_t value) noexcept
		{
			for(std::size_t i = 0; i < 8; ++i)
			{
				field[i] = static_cast<std::uint8_t>(value >> (56 - i * 8));
			}
		}

		/**
		 * @brief Encodes a byte count as the big-endian bit-length field appended by padding.
		 *
		 * The count is multiplied by eight and laid out most significant byte first, per
		 * FIPS 180-4 sections 5.1.1 and 5.1.2. A count beyond the standard's domain
		 * wraps rather than being detected; exceeding it is a documented precondition,
		 * unreachable by any message that can actually be hashed.
		 *
		 * @tparam LengthSize Size of the field in bytes: 8 or 16.
		 * @param  bytes      Number of message bytes consumed.
		 *
		 * @return The encoded length field.
		 */
		template<std::size_t LengthSize>
		[[nodiscard]] constexpr std::array<std::uint8_t, LengthSize>
		  encode_bit_length(byte_counter<LengthSize> bytes) noexcept
		{
			const std::uint64_t low_bits = bytes.low() << 3;

			std::array<std::uint8_t, LengthSize> field{};
			if constexpr(LengthSize == 8)
			{
				write_be64(field, low_bits);
			}
			else
			{
				// The low three bits of the high word come from the top of the low word.
				const std::uint64_t high_bits = (bytes.high() << 3) | (bytes.low() >> 61);

				std::array<std::uint8_t, 8> high_field{};
				std::array<std::uint8_t, 8> low_field{};
				write_be64(high_field, high_bits);
				write_be64(low_field, low_bits);

				for(std::size_t i = 0; i < 8; ++i)
				{
					field[i]     = high_field[i];
					field[8 + i] = low_field[i];
				}
			}
			return field;
		}

		/** Converts one byte-like element to a raw byte value. */
		template<typename T>
		constexpr std::uint8_t to_byte(T value) noexcept
		{
			if constexpr(std::same_as<std::remove_cv_t<T>, std::byte>)
			{
				return std::to_integer<std::uint8_t>(value);
			}
			else
			{
				return static_cast<std::uint8_t>(value);
			}
		}

		/** Reads a big-endian word of type @p Word beginning at @p offset in @p block. */
		template<typename Word>
		[[nodiscard]] constexpr Word read_be(std::span<const std::uint8_t> block, std::size_t offset) noexcept
		{
			Word value = 0;
			for(std::size_t i = 0; i < sizeof(Word); ++i)
			{
				value = static_cast<Word>((value << 8) | block[offset + i]);
			}
			return value;
		}

		/** Writes @p value big-endian into the @c sizeof(Word) bytes of @p out at @p offset. */
		template<typename Word>
		constexpr void write_be(std::span<std::uint8_t> out, std::size_t offset, Word value) noexcept
		{
			for(std::size_t i = 0; i < sizeof(Word); ++i)
			{
				out[offset + i] = static_cast<std::uint8_t>(value >> (8 * (sizeof(Word) - 1 - i)));
			}
		}

	} // namespace detail

	/**
	 * @brief Describes one iterated hash algorithm to @c alt::hasher.
	 *
	 * Every algorithm in FIPS 180-4 is the same construction over a different set of
	 * parameters, so buffering, padding, length counting, and truncation live in the
	 * engine and an algorithm supplies only what actually varies.
	 *
	 * @c length_size does double duty: it sizes the trailing length field appended
	 * during padding and selects the width of the engine's byte counter, so the two
	 * can never disagree.
	 */
	template<typename T>
	concept hash_algorithm =
	  requires(typename T::state_type& state, std::span<const std::uint8_t, T::block_size> block) {
		  typename T::word_type;
		  typename T::state_type;
		  { T::block_size } -> std::convertible_to<std::size_t>;
		  { T::length_size } -> std::convertible_to<std::size_t>;
		  { T::digest_size } -> std::convertible_to<std::size_t>;
		  { T::name } -> std::convertible_to<std::string_view>;
		  { T::initial_value } -> std::convertible_to<typename T::state_type>;
		  { T::compress(state, block) } noexcept;
	  } && detail::digest_traits<T>;

	/**
	 * @brief Incremental hasher for any algorithm satisfying @c hash_algorithm.
	 *
	 * Feed input with @c update(), which accepts any @c byte_range and may be called
	 * any number of times, then take the result with @c finish().
	 *
	 * @tparam Algorithm The algorithm to run.
	 */
	template<hash_algorithm Algorithm>
	class hasher
	{
	public:
		/** The digest type this hasher produces. */
		using digest_type = digest<Algorithm>;

		/** Bytes per compression block. */
		static constexpr std::size_t block_size = Algorithm::block_size;

		/** Constructs a hasher over the empty message. */
		constexpr hasher() noexcept = default;

		/**
		 * @brief Appends @p input to the message being hashed.
		 *
		 * A contiguous range is consumed in block-sized chunks; any other input range
		 * is consumed one element at a time, which lets a lazy view feed the hasher
		 * without being materialized first.
		 *
		 * @return @c *this, so calls can be chained.
		 */
		template<byte_range Range>
		constexpr hasher& update(Range&& input)
		{
			if constexpr(std::ranges::contiguous_range<Range> && std::ranges::sized_range<Range>)
			{
				auto        first     = std::ranges::begin(input);
				std::size_t remaining = std::ranges::size(input);
				m_count += remaining;

				while(remaining > 0)
				{
					const std::size_t take = std::min(remaining, block_size - m_buffered);
					for(std::size_t i = 0; i < take; ++i)
					{
						m_buffer[m_buffered + i] = detail::to_byte(first[static_cast<std::ptrdiff_t>(i)]);
					}
					m_buffered += take;
					first += static_cast<std::ptrdiff_t>(take);
					remaining -= take;

					if(m_buffered == block_size)
					{
						Algorithm::compress(m_state, std::span<const std::uint8_t, block_size>{m_buffer});
						m_buffered = 0;
					}
				}
			}
			else
			{
				for(const auto element: input)
				{
					m_buffer[m_buffered] = detail::to_byte(element);
					++m_buffered;
					m_count += 1;

					if(m_buffered == block_size)
					{
						Algorithm::compress(m_state, std::span<const std::uint8_t, block_size>{m_buffer});
						m_buffered = 0;
					}
				}
			}
			return *this;
		}

		/**
		 * @brief Returns the digest of everything fed so far.
		 *
		 * Padding is applied to a copy, so the hasher is left untouched and can keep
		 * accepting input afterwards. There is no finished state to guard against.
		 */
		[[nodiscard]] constexpr digest_type finish() const
		{
			constexpr std::size_t length_size = Algorithm::length_size;

			auto        state    = m_state;
			auto        buffer   = m_buffer;
			std::size_t buffered = m_buffered;

			// FIPS 180-4 section 5.1: a single 1 bit, zeros up to the length field, then
			// the message length in bits. Whole bytes throughout, so the 1 bit is 0x80.
			buffer[buffered] = 0x80;
			++buffered;

			if(buffered + length_size > block_size)
			{
				while(buffered < block_size)
				{
					buffer[buffered] = 0;
					++buffered;
				}
				Algorithm::compress(state, std::span<const std::uint8_t, block_size>{buffer});
				buffered = 0;
			}

			while(buffered < block_size - length_size)
			{
				buffer[buffered] = 0;
				++buffered;
			}

			const auto length_field = detail::encode_bit_length(m_count);
			for(std::size_t i = 0; i < length_size; ++i)
			{
				buffer[block_size - length_size + i] = length_field[i];
			}
			Algorithm::compress(state, std::span<const std::uint8_t, block_size>{buffer});

			return truncate(state);
		}

		/** Discards all input and returns the hasher to its initial state. */
		constexpr void reset() noexcept
		{
			*this = hasher{};
		}

	private:
		using word_type = typename Algorithm::word_type;

		static constexpr std::size_t state_words = std::tuple_size_v<typename Algorithm::state_type>;

		/** Serializes the state big-endian and keeps the leading @c digest_size bytes. */
		static constexpr digest_type truncate(const typename Algorithm::state_type& state) noexcept
		{
			std::array<std::uint8_t, state_words * sizeof(word_type)> full{};
			for(std::size_t i = 0; i < state_words; ++i)
			{
				detail::write_be<word_type>(full, i * sizeof(word_type), state[i]);
			}

			typename digest_type::array_type bytes{};
			for(std::size_t i = 0; i < Algorithm::digest_size; ++i)
			{
				bytes[i] = full[i];
			}
			return digest_type{bytes};
		}

		typename Algorithm::state_type              m_state{Algorithm::initial_value};
		std::array<std::uint8_t, block_size>        m_buffer{};
		std::size_t                                 m_buffered{};
		detail::byte_counter<Algorithm::length_size> m_count{};
	};

} // namespace alt

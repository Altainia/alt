#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace alt
{

	/** Reasons a base64 decode can reject its input, per RFC 4648 sections 3.3 and 3.5. */
	enum class base64_error
	{
		/** A character outside the alphabet appeared, which RFC 4648 section 3.3 requires be rejected. */
		invalid_character,
		/** The encoded length cannot describe any whole number of bytes. */
		invalid_length,
		/** A pad character appeared before the final quantum, or more padding than the tail allows. */
		unexpected_padding,
		/** The final quantum was short of the padding its alphabet requires. */
		missing_padding,
		/** The bits discarded from the final quantum were not zero, so the encoding is not canonical. */
		non_canonical_bits
	};

	/** Returns a short human-readable description of @p error. */
	[[nodiscard]] constexpr std::string_view base64_error_message(base64_error error) noexcept
	{
		switch(error)
		{
		case base64_error::invalid_character:
			return "character outside the base64 alphabet";
		case base64_error::invalid_length:
			return "encoded length cannot describe a whole number of bytes";
		case base64_error::unexpected_padding:
			return "pad character before the end of the encoded data";
		case base64_error::missing_padding:
			return "final quantum is missing its padding";
		case base64_error::non_canonical_bits:
			return "discarded bits of the final quantum were not zero";
		}
		return "unknown base64 error";
	}

	/** Thrown by a decode view parameterized on @c base64_throw_errors. */
	class base64_exception: public std::runtime_error
	{
		base64_error m_error;

	public:
		/** Constructs an exception reporting @p error. */
		explicit base64_exception(base64_error error):
		  std::runtime_error(std::string{base64_error_message(error)}), m_error(error)
		{}

		/** Returns the reason the decode failed. */
		[[nodiscard]] base64_error error() const noexcept
		{
			return m_error;
		}
	};

	/**
	 * @brief Error policy making a decode view yield @c std::expected elements.
	 *
	 * The view's element type becomes @c std::expected<std::byte, base64_error>. A
	 * malformed input produces one element holding the error, after which the range
	 * ends. Nothing is thrown, which is why this is the default.
	 */
	struct base64_expected_errors
	{};

	/**
	 * @brief Error policy making a decode view throw @c base64_exception.
	 *
	 * The view's element type stays @c std::byte, which keeps the range usable
	 * anywhere a plain byte range is expected, at the cost of reporting failure
	 * through an exception rather than a value.
	 */
	struct base64_throw_errors
	{};

	namespace detail
	{

		/** Input bytes consumed by one full encoding quantum, RFC 4648 section 4. */
		inline constexpr std::size_t base64_bytes_per_quantum = 3;

		/** Characters produced by one full encoding quantum, RFC 4648 section 4. */
		inline constexpr std::size_t base64_chars_per_quantum = 4;

		/** Bits of input carried by one encoded character. */
		inline constexpr std::size_t base64_bits_per_char = 6;

		/** Characters in a base64 alphabet, the "65-character subset" of RFC 4648 section 4 less the pad. */
		inline constexpr std::size_t base64_alphabet_size = 64;

		/** Value of @c padding meaning the alphabet emits and accepts no padding at all. */
		inline constexpr char base64_no_padding = '\0';

		/**
		 * @brief Constrains a type to a usable base64 alphabet.
		 *
		 * A model supplies a human-readable @c name, exactly 64 @c characters mapping
		 * sextet values 0 through 63 to output in order, and a @c padding character.
		 * Padding must not also be an alphabet character, or a decoder could not tell
		 * the end of the data from its content. Use @c base64_no_padding for an
		 * alphabet that omits padding, which RFC 4648 section 3.2 permits only where
		 * the length is known implicitly.
		 */
		template<typename T>
		concept base64_alphabet = requires {
			{ T::name } -> std::convertible_to<std::string_view>;
			{ T::characters } -> std::convertible_to<std::string_view>;
			{ T::padding } -> std::convertible_to<char>;
		} && (std::string_view{T::characters}.size() == base64_alphabet_size) && (T::padding == base64_no_padding || std::string_view{T::characters}.find(T::padding) == std::string_view::npos);

		/** True when @p Alphabet pads its output, RFC 4648 section 3.2. */
		template<base64_alphabet Alphabet>
		inline constexpr bool base64_pads = Alphabet::padding != base64_no_padding;

		/** Returns the character @p Alphabet assigns to sextet value @p sextet. */
		template<base64_alphabet Alphabet>
		constexpr char base64_char_of(std::uint32_t sextet) noexcept
		{
			return std::string_view{Alphabet::characters}[sextet];
		}

		/**
		 * @brief Encodes one quantum of @p count bytes packed into @p bits.
		 *
		 * @param bits  Up to three bytes, most significant byte first, zero-filled on
		 *              the right. RFC 4648 section 4 requires those fill bits be zero.
		 * @param count Bytes actually present, 1 through 3.
		 * @param out   Receives the characters produced.
		 *
		 * @return The number of characters written to @p out.
		 */
		template<base64_alphabet Alphabet>
		constexpr std::size_t base64_encode_quantum(std::uint32_t                               bits,
		                                            std::size_t                                 count,
		                                            std::array<char, base64_chars_per_quantum>& out) noexcept
		{
			// A quantum of n bytes fills n + 1 characters: 8n bits need ceil(8n / 6) sextets.
			const std::size_t produced = count + 1;
			for(std::size_t i = 0; i < produced; ++i)
			{
				const auto shift = static_cast<std::uint32_t>(18 - (i * base64_bits_per_char));
				out[i]           = base64_char_of<Alphabet>((bits >> shift) & 0x3Fu);
			}

			if constexpr(base64_pads<Alphabet>)
			{
				for(std::size_t i = produced; i < base64_chars_per_quantum; ++i)
				{
					out[i] = Alphabet::padding;
				}
				return base64_chars_per_quantum;
			}
			else
			{
				return produced;
			}
		}

		/**
		 * @brief Input iterator producing the base64 characters of an underlying byte range.
		 *
		 * Holds one quantum of output at a time, so encoding a range costs a fixed
		 * four characters of storage regardless of the input's length.
		 */
		template<std::ranges::input_range V, base64_alphabet Alphabet>
		class base64_encode_iterator
		{
			using base_iterator = std::ranges::iterator_t<V>;
			using base_sentinel = std::ranges::sentinel_t<V>;

			base_iterator                              m_current{};
			base_sentinel                              m_end{};
			std::array<char, base64_chars_per_quantum> m_buffer{};
			std::size_t                                m_size = 0;
			std::size_t                                m_pos  = 0;

			/** Consumes up to three input bytes and refills the character buffer. */
			constexpr void fill()
			{
				std::uint32_t bits  = 0;
				std::size_t   count = 0;
				while(count < base64_bytes_per_quantum && m_current != m_end)
				{
					const auto byte = static_cast<std::uint32_t>(static_cast<std::uint8_t>(*m_current));
					bits |= byte << (16 - (count * 8));
					++m_current;
					++count;
				}

				m_pos  = 0;
				m_size = count == 0 ? 0 : base64_encode_quantum<Alphabet>(bits, count, m_buffer);
			}

		public:
			using value_type       = char;
			using difference_type  = std::ptrdiff_t;
			using iterator_concept = std::input_iterator_tag;

			/** Constructs a past-the-end iterator. */
			base64_encode_iterator() = default;

			/** Constructs an iterator over [@p first, @p last) and encodes the first quantum. */
			constexpr base64_encode_iterator(base_iterator first, base_sentinel last):
			  m_current(std::move(first)), m_end(std::move(last))
			{
				fill();
			}

			/** Returns the current encoded character. */
			constexpr char operator*() const
			{
				return m_buffer[m_pos];
			}

			/** Advances to the next character, encoding another quantum as needed. */
			constexpr base64_encode_iterator& operator++()
			{
				if(++m_pos >= m_size)
				{
					fill();
				}
				return *this;
			}

			/** Advances past the current character, discarding the previous value. */
			constexpr void operator++(int)
			{
				++*this;
			}

			/** Compares against the end sentinel; true once input and buffer are exhausted. */
			constexpr bool operator==(std::default_sentinel_t /*sentinel*/) const
			{
				return m_size == 0;
			}
		};

		/**
		 * @brief A lazy view of the base64 encoding of an underlying range of bytes.
		 *
		 * @tparam V        The underlying view; its element type is a byte.
		 * @tparam Alphabet The alphabet whose characters and padding the view produces.
		 */
		template<std::ranges::input_range V, base64_alphabet Alphabet>
		  requires std::ranges::view<V>
		class base64_encode_view: public std::ranges::view_interface<base64_encode_view<V, Alphabet>>
		{
			V m_base{};

		public:
			base64_encode_view()
			  requires std::default_initializable<V>
			= default;

			/** Constructs the view over the given underlying view @p base. */
			constexpr explicit base64_encode_view(V base): m_base(std::move(base))
			{}

			/** Returns a copy of the underlying view. */
			[[nodiscard]] constexpr V base() const&
			  requires std::copy_constructible<V>
			{
				return m_base;
			}

			/** Returns the underlying view by move. */
			constexpr V base() &&
			{
				return std::move(m_base);
			}

			/** Returns an iterator to the first encoded character. */
			[[nodiscard]] constexpr auto begin()
			{
				return base64_encode_iterator<V, Alphabet>{std::ranges::begin(m_base), std::ranges::end(m_base)};
			}

			/** Returns the end sentinel. */
			[[nodiscard]] constexpr std::default_sentinel_t end() const noexcept
			{
				return std::default_sentinel;
			}
		};

		/** Satisfied by the error-reporting policies a decode view accepts. */
		template<typename T>
		concept base64_error_policy =
		  std::same_as<T, base64_expected_errors> || std::same_as<T, base64_throw_errors>;

		/** The element type a decode view yields under @p Policy. */
		template<base64_error_policy Policy>
		using base64_decode_element =
		  std::conditional_t<std::same_as<Policy, base64_throw_errors>, std::byte, std::expected<std::byte, base64_error>>;

		/** Sentinel in a reverse lookup table marking a character outside the alphabet. */
		inline constexpr std::int8_t base64_not_in_alphabet = -1;

		/**
		 * @brief Reverse lookup table for @p Alphabet, indexed by unsigned character value.
		 *
		 * Holds the sextet value of every alphabet character and
		 * @c base64_not_in_alphabet everywhere else, so rejecting the non-alphabet
		 * characters of RFC 4648 section 3.3 costs one indexed load.
		 */
		template<base64_alphabet Alphabet>
		inline constexpr auto base64_decode_table = [] {
			std::array<std::int8_t, 256> table{};
			table.fill(base64_not_in_alphabet);

			constexpr std::string_view characters = Alphabet::characters;
			for(std::size_t i = 0; i < characters.size(); ++i)
			{
				table[static_cast<unsigned char>(characters[i])] = static_cast<std::int8_t>(i);
			}
			return table;
		}();

		/**
		 * @brief Returns the bits of the final quantum that @p chars characters do not use.
		 *
		 * A quantum of two characters carries twelve bits but yields one byte, and one
		 * of three carries eighteen but yields two. RFC 4648 section 3.5 requires a
		 * conforming encoder to zero the leftover bits, and permits a decoder to reject
		 * an encoding where they are not, which is what this makes possible.
		 */
		constexpr std::uint32_t base64_unused_bit_mask(std::size_t chars) noexcept
		{
			const std::size_t carried  = chars * base64_bits_per_char;
			const std::size_t produced = (carried / 8) * 8;
			const std::size_t unused   = carried - produced;
			if(unused == 0)
			{
				return 0;
			}
			return static_cast<std::uint32_t>(((1u << unused) - 1u) << (24 - carried));
		}

		/**
		 * @brief Input iterator producing the bytes an underlying range of base64 characters encodes.
		 *
		 * @tparam Policy Selects how a malformed input is reported. See
		 *                @c base64_expected_errors and @c base64_throw_errors.
		 */
		template<std::ranges::input_range V, base64_alphabet Alphabet, base64_error_policy Policy>
		class base64_decode_iterator
		{
			using base_iterator = std::ranges::iterator_t<V>;
			using base_sentinel = std::ranges::sentinel_t<V>;

			static constexpr bool throws = std::same_as<Policy, base64_throw_errors>;

			base_iterator                                   m_current{};
			base_sentinel                                   m_end{};
			std::array<std::byte, base64_bytes_per_quantum> m_buffer{};
			std::size_t                                     m_size      = 0;
			std::size_t                                     m_pos       = 0;
			bool                                            m_exhausted = false;
			std::optional<base64_error>                     m_error;

			/**
			 * @brief Reads one quantum and writes the bytes it encodes to the buffer.
			 *
			 * @return The reason the quantum was rejected, or nothing on success.
			 */
			constexpr std::optional<base64_error> decode_quantum()
			{
				std::uint32_t bits  = 0;
				std::size_t   chars = 0;
				std::size_t   pads  = 0;

				for(std::size_t i = 0; i < base64_chars_per_quantum && m_current != m_end; ++i)
				{
					const char c = static_cast<char>(*m_current);
					++m_current;

					if constexpr(base64_pads<Alphabet>)
					{
						if(c == Alphabet::padding)
						{
							// Only the third and fourth positions can be padding: a quantum
							// always carries at least one byte, which needs two characters.
							if(i < 2)
							{
								return base64_error::unexpected_padding;
							}
							++pads;
							continue;
						}
					}

					if(pads != 0)
					{
						return base64_error::unexpected_padding;
					}

					const std::int8_t value = base64_decode_table<Alphabet>[static_cast<unsigned char>(c)];
					if(value == base64_not_in_alphabet)
					{
						return base64_error::invalid_character;
					}

					bits |= static_cast<std::uint32_t>(value) << (18 - (i * base64_bits_per_char));
					++chars;
				}

				m_pos  = 0;
				m_size = 0;
				if(chars == 0 && pads == 0)
				{
					m_exhausted = true;
					return std::nullopt;
				}

				if constexpr(base64_pads<Alphabet>)
				{
					// RFC 4648 section 3.2: the encoder must have completed the quantum.
					if(chars + pads != base64_chars_per_quantum)
					{
						return chars < 2 ? base64_error::invalid_length : base64_error::missing_padding;
					}
				}

				// A lone trailing character carries six bits, too few for even one byte.
				if(chars < 2)
				{
					return base64_error::invalid_length;
				}

				if((bits & base64_unused_bit_mask(chars)) != 0)
				{
					return base64_error::non_canonical_bits;
				}

				if(pads != 0 && m_current != m_end)
				{
					return base64_error::unexpected_padding;
				}

				m_size = chars - 1;
				for(std::size_t i = 0; i < m_size; ++i)
				{
					m_buffer[i] = static_cast<std::byte>((bits >> (16 - (i * 8))) & 0xFFu);
				}
				return std::nullopt;
			}

			/** Decodes the next quantum, reporting any failure the way @c Policy directs. */
			constexpr void fill()
			{
				if(m_exhausted)
				{
					m_size = 0;
					return;
				}

				const std::optional<base64_error> error = decode_quantum();
				if(!error.has_value())
				{
					return;
				}

				m_exhausted = true;
				if constexpr(throws)
				{
					throw base64_exception(*error);
				}
				else
				{
					// Surface the failure as the range's final element.
					m_error = error;
					m_pos   = 0;
					m_size  = 1;
				}
			}

		public:
			using value_type       = base64_decode_element<Policy>;
			using difference_type  = std::ptrdiff_t;
			using iterator_concept = std::input_iterator_tag;

			/** Constructs a past-the-end iterator. */
			base64_decode_iterator() = default;

			/** Constructs an iterator over [@p first, @p last) and decodes the first quantum. */
			constexpr base64_decode_iterator(base_iterator first, base_sentinel last):
			  m_current(std::move(first)), m_end(std::move(last))
			{
				fill();
			}

			/** Returns the current decoded byte, or under the expected policy the failure. */
			constexpr value_type operator*() const
			{
				if constexpr(throws)
				{
					return m_buffer[m_pos];
				}
				else
				{
					if(m_error.has_value())
					{
						return value_type{std::unexpect, *m_error};
					}
					return value_type{m_buffer[m_pos]};
				}
			}

			/** Advances to the next byte, decoding another quantum as needed. */
			constexpr base64_decode_iterator& operator++()
			{
				if(++m_pos >= m_size)
				{
					fill();
				}
				return *this;
			}

			/** Advances past the current byte, discarding the previous value. */
			constexpr void operator++(int)
			{
				++*this;
			}

			/** Compares against the end sentinel; true once input and buffer are exhausted. */
			constexpr bool operator==(std::default_sentinel_t /*sentinel*/) const
			{
				return m_size == 0;
			}
		};

		/**
		 * @brief A lazy view of the bytes an underlying range of base64 characters encodes.
		 *
		 * @tparam V        The underlying view; its element type is a character.
		 * @tparam Alphabet The alphabet the input is expected to use.
		 * @tparam Policy   How a malformed input is reported.
		 */
		template<std::ranges::input_range V, base64_alphabet Alphabet, base64_error_policy Policy>
		  requires std::ranges::view<V>
		class base64_decode_view: public std::ranges::view_interface<base64_decode_view<V, Alphabet, Policy>>
		{
			V m_base{};

		public:
			base64_decode_view()
			  requires std::default_initializable<V>
			= default;

			/** Constructs the view over the given underlying view @p base. */
			constexpr explicit base64_decode_view(V base): m_base(std::move(base))
			{}

			/** Returns a copy of the underlying view. */
			[[nodiscard]] constexpr V base() const&
			  requires std::copy_constructible<V>
			{
				return m_base;
			}

			/** Returns the underlying view by move. */
			constexpr V base() &&
			{
				return std::move(m_base);
			}

			/** Returns an iterator to the first decoded byte. */
			[[nodiscard]] constexpr auto begin()
			{
				return base64_decode_iterator<V, Alphabet, Policy>{std::ranges::begin(m_base), std::ranges::end(m_base)};
			}

			/** Returns the end sentinel. */
			[[nodiscard]] constexpr std::default_sentinel_t end() const noexcept
			{
				return std::default_sentinel;
			}
		};

	} // namespace detail

} // namespace alt

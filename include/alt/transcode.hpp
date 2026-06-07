#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <utility>

namespace alt
{
	namespace ranges
	{
		namespace views
		{
			namespace detail
			{

				/** True when @p C is a UTF-8 code unit type (@c char or @c char8_t). */
				template<typename C>
				inline constexpr bool is_utf8_unit = std::same_as<C, char> || std::same_as<C, char8_t>;

				/** True when @p C is the UTF-16 code unit type (@c char16_t). */
				template<typename C>
				inline constexpr bool is_utf16_unit = std::same_as<C, char16_t>;

				/** True when @p C is the UTF-32 code unit type (@c char32_t). */
				template<typename C>
				inline constexpr bool is_utf32_unit = std::same_as<C, char32_t>;

				/**
				 * @brief Constrains a type to a supported UTF code unit.
				 *
				 * Satisfied by @c char, @c char8_t (UTF-8), @c char16_t (UTF-16),
				 * and @c char32_t (UTF-32). @c wchar_t is intentionally excluded.
				 */
				template<typename C>
				concept code_unit = is_utf8_unit<C> || is_utf16_unit<C> || is_utf32_unit<C>;

				/** The Unicode REPLACEMENT CHARACTER, substituted for ill-formed input. */
				inline constexpr char32_t replacement_character = 0xFFFD;

				/** Maximum number of @p C code units a single code point can require. */
				template<code_unit C>
				inline constexpr std::size_t max_units = 4 / sizeof(C); // 4 (UTF-8), 2 (UTF-16), 1 (UTF-32)

				/** Fixed-capacity output buffer for the code units of one code point. */
				template<code_unit C>
				using unit_buffer = std::array<C, max_units<C>>;

				/**
				 * @brief Returns @p value byte-swapped iff @p order differs from native.
				 *
				 * A no-op for single-byte code units, for which byte order is meaningless.
				 */
				template<code_unit C>
				constexpr C order_unit(C value, std::endian order) noexcept
				{
					if constexpr(sizeof(C) == 1)
					{
						return value;
					}
					else
					{
						return order == std::endian::native ? value : static_cast<C>(std::byteswap(value));
					}
				}

				/**
				 * @brief Encodes one Unicode scalar @p cp into @p out in the target encoding.
				 *
				 * @tparam TargetChar Target code unit type (selects UTF-8/16/32).
				 * @tparam TargetEndian Byte order applied to each produced multi-byte unit.
				 *
				 * @param cp  A valid Unicode scalar value (<= U+10FFFF, not a surrogate). The
				 *            caller (the decoder) guarantees validity, so encoding never fails.
				 * @param out Buffer receiving the produced code units.
				 *
				 * @return The number of code units written to @p out.
				 */
				template<code_unit TargetChar, std::endian TargetEndian>
				constexpr std::size_t encode_one(char32_t cp, unit_buffer<TargetChar>& out) noexcept
				{
					if constexpr(is_utf8_unit<TargetChar>)
					{
						if(cp <= 0x7F)
						{
							out[0] = static_cast<TargetChar>(cp);
							return 1;
						}
						if(cp <= 0x7FF)
						{
							out[0] = static_cast<TargetChar>(0xC0 | (cp >> 6));
							out[1] = static_cast<TargetChar>(0x80 | (cp & 0x3F));
							return 2;
						}
						if(cp <= 0xFFFF)
						{
							out[0] = static_cast<TargetChar>(0xE0 | (cp >> 12));
							out[1] = static_cast<TargetChar>(0x80 | ((cp >> 6) & 0x3F));
							out[2] = static_cast<TargetChar>(0x80 | (cp & 0x3F));
							return 3;
						}
						out[0] = static_cast<TargetChar>(0xF0 | (cp >> 18));
						out[1] = static_cast<TargetChar>(0x80 | ((cp >> 12) & 0x3F));
						out[2] = static_cast<TargetChar>(0x80 | ((cp >> 6) & 0x3F));
						out[3] = static_cast<TargetChar>(0x80 | (cp & 0x3F));
						return 4;
					}
					else if constexpr(is_utf16_unit<TargetChar>)
					{
						if(cp <= 0xFFFF)
						{
							out[0] = order_unit<TargetChar>(static_cast<TargetChar>(cp), TargetEndian);
							return 1;
						}
						const char32_t v  = cp - 0x10000;
						const auto     hi = static_cast<TargetChar>(0xD800 + (v >> 10));
						const auto     lo = static_cast<TargetChar>(0xDC00 + (v & 0x3FF));
						out[0]            = order_unit<TargetChar>(hi, TargetEndian);
						out[1]            = order_unit<TargetChar>(lo, TargetEndian);
						return 2;
					}
					else
					{
						out[0] = order_unit<TargetChar>(static_cast<TargetChar>(cp), TargetEndian);
						return 1;
					}
				}

				/**
				 * @brief Decodes one scalar from a UTF-8 sequence in [@p current, @p end).
				 *
				 * Advances @p current past the consumed bytes. Ill-formed input yields
				 * @c replacement_character, consuming the maximal valid subpart only (an
				 * invalid continuation byte is left unconsumed so it can begin a new sequence),
				 * per the Unicode-recommended substitution practice.
				 *
				 * @pre @p current != @p end.
				 */
				template<std::input_iterator It, std::sentinel_for<It> Sent>
				constexpr char32_t decode_one_utf8(It& current, Sent end)
				{
					const auto b0 = static_cast<unsigned char>(*current);
					++current;
					if(b0 <= 0x7F)
					{
						return b0;
					}

					unsigned      len = 0;
					char32_t      cp  = 0;
					unsigned char lo2 = 0x80; // valid range of the first continuation byte
					unsigned char hi2 = 0xBF;
					if(b0 >= 0xC2 && b0 <= 0xDF)
					{
						len = 2;
						cp  = b0 & 0x1F;
					}
					else if(b0 >= 0xE0 && b0 <= 0xEF)
					{
						len = 3;
						cp  = b0 & 0x0F;
						if(b0 == 0xE0)
						{
							lo2 = 0xA0;
						}
						else if(b0 == 0xED)
						{
							hi2 = 0x9F;
						}
					}
					else if(b0 >= 0xF0 && b0 <= 0xF4)
					{
						len = 4;
						cp  = b0 & 0x07;
						if(b0 == 0xF0)
						{
							lo2 = 0x90;
						}
						else if(b0 == 0xF4)
						{
							hi2 = 0x8F;
						}
					}
					else
					{
						return replacement_character; // 0x80-0xC1, 0xF5-0xFF: invalid lead
					}

					if(current == end)
					{
						return replacement_character;
					}
					auto b = static_cast<unsigned char>(*current);
					if(b < lo2 || b > hi2)
					{
						return replacement_character; // do not consume: may start a new sequence
					}
					cp = (cp << 6) | (b & 0x3F);
					++current;

					for(unsigned i = 2; i < len; ++i)
					{
						if(current == end)
						{
							return replacement_character;
						}
						b = static_cast<unsigned char>(*current);
						if(b < 0x80 || b > 0xBF)
						{
							return replacement_character; // do not consume
						}
						cp = (cp << 6) | (b & 0x3F);
						++current;
					}
					return cp;
				}

				/**
				 * @brief Decodes one scalar from a UTF-16 sequence in [@p current, @p end).
				 *
				 * Reads each unit in @p SourceEndian byte order. Unpaired surrogates yield
				 * @c replacement_character; a high surrogate not followed by a low surrogate
				 * leaves the following unit unconsumed.
				 *
				 * @pre @p current != @p end.
				 */
				template<std::endian SourceEndian, std::input_iterator It, std::sentinel_for<It> Sent>
				constexpr char32_t decode_one_utf16(It& current, Sent end)
				{
					const auto w0 = order_unit<char16_t>(static_cast<char16_t>(*current), SourceEndian);
					++current;
					if(w0 < 0xD800 || w0 > 0xDFFF)
					{
						return w0;
					}
					if(w0 >= 0xDC00)
					{
						return replacement_character; // lone low surrogate
					}
					if(current == end)
					{
						return replacement_character; // high surrogate at end of input
					}
					const auto w1 = order_unit<char16_t>(static_cast<char16_t>(*current), SourceEndian);
					if(w1 < 0xDC00 || w1 > 0xDFFF)
					{
						return replacement_character; // do not consume the mismatched unit
					}
					++current;
					return 0x10000 + ((static_cast<char32_t>(w0 - 0xD800) << 10) | static_cast<char32_t>(w1 - 0xDC00));
				}

				/**
				 * @brief Decodes one scalar from a UTF-32 unit in [@p current, @p end).
				 *
				 * Reads the unit in @p SourceEndian byte order. Values above U+10FFFF or in the
				 * surrogate range yield @c replacement_character.
				 *
				 * @pre @p current != @p end.
				 */
				template<std::endian SourceEndian, std::input_iterator It, std::sentinel_for<It> Sent>
				constexpr char32_t decode_one_utf32(It& current, Sent end)
				{
					(void)end;
					const auto cp = order_unit<char32_t>(static_cast<char32_t>(*current), SourceEndian);
					++current;
					if(cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
					{
						return replacement_character;
					}
					return cp;
				}

				/**
				 * @brief Decodes one Unicode scalar from [@p current, @p end), dispatching on
				 *        the source code unit type, and advances @p current.
				 *
				 * @tparam SourceEndian Byte order of the source code units (ignored for UTF-8).
				 *
				 * @pre @p current != @p end.
				 * @return The decoded scalar, or @c replacement_character for ill-formed input.
				 */
				template<std::endian SourceEndian, std::input_iterator It, std::sentinel_for<It> Sent>
				  requires code_unit<std::iter_value_t<It>>
				constexpr char32_t decode_one(It& current, Sent end)
				{
					using unit = std::iter_value_t<It>;
					if constexpr(is_utf8_unit<unit>)
					{
						return decode_one_utf8(current, end);
					}
					else if constexpr(is_utf16_unit<unit>)
					{
						return decode_one_utf16<SourceEndian>(current, end);
					}
					else
					{
						return decode_one_utf32<SourceEndian>(current, end);
					}
				}

			} // namespace detail
		} // namespace views
	} // namespace ranges
} // namespace alt

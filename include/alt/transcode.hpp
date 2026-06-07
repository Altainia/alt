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
						const char32_t   v  = cp - 0x10000;
						const TargetChar hi = static_cast<TargetChar>(0xD800 + (v >> 10));
						const TargetChar lo = static_cast<TargetChar>(0xDC00 + (v & 0x3FF));
						out[0]              = order_unit<TargetChar>(hi, TargetEndian);
						out[1]              = order_unit<TargetChar>(lo, TargetEndian);
						return 2;
					}
					else
					{
						out[0] = order_unit<TargetChar>(static_cast<TargetChar>(cp), TargetEndian);
						return 1;
					}
				}

			} // namespace detail
		} // namespace views
	} // namespace ranges
} // namespace alt

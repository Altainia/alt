# alt::transcode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A lazy, range-composable UTF transcoding view (`alt::transcode`) that converts between UTF-8/16/32 and byte orders on demand, replacing ill-formed input with U+FFFD.

**Architecture:** A header-only view adaptor. Internal `detail` codec functions (`decode_one`, `encode_one`, `order_unit`) do the per-code-point work; a single-pass input iterator pulls one code point from the source, encodes it into a small fixed buffer, and yields target code units one at a time. `transcode<...>()` returns a pipeable closure whose `friend operator|` builds a `transcode_view`. Everything is `constexpr`.

**Tech Stack:** C++23, GoogleTest, CMake. Uses `std::byteswap`, `std::ranges`, `std::views::all`, `std::ranges::to` (tests). Compiler: GCC 13+ / Clang 17+.

**Design reference:** `docs/superpowers/specs/2026-06-07-transcode-design.md`. User-facing doc already written at `docs/transcode.md`.

---

## File Structure

- **Create** `include/alt/transcode.hpp` — the entire component (detail codecs, iterator, view, closure, `transcode` functions, namespace wiring). Header-only.
- **Create** `tests/test_transcode.cpp` — GoogleTest suite.
- **Modify** `tests/CMakeLists.txt` — add `test_transcode.cpp` to the `alt_tests` source list.

Conventions to follow throughout: tabs for indentation; `snake_case`; `m_` member prefix; Doxygen `/** */` on every public symbol; American spelling; `const` by default.

---

### Task 1: Header scaffold — traits, concept, `order_unit`

**Files:**
- Create: `include/alt/transcode.hpp`
- Create: `tests/test_transcode.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Register the test executable source**

In `tests/CMakeLists.txt`, add `test_transcode.cpp` to the `add_executable(alt_tests ...)` list (alphabetical-ish placement is fine), e.g. after `test_clearing_string.cpp`:

```cmake
add_executable(alt_tests
    test_algorithm.cpp
    test_version.cpp
    test_flags.cpp
    test_functional.cpp
    test_type_traits.cpp
    test_utility.cpp
    test_scope.cpp
    test_memory.cpp
    test_clearing_string.cpp
    test_transcode.cpp
)
```

- [ ] **Step 2: Write the header scaffold (traits, concept, helpers)**

Create `include/alt/transcode.hpp`:

```cpp
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
					if constexpr (sizeof(C) == 1)
					{
						return value;
					}
					else
					{
						return order == std::endian::native ? value : static_cast<C>(std::byteswap(value));
					}
				}

			} // namespace detail
		} // namespace views
	} // namespace ranges
} // namespace alt
```

- [ ] **Step 3: Write the failing test for the scaffold**

Create `tests/test_transcode.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/transcode.hpp>

#include <bit>

namespace detail = alt::ranges::views::detail;

TEST(TranscodeDetail, CodeUnitConcept)
{
	static_assert(detail::code_unit<char>);
	static_assert(detail::code_unit<char8_t>);
	static_assert(detail::code_unit<char16_t>);
	static_assert(detail::code_unit<char32_t>);
	static_assert(!detail::code_unit<wchar_t>);
	static_assert(!detail::code_unit<int>);
	SUCCEED();
}

TEST(TranscodeDetail, MaxUnits)
{
	static_assert(detail::max_units<char8_t> == 4);
	static_assert(detail::max_units<char16_t> == 2);
	static_assert(detail::max_units<char32_t> == 1);
	SUCCEED();
}

TEST(TranscodeDetail, OrderUnitByteSwaps)
{
	// 8-bit: always identity.
	EXPECT_EQ(detail::order_unit<char8_t>(char8_t{0x41}, std::endian::big), char8_t{0x41});

	// Native order: identity.
	EXPECT_EQ(detail::order_unit<char32_t>(char32_t{0x41}, std::endian::native), char32_t{0x41});

	// Non-native order: byte-swapped.
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ(detail::order_unit<char32_t>(char32_t{0x41}, other), std::byteswap(char32_t{0x41}));
	EXPECT_EQ(detail::order_unit<char16_t>(char16_t{0x41}, other), std::byteswap(char16_t{0x41}));
}
```

- [ ] **Step 4: Configure and build, verify tests pass**

Run: `cmake --preset debug && cmake --build --preset debug --target alt_tests`
Then: `ctest --preset debug --output-on-failure -R Transcode`
Expected: `TranscodeDetail.*` tests PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add include/alt/transcode.hpp tests/test_transcode.cpp tests/CMakeLists.txt
git commit -m "Add transcode scaffold: code_unit concept and order_unit helper"
```

---

### Task 2: `encode_one`

**Files:**
- Modify: `include/alt/transcode.hpp` (add `encode_one` inside `detail`)
- Test: `tests/test_transcode.cpp`

- [ ] **Step 1: Write the failing tests for encoding**

Add to `tests/test_transcode.cpp`:

```cpp
TEST(TranscodeDetail, EncodeUtf8)
{
	detail::unit_buffer<char8_t> buf{};
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], char8_t{0x41});

	// U+00E9 (é) -> C3 A9
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'é', buf)), 2u);
	EXPECT_EQ(buf[0], char8_t{0xC3});
	EXPECT_EQ(buf[1], char8_t{0xA9});

	// U+20AC (€) -> E2 82 AC
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'€', buf)), 3u);
	EXPECT_EQ(buf[0], char8_t{0xE2});
	EXPECT_EQ(buf[1], char8_t{0x82});
	EXPECT_EQ(buf[2], char8_t{0xAC});

	// U+1F600 (😀) -> F0 9F 98 80
	EXPECT_EQ((detail::encode_one<char8_t, std::endian::native>(U'\U0001F600', buf)), 4u);
	EXPECT_EQ(buf[0], char8_t{0xF0});
	EXPECT_EQ(buf[1], char8_t{0x9F});
	EXPECT_EQ(buf[2], char8_t{0x98});
	EXPECT_EQ(buf[3], char8_t{0x80});
}

TEST(TranscodeDetail, EncodeUtf16)
{
	detail::unit_buffer<char16_t> buf{};
	// BMP
	EXPECT_EQ((detail::encode_one<char16_t, std::endian::native>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], char16_t{0x0041});

	// Supplementary: U+1F600 -> D83D DE00
	EXPECT_EQ((detail::encode_one<char16_t, std::endian::native>(U'\U0001F600', buf)), 2u);
	EXPECT_EQ(buf[0], char16_t{0xD83D});
	EXPECT_EQ(buf[1], char16_t{0xDE00});

	// Big-endian output byte-swaps each unit.
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ((detail::encode_one<char16_t, other>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], std::byteswap(char16_t{0x0041}));
}

TEST(TranscodeDetail, EncodeUtf32)
{
	detail::unit_buffer<char32_t> buf{};
	EXPECT_EQ((detail::encode_one<char32_t, std::endian::native>(U'\U0001F600', buf)), 1u);
	EXPECT_EQ(buf[0], char32_t{0x1F600});

	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	EXPECT_EQ((detail::encode_one<char32_t, other>(U'A', buf)), 1u);
	EXPECT_EQ(buf[0], std::byteswap(char32_t{0x41}));
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset debug --target alt_tests`
Expected: FAIL to compile — `encode_one` is not declared.

- [ ] **Step 3: Implement `encode_one`**

Add inside `namespace detail` (after `order_unit`) in `include/alt/transcode.hpp`:

```cpp
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
	if constexpr (is_utf8_unit<TargetChar>)
	{
		if (cp <= 0x7F)
		{
			out[0] = static_cast<TargetChar>(cp);
			return 1;
		}
		if (cp <= 0x7FF)
		{
			out[0] = static_cast<TargetChar>(0xC0 | (cp >> 6));
			out[1] = static_cast<TargetChar>(0x80 | (cp & 0x3F));
			return 2;
		}
		if (cp <= 0xFFFF)
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
	else if constexpr (is_utf16_unit<TargetChar>)
	{
		if (cp <= 0xFFFF)
		{
			out[0] = order_unit<TargetChar>(static_cast<TargetChar>(cp), TargetEndian);
			return 1;
		}
		const char32_t    v  = cp - 0x10000;
		const TargetChar  hi = static_cast<TargetChar>(0xD800 + (v >> 10));
		const TargetChar  lo = static_cast<TargetChar>(0xDC00 + (v & 0x3FF));
		out[0] = order_unit<TargetChar>(hi, TargetEndian);
		out[1] = order_unit<TargetChar>(lo, TargetEndian);
		return 2;
	}
	else
	{
		out[0] = order_unit<TargetChar>(static_cast<TargetChar>(cp), TargetEndian);
		return 1;
	}
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build --preset debug --target alt_tests && ctest --preset debug --output-on-failure -R TranscodeDetail`
Expected: PASS (all `TranscodeDetail.Encode*`).

- [ ] **Step 5: Commit**

```bash
git add include/alt/transcode.hpp tests/test_transcode.cpp
git commit -m "Add transcode encode_one for UTF-8/16/32 with endianness"
```

---

### Task 3: `decode_one`

**Files:**
- Modify: `include/alt/transcode.hpp` (add `decode_one_utf8/16/32` and `decode_one`)
- Test: `tests/test_transcode.cpp`

- [ ] **Step 1: Write the failing tests for decoding**

Add to `tests/test_transcode.cpp`. Helper decodes one code point from a container of code units:

```cpp
template<std::endian SrcE = std::endian::native, typename R>
char32_t decode_first(const R& units)
{
	auto first = std::ranges::begin(units);
	auto last  = std::ranges::end(units);
	return detail::decode_one<SrcE>(first, last);
}

TEST(TranscodeDetail, DecodeUtf8Valid)
{
	EXPECT_EQ(decode_first(std::u8string{u8"A"}), U'A');
	EXPECT_EQ(decode_first(std::u8string{u8"é"}), U'é');
	EXPECT_EQ(decode_first(std::u8string{u8"€"}), U'€');
	EXPECT_EQ(decode_first(std::u8string{u8"\U0001F600"}), U'\U0001F600');
}

TEST(TranscodeDetail, DecodeUtf8Invalid)
{
	// Lone continuation byte.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0x80}), detail::replacement_character);
	// Invalid lead byte.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xFF}), detail::replacement_character);
	// Truncated 2-byte sequence (lead with no continuation).
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xC3}), detail::replacement_character);
	// Overlong encoding of U+0000 (C0 80).
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xC0, 0x80}), detail::replacement_character);
	// Surrogate U+D800 encoded as UTF-8 (ED A0 80) -> rejected.
	EXPECT_EQ(decode_first(std::vector<char8_t>{0xED, 0xA0, 0x80}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeUtf16Valid)
{
	EXPECT_EQ(decode_first(std::u16string{u"A"}), U'A');
	EXPECT_EQ(decode_first(std::u16string{u"\U0001F600"}), U'\U0001F600'); // surrogate pair
}

TEST(TranscodeDetail, DecodeUtf16Invalid)
{
	// Lone high surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xD83D}), detail::replacement_character);
	// Lone low surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xDE00}), detail::replacement_character);
	// High surrogate followed by non-low-surrogate.
	EXPECT_EQ(decode_first(std::vector<char16_t>{0xD83D, 0x0041}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeUtf32Invalid)
{
	// Beyond U+10FFFF.
	EXPECT_EQ(decode_first(std::vector<char32_t>{0x110000}), detail::replacement_character);
	// Surrogate scalar.
	EXPECT_EQ(decode_first(std::vector<char32_t>{0xD800}), detail::replacement_character);
}

TEST(TranscodeDetail, DecodeRespectsSourceEndian)
{
	constexpr std::endian other =
	  std::endian::native == std::endian::little ? std::endian::big : std::endian::little;
	// A big-endian-stored 'A' decodes back to U'A'.
	std::vector<char32_t> swapped{std::byteswap(char32_t{0x41})};
	EXPECT_EQ(decode_first<other>(swapped), U'A');
}
```

Add `#include <string>` and `#include <vector>` near the top of the test file if not already present.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset debug --target alt_tests`
Expected: FAIL to compile — `decode_one` is not declared.

- [ ] **Step 3: Implement the decoders**

Add inside `namespace detail` (after `encode_one`) in `include/alt/transcode.hpp`:

```cpp
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
	if (b0 <= 0x7F)
	{
		return b0;
	}

	unsigned      len  = 0;
	char32_t      cp   = 0;
	unsigned char lo2  = 0x80; // valid range of the first continuation byte
	unsigned char hi2  = 0xBF;
	if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; cp = b0 & 0x1F; }
	else if (b0 == 0xE0)               { len = 3; cp = b0 & 0x0F; lo2 = 0xA0; }
	else if (b0 >= 0xE1 && b0 <= 0xEC) { len = 3; cp = b0 & 0x0F; }
	else if (b0 == 0xED)               { len = 3; cp = b0 & 0x0F; hi2 = 0x9F; }
	else if (b0 >= 0xEE && b0 <= 0xEF) { len = 3; cp = b0 & 0x0F; }
	else if (b0 == 0xF0)               { len = 4; cp = b0 & 0x07; lo2 = 0x90; }
	else if (b0 >= 0xF1 && b0 <= 0xF3) { len = 4; cp = b0 & 0x07; }
	else if (b0 == 0xF4)               { len = 4; cp = b0 & 0x07; hi2 = 0x8F; }
	else { return replacement_character; } // 0x80-0xC1, 0xF5-0xFF: invalid lead

	if (current == end)
	{
		return replacement_character;
	}
	unsigned char b = static_cast<unsigned char>(*current);
	if (b < lo2 || b > hi2)
	{
		return replacement_character; // do not consume: may start a new sequence
	}
	cp = (cp << 6) | (b & 0x3F);
	++current;

	for (unsigned i = 2; i < len; ++i)
	{
		if (current == end)
		{
			return replacement_character;
		}
		b = static_cast<unsigned char>(*current);
		if (b < 0x80 || b > 0xBF)
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
	const char16_t w0 = order_unit<char16_t>(static_cast<char16_t>(*current), SourceEndian);
	++current;
	if (w0 < 0xD800 || w0 > 0xDFFF)
	{
		return w0;
	}
	if (w0 >= 0xDC00)
	{
		return replacement_character; // lone low surrogate
	}
	if (current == end)
	{
		return replacement_character; // high surrogate at end of input
	}
	const char16_t w1 = order_unit<char16_t>(static_cast<char16_t>(*current), SourceEndian);
	if (w1 < 0xDC00 || w1 > 0xDFFF)
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
	const char32_t cp = order_unit<char32_t>(static_cast<char32_t>(*current), SourceEndian);
	++current;
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
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
	if constexpr (is_utf8_unit<unit>)
	{
		return decode_one_utf8(current, end);
	}
	else if constexpr (is_utf16_unit<unit>)
	{
		return decode_one_utf16<SourceEndian>(current, end);
	}
	else
	{
		return decode_one_utf32<SourceEndian>(current, end);
	}
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build --preset debug --target alt_tests && ctest --preset debug --output-on-failure -R TranscodeDetail`
Expected: PASS (all decode tests).

- [ ] **Step 5: Commit**

```bash
git add include/alt/transcode.hpp tests/test_transcode.cpp
git commit -m "Add transcode decode_one for UTF-8/16/32 with U+FFFD substitution"
```

---

### Task 4: View, iterator, closure, and `transcode` functions

**Files:**
- Modify: `include/alt/transcode.hpp` (add iterator, view, closure, `transcode`, namespace re-exports)
- Test: `tests/test_transcode.cpp`

- [ ] **Step 1: Write the failing test for the public pipe API**

Add to `tests/test_transcode.cpp` (add `#include <ranges>` if not present):

```cpp
TEST(Transcode, Utf8ToUtf32Native)
{
	std::string s1 = "Hello world";
	auto s2 = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	static_assert(std::same_as<decltype(s2), std::u32string>);
	EXPECT_EQ(s2, U"Hello world");
}

TEST(Transcode, NamespaceAliasesNameSameThing)
{
	std::string s1 = "hi";
	auto a = s1 | alt::transcode<char32_t>()                | std::ranges::to<std::basic_string>();
	auto b = s1 | alt::views::transcode<char32_t>()         | std::ranges::to<std::basic_string>();
	auto c = s1 | alt::ranges::transcode<char32_t>()        | std::ranges::to<std::basic_string>();
	auto d = s1 | alt::ranges::views::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(a, b);
	EXPECT_EQ(a, c);
	EXPECT_EQ(a, d);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset debug --target alt_tests`
Expected: FAIL to compile — `alt::transcode` is not declared.

- [ ] **Step 3: Implement iterator, view, closure, and functions**

In `include/alt/transcode.hpp`, add the following **after the `detail` namespace closes but still inside `namespace views`** (i.e. between `} // namespace detail` and `} // namespace views`):

```cpp
/**
 * @brief Single-pass iterator that lazily transcodes one source range into
 *        target code units.
 *
 * Decodes one code point from the source, encodes it into an internal buffer,
 * and yields the resulting code units one at a time. Models @c input_iterator.
 */
template<std::ranges::input_range V, detail::code_unit TargetChar, std::endian SourceEndian, std::endian TargetEndian>
class transcode_iterator
{
	using base_iterator = std::ranges::iterator_t<V>;
	using base_sentinel = std::ranges::sentinel_t<V>;

	base_iterator                     m_current{};
	base_sentinel                     m_end{};
	detail::unit_buffer<TargetChar>   m_buffer{};
	std::size_t                       m_size = 0; // valid units in buffer; 0 => exhausted
	std::size_t                       m_pos  = 0; // index of the current unit

	constexpr void fill()
	{
		if (m_current == m_end)
		{
			m_size = 0;
			return;
		}
		const char32_t cp = detail::decode_one<SourceEndian>(m_current, m_end);
		m_size            = detail::encode_one<TargetChar, TargetEndian>(cp, m_buffer);
		m_pos             = 0;
	}

public:
	using value_type      = TargetChar;
	using difference_type = std::ptrdiff_t;
	using iterator_concept = std::input_iterator_tag;

	transcode_iterator() = default;

	constexpr transcode_iterator(base_iterator first, base_sentinel last)
	  : m_current(std::move(first)), m_end(std::move(last))
	{
		fill();
	}

	constexpr TargetChar operator*() const { return m_buffer[m_pos]; }

	constexpr transcode_iterator& operator++()
	{
		if (++m_pos >= m_size)
		{
			fill();
		}
		return *this;
	}

	constexpr void operator++(int) { ++*this; }

	constexpr bool operator==(std::default_sentinel_t) const { return m_size == 0; }
};

/**
 * @brief A lazy view that transcodes its underlying range of code units into
 *        @p TargetChar code units in @p TargetEndian byte order.
 *
 * @tparam V            The underlying view; its element type is the source encoding.
 * @tparam TargetChar   Target code unit type.
 * @tparam SourceEndian Byte order of the source units (ignored for UTF-8).
 * @tparam TargetEndian Byte order of the produced units (ignored for UTF-8).
 */
template<std::ranges::input_range V, detail::code_unit TargetChar, std::endian SourceEndian, std::endian TargetEndian>
	requires std::ranges::view<V> && detail::code_unit<std::ranges::range_value_t<V>>
class transcode_view : public std::ranges::view_interface<transcode_view<V, TargetChar, SourceEndian, TargetEndian>>
{
	V m_base{};

public:
	transcode_view()
		requires std::default_initializable<V>
	= default;

	constexpr explicit transcode_view(V base) : m_base(std::move(base)) {}

	/** Returns a copy of the underlying view. */
	constexpr V base() const&
		requires std::copy_constructible<V>
	{
		return m_base;
	}

	/** Returns the underlying view by move. */
	constexpr V base() && { return std::move(m_base); }

	/** Returns an iterator to the first transcoded code unit. */
	constexpr auto begin()
	{
		return transcode_iterator<V, TargetChar, SourceEndian, TargetEndian>{
			std::ranges::begin(m_base), std::ranges::end(m_base)};
	}

	/** Returns the end sentinel. */
	constexpr std::default_sentinel_t end() const noexcept { return std::default_sentinel; }
};

/**
 * @brief Pipeable range-adaptor closure produced by @c transcode().
 *
 * Applying it to a viewable range yields a @c transcode_view.
 */
template<detail::code_unit TargetChar, std::endian SourceEndian, std::endian TargetEndian>
struct transcode_closure
{
	/** Builds the transcoding view over @p r. */
	template<std::ranges::viewable_range R>
		requires detail::code_unit<std::ranges::range_value_t<R>>
	constexpr auto operator()(R&& r) const
	{
		return transcode_view<std::views::all_t<R>, TargetChar, SourceEndian, TargetEndian>{
			std::views::all(std::forward<R>(r))};
	}

	/** Enables `range | transcode<...>()` pipe syntax. */
	template<std::ranges::viewable_range R>
		requires detail::code_unit<std::ranges::range_value_t<R>>
	friend constexpr auto operator|(R&& r, const transcode_closure& closure)
	{
		return closure(std::forward<R>(r));
	}
};

/**
 * @brief Creates a transcoding adaptor targeting @p TargetChar.
 *
 * Source byte order defaults to native. Use this overload for
 * `transcode<Target>()` and `transcode<Target, OutEndian>()`.
 *
 * @tparam TargetChar   Target code unit type (@c char, @c char8_t, @c char16_t, @c char32_t).
 * @tparam TargetEndian Byte order of the produced code units. Defaults to native.
 */
template<detail::code_unit TargetChar, std::endian TargetEndian = std::endian::native>
constexpr auto transcode()
{
	return transcode_closure<TargetChar, std::endian::native, TargetEndian>{};
}

/**
 * @brief Creates a transcoding adaptor that reads the source in @p SourceEndian
 *        byte order and targets @p TargetChar.
 *
 * Use this overload for `transcode<InEndian, Target>()` and
 * `transcode<InEndian, Target, OutEndian>()`. The leading @c std::endian
 * argument selects this overload over the type-leading one.
 *
 * @tparam SourceEndian Byte order of the source code units (ignored for UTF-8).
 * @tparam TargetChar   Target code unit type.
 * @tparam TargetEndian Byte order of the produced code units. Defaults to native.
 */
template<std::endian SourceEndian, detail::code_unit TargetChar, std::endian TargetEndian = std::endian::native>
constexpr auto transcode()
{
	return transcode_closure<TargetChar, SourceEndian, TargetEndian>{};
}
```

Then add the namespace re-exports. Replace the closing of the namespaces at the bottom of the file:

```cpp
		} // namespace views

		using views::transcode;

	} // namespace ranges

	namespace views = ranges::views;

	using ranges::views::transcode;

} // namespace alt
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build --preset debug --target alt_tests && ctest --preset debug --output-on-failure -R Transcode`
Expected: PASS (`Transcode.Utf8ToUtf32Native`, `Transcode.NamespaceAliasesNameSameThing`, and all earlier `TranscodeDetail.*`).

- [ ] **Step 5: Commit**

```bash
git add include/alt/transcode.hpp tests/test_transcode.cpp
git commit -m "Add transcode view, iterator, closure, and transcode() functions"
```

---

### Task 5: Comprehensive behavior tests

**Files:**
- Test: `tests/test_transcode.cpp`

- [ ] **Step 1: Add the spec round-trip and coverage tests**

Add to `tests/test_transcode.cpp`. Helper for the stored value of a code unit in a given byte order:

```cpp
template<detail::code_unit C>
constexpr C ordered(C value, std::endian order)
{
	return detail::order_unit<C>(value, order);
}

TEST(Transcode, SpecRoundTrips)
{
	std::string s1 = "Hello world";

	auto s2 = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	auto s3 = s1 | alt::transcode<char32_t, std::endian::big>() | std::ranges::to<std::basic_string>();
	auto s4 = s3 | alt::transcode<std::endian::big, char16_t>() | std::ranges::to<std::basic_string>();
	auto s5 = s3 | alt::transcode<std::endian::big, char16_t, std::endian::big>() | std::ranges::to<std::basic_string>();

	static_assert(std::same_as<decltype(s2), std::u32string>);
	static_assert(std::same_as<decltype(s4), std::u16string>);

	// s2: native UTF-32.
	EXPECT_EQ(s2, U"Hello world");

	// s3: big-endian UTF-32 (each unit byte-swapped on a little-endian host).
	std::u32string expected_s3;
	for (char32_t c : std::u32string(U"Hello world"))
	{
		expected_s3.push_back(ordered(c, std::endian::big));
	}
	EXPECT_EQ(s3, expected_s3);

	// s4: native UTF-16 recovered from big-endian UTF-32.
	EXPECT_EQ(s4, u"Hello world");

	// s5: big-endian UTF-16.
	std::u16string expected_s5;
	for (char16_t c : std::u16string(u"Hello world"))
	{
		expected_s5.push_back(ordered(c, std::endian::big));
	}
	EXPECT_EQ(s5, expected_s5);
}

TEST(Transcode, AllDirectionsBmpAndSupplementary)
{
	// Mix of ASCII, BMP (€ U+20AC), and supplementary (😀 U+1F600).
	std::string utf8 = "A€\U0001F600";

	auto u32 = utf8 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(u32, U"A€\U0001F600");

	auto u16 = u32 | alt::transcode<char16_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(u16, u"A€\U0001F600");

	auto back8 = u16 | alt::transcode<char8_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(back8, u8"A€\U0001F600");

	// char target is treated as UTF-8.
	auto back_char = u32 | alt::transcode<char>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(back_char, "A€\U0001F600");
}

TEST(Transcode, InvalidInputBecomesReplacement)
{
	// Invalid UTF-8 byte in the middle.
	std::vector<char8_t> bad8{u8'A', 0xFF, u8'B'};
	auto out = bad8 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(out, std::u32string{U'A', detail::replacement_character, U'B'});

	// Lone high surrogate in UTF-16.
	std::vector<char16_t> bad16{u'A', 0xD83D, u'B'};
	auto out2 = bad16 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(out2, std::u32string{U'A', detail::replacement_character, U'B'});

	// Out-of-range UTF-32 scalar.
	std::vector<char32_t> bad32{U'A', 0x110000, U'B'};
	auto out3 = bad32 | alt::transcode<char16_t>() | std::ranges::to<std::basic_string>();
	EXPECT_EQ(out3, std::u16string{u'A', static_cast<char16_t>(detail::replacement_character), u'B'});
}

TEST(Transcode, EmptyInput)
{
	std::string empty;
	auto out = empty | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
	EXPECT_TRUE(out.empty());
}

TEST(Transcode, ComposesWithOtherAdaptors)
{
	std::string s1 = "Hello world";
	auto first5 = s1
		| alt::transcode<char32_t>()
		| std::views::take(5)
		| std::ranges::to<std::basic_string>();
	EXPECT_EQ(first5, U"Hello");

	// Source produced by another adaptor (rvalue range).
	auto filtered = std::string("aXbXc")
		| std::views::filter([](char c) { return c != 'X'; })
		| alt::transcode<char32_t>()
		| std::ranges::to<std::basic_string>();
	EXPECT_EQ(filtered, U"abc");
}

TEST(Transcode, ModelsInputRange)
{
	using view_t = decltype(std::declval<std::string&>() | alt::transcode<char32_t>());
	static_assert(std::ranges::input_range<view_t>);
	static_assert(std::ranges::view<std::remove_cvref_t<view_t>>);
	SUCCEED();
}

TEST(Transcode, ConstexprEvaluation)
{
	constexpr auto first_unit = []
	{
		std::u8string_view s = u8"\U0001F600"; // 4 UTF-8 bytes -> 1 UTF-32 unit
		auto view = s | alt::transcode<char32_t>();
		return *view.begin();
	}();
	static_assert(first_unit == U'\U0001F600');
	SUCCEED();
}
```

Add `#include <string_view>` and `#include <type_traits>` to the test file if not already present.

- [ ] **Step 2: Build and run the full transcode suite**

Run: `cmake --build --preset debug --target alt_tests && ctest --preset debug --output-on-failure -R Transcode`
Expected: PASS (all `Transcode.*` and `TranscodeDetail.*`).

- [ ] **Step 3: Run the whole test suite (no regressions)**

Run: `ctest --preset debug --output-on-failure`
Expected: PASS (all existing suites plus transcode).

- [ ] **Step 4: Commit**

```bash
git add tests/test_transcode.cpp
git commit -m "Add comprehensive transcode tests: round-trips, errors, composition, constexpr"
```

---

### Task 6: Quality gates

**Files:** none (verification only), unless a check surfaces a fix.

- [ ] **Step 1: AddressSanitizer + UBSan**

Run:
```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R Transcode
```
Expected: PASS, no sanitizer diagnostics. If any fire, fix the header and re-run before committing.

- [ ] **Step 2: clang-tidy**

Run:
```bash
cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build/tidy --target alt_tests
```
Expected: build succeeds with zero clang-tidy findings (warnings are errors). Fix any findings in `include/alt/transcode.hpp`.

- [ ] **Step 3: cppcheck**

Run:
```bash
cmake -B build/check -DALT_CPPCHECK=ON
cmake --build build/check --target cppcheck
```
Expected: zero findings.

- [ ] **Step 4: Commit any fixes**

If steps 1-3 required changes:
```bash
git add include/alt/transcode.hpp
git commit -m "Address sanitizer/clang-tidy/cppcheck findings for transcode"
```
If no changes were needed, skip this commit.

---

## Self-Review Notes (for the implementer)

- **Spec coverage:** lazy view (Task 4) · all four char types as source/target (Tasks 2-5) · call grammar with two overloads (Task 4) · source endian ignored for UTF-8 via `order_unit` no-op (Task 1) · U+FFFD substitution incl. maximal-subpart UTF-8 (Task 3) · all four namespace spellings (Task 4) · `constexpr` (Task 5) · `docs/transcode.md` already present.
- **Type consistency:** the four template parameters are ordered `<V, TargetChar, SourceEndian, TargetEndian>` on `transcode_iterator`, `transcode_view`; `<TargetChar, SourceEndian, TargetEndian>` on `transcode_closure`. `transcode()` overloads always forward to `transcode_closure<TargetChar, SourceEndian, TargetEndian>`. Keep this order everywhere.
- **If `input_iterator` concept checks fail to compile:** confirm `value_type`, `difference_type`, and `iterator_concept` are all present on `transcode_iterator` and that `operator==(std::default_sentinel_t)` is `const`.
```

# Design: `alt::transcode` — composable UTF transcoding view

**Date:** 2026-06-07
**Component:** `include/alt/transcode.hpp` (header-only)
**Status:** Approved

## Goal

A UTF transcoding utility that composes with C++ ranges. Lazy. Supports
`char8_t`/`char` (UTF-8), `char16_t` (UTF-16), and `char32_t` (UTF-32) as both
source and target, in native or specified byte order. Does **not** support
`wchar_t`. `char` is treated as equivalent to `char8_t`.

Target usage:

```cpp
std::string s1 = "Hello world";
auto s2 = s1 | alt::transcode<char32_t>()                        | std::ranges::to<std::basic_string>();
auto s3 = s1 | alt::transcode<char32_t, std::endian::big>()      | std::ranges::to<std::basic_string>();
auto s4 = s3 | alt::transcode<std::endian::big, char16_t>()      | std::ranges::to<std::basic_string>();
auto s5 = s3 | alt::transcode<std::endian::big, char16_t, std::endian::big>() | std::ranges::to<std::basic_string>();

// s2: UTF-32, native endian
// s3: UTF-32, big endian
// s4: UTF-16, native endian
// s5: UTF-16, big endian
```

## Call grammar

`transcode` is a function template (note the `()` in usage) returning a
pipeable range-adaptor closure. The template-id grammar is:

- optional **leading `std::endian`** = source byte order (default native)
- required **char type** = target code unit type
- optional **trailing `std::endian`** = target byte order (default native)

Four forms:

| Form | Meaning |
|------|---------|
| `transcode<Target>` | target type, native in/out |
| `transcode<Target, OutEndian>` | target type + output byte order |
| `transcode<InEndian, Target>` | input byte order + target type |
| `transcode<InEndian, Target, OutEndian>` | all three |

The **source char type is deduced from the piped range's `value_type`**, not
specified.

### Disambiguation

Two overloaded function templates distinguish the leading parameter by *kind*:

```cpp
template<typename TargetChar, std::endian TargetEndian = std::endian::native>
constexpr auto transcode();

template<std::endian SourceEndian, typename TargetChar,
         std::endian TargetEndian = std::endian::native>
constexpr auto transcode();
```

`transcode<char32_t>` can only bind the first (a type cannot bind a
`std::endian` NTTP); `transcode<std::endian::big, char16_t>` can only bind the
second. Both return the same underlying
`transcode_closure<TargetChar, SourceEndian, TargetEndian>`.

## Namespacing

The adaptor lives in `alt::ranges::views`, with `namespace alt::views =
alt::ranges::views`, and is re-exported via `using` into `alt::ranges` and
`alt`. All of `alt::transcode`, `alt::views::transcode`,
`alt::ranges::transcode`, and `alt::ranges::views::transcode` name the same
entity — matching the standard ranges convention and the literal usage
`alt::transcode<...>()`.

## Architecture (lazy view)

- **`transcode_closure<TargetChar, SrcE, DstE>`** — derives
  `std::ranges::range_adaptor_closure` (C++23) for free pipe support. Its
  `operator()(viewable_range)` wraps the source in `std::views::all` and
  returns a `transcode_view`.
- **`transcode_view<V, TargetChar, SrcE, DstE>`** — a `view_interface` holding
  the source view; yields a transcoding **input iterator** + sentinel.
- **iterator** — holds the source `current`/`end`, plus a small output buffer
  `std::array<TargetChar, 4 / sizeof(TargetChar)>` (4 units for UTF-8, 2 for
  UTF-16, 1 for UTF-32) with a size and index. `operator++` advances the buffer
  index; when the buffer drains, it decodes one code point from the source
  (consuming 1–4 source units) and re-encodes it into the buffer. End = source
  exhausted **and** buffer drained. Input-range is sufficient for
  `| std::ranges::to<std::basic_string>()`.

The whole pipeline is `constexpr` (C++23 `std::byteswap` is constexpr;
`std::array` and the iteration logic are constexpr-friendly).

## Decode / encode / endianness

- Source char type deduced from `value_type`: `char`/`char8_t` → UTF-8,
  `char16_t` → UTF-16, `char32_t` → UTF-32. `char` ≡ `char8_t`.
- **Endianness = per-code-unit byte order.** On read, each 16/32-bit source
  unit is `std::byteswap`'d iff `SrcE != native`; on write, each target unit
  iff `DstE != native`. No-op for 8-bit units, so source endianness on a UTF-8
  source is silently ignored.
- A "big-endian UTF-32 string" therefore stores byte-swapped `char32_t` values:
  the in-memory byte sequence is big-endian.

## Error handling

Invalid **input** is replaced with `U+FFFD` (REPLACEMENT CHARACTER), following
the Unicode-recommended substitution of maximal subparts — one `U+FFFD` per
ill-formed maximal subsequence:

- UTF-8: invalid lead byte, unexpected continuation byte, overlong encoding,
  truncated sequence, or scalar > U+10FFFF / in surrogate range.
- UTF-16: unpaired high surrogate, lone low surrogate.
- UTF-32: scalar > U+10FFFF or in the surrogate range.

Encoding never fails — every code point handed to the encoder is already a
valid Unicode scalar (or the U+FFFD substitute).

## Files & testing

- New header-only `include/alt/transcode.hpp`.
- New `tests/test_transcode.cpp`, registered in `tests/CMakeLists.txt`.
- New user-facing `docs/transcode.md` (matching existing component docs).
- Doxygen on all public symbols; snake_case; tabs; American spelling.

Test coverage:

- The five spec round-trips (`s2`–`s5` plus the UTF-16→… continuation).
- All directions: UTF-8 ↔ UTF-16 ↔ UTF-32.
- BMP and supplementary code points (e.g. emoji requiring surrogate pairs).
- Both byte orders, including verification of byte-swapped storage.
- Every invalid-input class → `U+FFFD`.
- Empty input.
- `constexpr` evaluation (`static_assert` on a transcoded result).
- `std::ranges` composition: chaining with `filter`/`take`; lazy single-pass.

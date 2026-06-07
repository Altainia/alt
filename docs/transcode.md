# transcode

**Header:** `<alt/transcode.hpp>`

`alt::transcode` is a lazy, range-composable UTF transcoding view adaptor. It
converts a range of UTF code units into a range of code units in another UTF
encoding and/or byte order, on demand, without allocating intermediate storage.

```cpp
namespace alt::ranges::views {
    template<typename TargetChar, std::endian TargetEndian = std::endian::native>
    constexpr auto transcode();

    template<std::endian SourceEndian, typename TargetChar,
             std::endian TargetEndian = std::endian::native>
    constexpr auto transcode();
}

namespace alt::views = alt::ranges::views;
// also re-exported as alt::ranges::transcode and alt::transcode
```

Each overload returns a pipeable range-adaptor closure. The closure can be
applied to any viewable range of code units with `|`, and the resulting view
composes with the rest of the standard ranges library, including
`std::ranges::to`.

## Supported encodings

| Code unit | Encoding |
|-----------|----------|
| `char` / `char8_t` | UTF-8 |
| `char16_t` | UTF-16 |
| `char32_t` | UTF-32 |

`char` is treated as equivalent to `char8_t`. `wchar_t` is **not** supported.

The **source** encoding is deduced from the piped range's element type. The
**target** encoding is the `TargetChar` template argument.

## Template parameters

`transcode` accepts up to three template arguments. The leading argument's
*kind* selects the overload, so the four meaningful forms are:

| Form | Source byte order | Target | Target byte order |
|------|-------------------|--------|-------------------|
| `transcode<Target>()` | native | `Target` | native |
| `transcode<Target, OutEndian>()` | native | `Target` | `OutEndian` |
| `transcode<InEndian, Target>()` | `InEndian` | `Target` | native |
| `transcode<InEndian, Target, OutEndian>()` | `InEndian` | `Target` | `OutEndian` |

| Parameter | Description |
|-----------|-------------|
| `SourceEndian` | Byte order of the source code units. Defaults to `std::endian::native`. Ignored for 8-bit (UTF-8) sources. |
| `TargetChar` | Target code unit type: `char`, `char8_t`, `char16_t`, or `char32_t`. |
| `TargetEndian` | Byte order of the produced code units. Defaults to `std::endian::native`. No effect for 8-bit (UTF-8) targets. |

### Byte order

Endianness denotes the in-memory byte order of each multi-byte code unit. A view
targeting `char32_t` with `std::endian::big` on a little-endian platform
produces `char32_t` values whose bytes are stored big-endian (i.e. byte-swapped
relative to the native scalar value). Byte order has no effect on 8-bit code
units.

## Error handling

Ill-formed input is replaced with the Unicode REPLACEMENT CHARACTER `U+FFFD`,
following the Unicode-recommended substitution of maximal subparts (one `U+FFFD`
per ill-formed maximal subsequence). This covers invalid UTF-8 byte sequences,
unpaired UTF-16 surrogates, and out-of-range or surrogate UTF-32 scalars.

## `constexpr`

The entire pipeline is usable in constant expressions.

## Examples

### UTF-8 to UTF-32 (native)

```cpp
#include <alt/transcode.hpp>
#include <ranges>
#include <string>

std::string s1 = "Hello world";
auto s2 = s1 | alt::transcode<char32_t>() | std::ranges::to<std::basic_string>();
// s2 is a std::u32string, native endian
```

### Specifying output byte order

```cpp
auto s3 = s1 | alt::transcode<char32_t, std::endian::big>()
             | std::ranges::to<std::basic_string>();
// s3 is a UTF-32 string stored big-endian
```

### Specifying input byte order

```cpp
auto s4 = s3 | alt::transcode<std::endian::big, char16_t>()
             | std::ranges::to<std::basic_string>();
// reads s3 as big-endian UTF-32, produces native-endian UTF-16

auto s5 = s3 | alt::transcode<std::endian::big, char16_t, std::endian::big>()
             | std::ranges::to<std::basic_string>();
// reads big-endian UTF-32, produces big-endian UTF-16
```

### Composing with other range adaptors

```cpp
auto first_ascii = s1
    | alt::transcode<char32_t>()
    | std::views::take(5)
    | std::ranges::to<std::basic_string>();
```

# base64

Base 64 encoding and decoding, as specified by [RFC 4648][rfc4648].

```cpp
#include <alt/base64.hpp>
```

## Interface

| Name | Description |
|------|-------------|
| `alt::base64_encode(input)` | Returns the base64 text of a range of bytes |
| `alt::base64_decode(text)` | Returns the bytes of base64 text, or why it was rejected |
| `alt::views::base64_encode<Alphabet>()` | Lazy, pipeable encoding adaptor |
| `alt::views::base64_decode<Alphabet, Policy>()` | Lazy, pipeable decoding adaptor |
| `alt::base64_encoded_size<Alphabet>(n)` | Exact character count `n` bytes encode to |
| `alt::base64_error` | Why a decode was rejected |
| `alt::base64_error_message(error)` | A short description of a rejection |
| `alt::base64_exception` | Thrown by a decode view under the throwing policy |

## Encoding

```cpp
const std::string text = alt::base64_encode("foobar");   // "Zm9vYmFy"
```

`input` may be any range of bytes: a `std::string_view`, a `std::vector`, a
`std::span`, or a lazy view. Raw arrays of `char` are excluded so that a string
literal cannot be encoded together with its terminating NUL; a literal binds to
a `std::string_view` overload that encodes exactly the characters. Binary data
held in a `char` buffer should be passed as a `std::span`.

Encoding cannot fail.

## Decoding

```cpp
const auto bytes = alt::base64_decode("Zm9vYmFy");
if (bytes) {
    consume(*bytes);            // std::vector<std::byte>
} else {
    report(alt::base64_error_message(bytes.error()));
}
```

Both functions are `constexpr`, so a fixed input can be encoded or decoded at
compile time.

## Alphabets

Every function and adaptor takes an alphabet as its first template parameter,
defaulting to the standard one.

| Alphabet | 62nd and 63rd characters | Padding | Reference |
|----------|--------------------------|---------|-----------|
| `alt::base64_standard_alphabet` | `+` `/` | yes | RFC 4648 section 4 |
| `alt::base64_url_alphabet` | `-` `_` | yes | RFC 4648 section 5 |
| `alt::base64_url_unpadded_alphabet` | `-` `_` | no | RFC 4648 sections 5 and 3.2 |

```cpp
const std::string token = alt::base64_encode<alt::base64_url_unpadded_alphabet>(payload);
```

RFC 4648 section 3.4 notes that `+` and `/` are awkward in URLs, in file names,
and to legacy text indexers. Prefer a URL-safe alphabet where the output travels
through any of those. The unpadded variant is the one used by JSON Web
Signature; RFC 4648 section 5 permits omitting the padding only where the data
length is known implicitly.

A user-defined alphabet is any type satisfying `alt::base64_alphabet`, which
requires a `name`, exactly 64 `characters` giving sextet values 0 through 63 in
order, and a `padding` character. Set `padding` to `alt::base64_no_padding` to
omit padding entirely. The padding character must not also be an alphabet
character, or the encoding would be ambiguous.

## Strictness

Decoding is strict, which is what RFC 4648 requires of a decoder whose referring
specification says nothing else.

- **Non-alphabet characters are rejected**, whitespace and line breaks included,
  per section 3.3. Being liberal here is a security decision, not a convenience
  one: the section notes that such characters serve as a covert channel.
- **Padding must complete the final quantum** and appear nowhere else, per
  section 3.2.
- **The discarded bits of a final quantum must be zero**, which section 3.5
  permits a decoder to require. Without it `Zg==` and `Zh==` would both decode to
  `f`, so the encoding would carry no canonical form and two different texts
  could stand for the same bytes.

MIME wraps encoded data at 76 characters (section 3.1). Since those line breaks
are non-alphabet characters, a MIME caller strips them first, which the decode
view composes with directly:

```cpp
auto stripped = text | std::views::filter([](char c) { return c != '\n' && c != '\r'; });
for (const auto& byte : stripped | alt::views::base64_decode()) {
    ...
}
```

### Rejection reasons

| `alt::base64_error` | Meaning |
|---------------------|---------|
| `invalid_character` | A character outside the alphabet appeared |
| `invalid_length` | The length describes no whole number of bytes |
| `unexpected_padding` | Padding appeared before the end of the data |
| `missing_padding` | The final quantum was short of its padding |
| `non_canonical_bits` | The final quantum's discarded bits were not zero |

## Views

The eager functions are wrappers over two pipeable range adaptors, which encode
and decode lazily and allocate nothing.

```cpp
for (const char c : bytes | alt::views::base64_encode()) { ... }
```

Encoding cannot fail, so the encode adaptor takes only an alphabet. Decoding can,
and a range has no natural place to report it, so the decode adaptor takes an
error policy as its second template parameter.

| Policy | Element type | On malformed input |
|--------|--------------|--------------------|
| `alt::base64_expected_errors` (default) | `std::expected<std::byte, alt::base64_error>` | Yields one element holding the error, then the range ends |
| `alt::base64_throw_errors` | `std::byte` | Throws `alt::base64_exception` |

```cpp
// Values, not exceptions: the range ends after reporting the failure.
for (const auto& byte : text | alt::views::base64_decode()) {
    if (!byte) { report(byte.error()); break; }
    consume(*byte);
}

// A plain byte range, at the cost of reporting failure by exception.
using policy = alt::base64_throw_errors;
auto plain = text | alt::views::base64_decode<alt::base64_standard_alphabet, policy>();
```

## Sizes

`alt::base64_encoded_size<Alphabet>(n)` is exact rather than an upper bound, so
it is the right amount to reserve.

```cpp
static_assert(alt::base64_encoded_size(1) == 4);
static_assert(alt::base64_encoded_size<alt::base64_url_unpadded_alphabet>(1) == 2);
```

[rfc4648]: https://www.rfc-editor.org/rfc/rfc4648

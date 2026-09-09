# sha1

SHA-1, as specified by [FIPS 180-4][fips], the current Secure Hash Standard.

```cpp
#include <alt/sha1.hpp>
```

## Deprecation

**SHA-1 is deprecated and is not collision resistant.** NIST is transitioning
away from it for all applications by 31 December 2030, and intends to publish
FIPS 180-5 removing its specification before then. Do not use it where collision
resistance matters. For new work, use `alt::sha256` or `alt::sha512` from
[`<alt/sha2.hpp>`](sha2.md).

It is provided because protocols that specify it as a non-security checksum
still require it, most commonly the WebSocket opening handshake of
[RFC 6455][rfc6455] section 4.2.2, which concatenates the client key with a
fixed GUID, hashes it, and base64-encodes the result.

## Interface

| Name | Description |
|------|-------------|
| `alt::sha1(input)` | Returns the digest of `input` in one call |
| `alt::sha1_digest` | The 20-byte digest value type |
| `alt::sha1_hasher` | Incremental hasher: `update`, `finish`, `reset` |
| `alt::sha1_algorithm` | The algorithm trait, for use with the generic `alt::hasher` |

## One-shot hashing

```cpp
const alt::sha1_digest digest = alt::sha1("abc");
std::println("{}", digest);   // a9993e364706816aba3e25717850c26c9cd0d89d
```

`input` may be any range of bytes: a `std::string_view`, a `std::vector`, a
`std::span`, or a lazy view. See [Accepted input](#accepted-input) below.

## Incremental hashing

Useful when the message arrives in pieces, or is too large to hold at once.

```cpp
alt::sha1_hasher hasher;
hasher.update(header).update(body);

const alt::sha1_digest digest = hasher.finish();
```

`finish()` pads a copy of the internal state, so the hasher is left untouched
and can keep accepting input afterwards. There is no finished state to guard
against, and no need to construct a fresh hasher to continue.

`reset()` discards all input and returns the hasher to its initial state.

## Compile-time evaluation

Every algorithm is fully `constexpr`, so a digest of a compile-time constant is
itself a compile-time constant.

```cpp
static_assert(alt::sha1("abc") ==
              alt::sha1_digest::from_hex("a9993e364706816aba3e25717850c26c9cd0d89d").value());
```

## Accepted input

A byte range is any input range whose element type is `char`, `signed char`,
`unsigned char`, `char8_t`, or `std::byte`. Contiguity is not required, so a
lazy view works:

```cpp
alt::sha1(text | alt::views::transcode<char8_t>());
```

### String literals and the terminating NUL

A string literal is a character array whose last element is a NUL. Hashing it
whole would consume one byte more than the message it represents, so raw arrays
of `char` and `char8_t` are excluded from the byte-range concept. A literal
binds instead to the `std::string_view` overload, which drops the terminator:

```cpp
alt::sha1("abc");   // hashes 3 bytes, not 4
```

**Binary data held in a `char` buffer must be passed as a span.** Converting it
to a `std::string_view` would stop at the first NUL:

```cpp
char buffer[1024];
alt::sha1(std::span{reinterpret_cast<const std::byte*>(buffer), length});
```

Arrays of `unsigned char`, `signed char`, and `std::byte` carry no NUL
convention and are accepted whole.

## Message length

FIPS 180-4 defines SHA-1 over messages shorter than 2^64 bits, which is 2^61
bytes. Exceeding that is a precondition violation, not a checked condition. It
is unreachable in practice: 2^61 bytes is roughly seventy-three years of
continuous hashing at a gigabyte per second.

## See also

- [sha2.md](sha2.md) for SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/224, and
  SHA-512/256.

[fips]: https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
[rfc6455]: https://www.rfc-editor.org/rfc/rfc6455

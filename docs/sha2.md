# sha2

The SHA-2 family, as specified by [FIPS 180-4][fips]: SHA-224, SHA-256,
SHA-384, SHA-512, SHA-512/224, and SHA-512/256.

```cpp
#include <alt/sha2.hpp>
```

For SHA-1, which is deprecated and lives in its own header so this one need not
be included to get it, see [sha1.md](sha1.md).

## Algorithms

| Function | Digest | Block | Specification |
|----------|--------|-------|---------------|
| `alt::sha224` | 28 bytes | 512-bit | sections 5.3.2, 6.3 |
| `alt::sha256` | 32 bytes | 512-bit | sections 5.3.3, 6.2 |
| `alt::sha384` | 48 bytes | 1024-bit | sections 5.3.4, 6.5 |
| `alt::sha512` | 64 bytes | 1024-bit | sections 5.3.5, 6.4 |
| `alt::sha512_224` | 28 bytes | 1024-bit | sections 5.3.6.1, 6.6 |
| `alt::sha512_256` | 32 bytes | 1024-bit | sections 5.3.6.2, 6.7 |

Each has a matching `_digest` type, `_hasher` type, and `_algorithm` trait, for
example `alt::sha256_digest`, `alt::sha256_hasher`, and
`alt::sha256_algorithm`.

## Usage

```cpp
const alt::sha256_digest digest = alt::sha256("abc");
std::println("{}", digest);   // ba7816bf8f01cfea...

alt::sha512_hasher hasher;
hasher.update(header).update(body);
const alt::sha512_digest sum = hasher.finish();
```

Input handling, incremental hashing, and compile-time evaluation work exactly as
described in [sha1.md](sha1.md), including the rule about string literals and
the terminating NUL. Everything there applies here.

## Digests are typed by algorithm, not by length

SHA-224 and SHA-512/224 both produce 28 bytes, and SHA-256 and SHA-512/256 both
produce 32. Keying the digest type on length alone would make each pair
interchangeable, turning a mixed-up algorithm into a silent comparison failure.
They are distinct types instead:

```cpp
const auto a = alt::sha256(message);
const auto b = alt::sha512_256(message);

a == b;   // ill-formed: different types, despite both being 32 bytes
```

## Choosing an algorithm

`alt::sha256` is the reasonable default. On 64-bit hardware the SHA-512 family
is often faster than the SHA-256 family despite the longer digest, because it
works in 64-bit words, so `alt::sha512_256` is worth considering where a 32-byte
digest is wanted at 64-bit speed.

`alt::sha224` and `alt::sha512_224` exist for interoperability with systems that
specify them. NIST's draft SP 800-131A revision 3 proposes retiring 224-bit hash
functions, so prefer a longer digest for new work.

## Derived initial values

FIPS 180-4 section 5.3.6 specifies the initial values for SHA-512/224 and
SHA-512/256 as the output of a generation function, then publishes the resulting
constants. This library runs that generation function at compile time rather
than transcribing its output, and the test suite asserts that the derived values
match the published ones. A transcription error therefore cannot pass unnoticed.

## Message length

FIPS 180-4 defines the 512-bit-block algorithms over messages shorter than 2^64
bits and the 1024-bit-block algorithms over messages shorter than 2^128 bits.
The internal byte counter is sized per family so that the standard, rather than
this implementation, is always the binding limit. Exceeding a ceiling is a
precondition violation and is unreachable in practice.

## See also

- [sha1.md](sha1.md) for SHA-1 and the shared input rules.

[fips]: https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf

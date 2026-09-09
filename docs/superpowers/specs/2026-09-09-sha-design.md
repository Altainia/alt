# Design: `alt` secure hash algorithms — FIPS 180-4 family

**Date:** 2026-09-09
**Components:**
- `include/alt/sha1.hpp` (header-only, public)
- `include/alt/sha2.hpp` (header-only, public)
- `include/alt/detail/digest.hpp` (header-only, helper)
- `include/alt/detail/hash.hpp` (header-only, helper)
- `include/alt/concepts.hpp` (existing public header, gains `byte_range`)

**Status:** Approved

## Goal

A generic hashing framework covering every algorithm specified by FIPS 180-4,
usable at compile time, accepting any range of bytes, and producing a strongly
typed digest value.

Target usage:

```cpp
#include <alt/sha2.hpp>

// One-shot
const alt::sha256_digest d = alt::sha256("abc");

// Compile-time
static_assert(alt::sha256("abc") ==
              alt::sha256_digest::from_hex(
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad").value());

// Streaming
alt::sha512_hasher hasher;
hasher.update(header).update(body);
const alt::sha512_digest sum = hasher.finish();

// Lazy ranges, including this library's own views
const auto utf8 = alt::sha256(text | alt::views::transcode<char8_t>());

// Formatting and parsing
std::println("{}", d);
const auto parsed = alt::sha256_digest::from_hex(user_input);
```

## Specification grounding

The normative source is **FIPS 180-4** (August 2015), *Secure Hash Standard*,
which remains the governing specification for all seven algorithms. Every
constant, initial value, round function, padding rule, and computation step in
this design is taken from it, specifically:

| Item | Section |
|------|---------|
| SHA-1 functions and constants | 4.1.1, 4.2.1 |
| SHA-224/256 functions and constants | 4.1.2, 4.2.2 |
| SHA-384/512 functions and constants | 4.1.3, 4.2.3 |
| Padding (512-bit and 1024-bit blocks) | 5.1.1, 5.1.2 |
| Parsing into blocks | 5.2.1, 5.2.2 |
| Initial hash values | 5.3.1 through 5.3.6 |
| SHA-512/t IV generation function | 5.3.6 |
| Hash computations | 6.1.2, 6.2.2, 6.3, 6.4.2, 6.5, 6.6, 6.7 |

RFC 3174 and FIPS 180-1, cited by the sister project's `detail/sha1.hpp`, are
older restatements of the same SHA-1 algorithm and are not used as the source
here.

### Status of SHA-1

NIST has deprecated SHA-1 and intends to publish FIPS 180-5 removing its
specification before 31 December 2030, with SHA-1 disallowed for applying
cryptographic protection after that date. FIPS 180-5 does not exist yet, so
FIPS 180-4 is still current. SHA-1 is included here because it remains required
by protocols that specify it as a non-security checksum, notably the WebSocket
handshake of RFC 6455. Its Doxygen comment and its page in `docs/` will state
the deprecation plainly. Nothing in this library treats SHA-1 as
collision-resistant.

## Architecture

All seven algorithms are the same Merkle-Damgard construction. Input is
buffered into fixed-size blocks, each block is compressed into a state of
words, and finalization appends a `0x80` byte, a zero run, and a big-endian bit
length, then serializes the state big-endian and truncates it. Exactly six
things vary:

| Varying element | SHA-1 | SHA-224/256 | SHA-384/512, SHA-512/224, SHA-512/256 |
|---|---|---|---|
| Word type | `uint32_t` | `uint32_t` | `uint64_t` |
| Block size | 64 bytes | 64 bytes | 128 bytes |
| Length field | 8 bytes | 8 bytes | 16 bytes |
| Byte counter | 64-bit | 64-bit | 128-bit |
| State words | 5 | 8 | 8 |
| Rounds | 80 | 64 | 80 |
| Digest bytes | 20 | 28 / 32 | 48 / 64 / 28 / 32 |

That variance is captured in an algorithm trait; the invariant machinery lives
in one engine. Padding is the part of a hash implementation that attracts
subtle bugs, so it is written once and tested once, exhaustively.

### `alt::hash_algorithm` concept (`detail/hash.hpp`)

A trait type satisfies `hash_algorithm` when it provides:

```cpp
struct example_algorithm
{
    using word_type = std::uint32_t;                    // or std::uint64_t
    using state_type = std::array<word_type, 8>;        // 5 words for SHA-1

    static constexpr std::size_t block_size  = 64;      // bytes per block
    static constexpr std::size_t length_size = 8;       // bytes of trailing length field
                                                        // also selects the byte-counter width
    static constexpr std::size_t digest_size = 32;      // bytes of output
    static constexpr std::string_view name   = "SHA-256";
    static constexpr state_type initial_value = { /* FIPS 180-4 section 5.3 */ };

    static constexpr void compress(state_type& state,
                                   std::span<const std::uint8_t, block_size> block) noexcept;
};
```

The state is the trait's own `state_type`, which fixes the word count for that
algorithm. `length_size` does double duty: it sizes the trailing length field
and selects the width of the engine's byte counter, so the two can never
disagree. See *Message length ceilings* under Error handling. Truncation is implicit: finalization serializes the full state
big-endian and copies the leading `digest_size` bytes, which is exactly how
sections 6.3, 6.5, 6.6, and 6.7 define SHA-224, SHA-384, SHA-512/224, and
SHA-512/256.

Bytes are `std::uint8_t` throughout the engine and the digest, rather than
`std::byte`, because every operation performed on them here is arithmetic:
word packing, hex conversion, and length encoding. `std::byte` remains
accepted on input through `byte_range` and is converted on the way in.

### `alt::hasher<A>` (`detail/hash.hpp`)

Holds the state, a partial-block buffer, and a byte counter. Members:

```cpp
constexpr hasher() noexcept;                             // state = A::initial_value
constexpr hasher& update(byte_range auto&& input);       // chainable
[[nodiscard]] constexpr digest<A> finish() const;        // pads a copy
constexpr void reset() noexcept;
```

`finish()` is `const` and pads a copy of the state. A hasher therefore has no
finished state to guard against, and callers can take an intermediate digest
and keep feeding the same hasher. The copy is at most a few hundred bytes.

`update` has two paths. A `std::ranges::contiguous_range` is consumed by block
copies. Any other input range is consumed element by element, which is what
allows a lazy view to feed a hasher directly.

### `alt::digest<A>` (`detail/digest.hpp`)

Keyed on the algorithm, not on the byte length. SHA-224 and SHA-512/224 both
produce 28 bytes and SHA-256 and SHA-512/256 both produce 32; keying on length
would make those pairs the same type and let a mixed-up algorithm produce a
silent comparison failure instead of a compile error.

```cpp
constexpr std::span<const std::uint8_t, A::digest_size> bytes() const noexcept;
constexpr std::size_t size() const noexcept;
constexpr std::uint8_t operator[](std::size_t index) const;
constexpr auto begin() const noexcept;  // and end()

constexpr std::array<char, A::digest_size * 2> to_hex() const noexcept;  // lowercase
std::string to_string() const;

static constexpr std::expected<digest, hex_error> from_hex(std::string_view text);

friend constexpr bool operator==(const digest&, const digest&) noexcept;
friend constexpr auto operator<=>(const digest&, const digest&) noexcept;
```

`to_hex` returns a fixed-size array so it is usable inside a constant
expression, where a `std::string` could not escape. `to_string` is the
allocating convenience.

Specializations of `std::hash` and `std::formatter` are provided. The
`std::hash` specialization takes the leading bytes of the digest directly,
since a digest is already uniformly distributed and rehashing it buys nothing.

### `alt::byte_range` concept (`concepts.hpp`)

Satisfied by an input range whose value type is `char`, `unsigned char`,
`signed char`, `std::byte`, or `char8_t`. It is a general purpose concept with
no dependency on hashing, so it belongs in the existing public concepts header
rather than in a helper directory.

### Header layout and the `detail/` rule

`detail/digest.hpp` and `detail/hash.hpp` are placed in a subdirectory because
neither has a standalone use: every digest a caller holds came out of a hasher,
and `hasher<A>` cannot be instantiated without an algorithm. A caller always
reaches them through `sha1.hpp` or `sha2.hpp`.

**The subdirectory describes includability, not visibility.** The symbols in
those two files live in namespace `alt`, are fully documented, and are part of
the public API. `alt::digest` is a type callers name, store, compare, and
format. Only the engine internals live in `alt::detail`. The `detail/` path
must not be read as permission to change those types freely.

SHA-1 and SHA-2 are split into separate headers so that the common case of
needing only SHA-1 does not pull in eighty 64-bit round constants it will never
use. `install(DIRECTORY include/alt)` in the root `CMakeLists.txt` is recursive
and unfiltered, so the subdirectory installs and packages with no build change.

## Public API surface

Each algorithm header contributes, per algorithm, a trait, two aliases, and a
one-shot function:

```cpp
namespace alt {
    struct sha256_algorithm { /* ... */ };
    using sha256_digest = digest<sha256_algorithm>;
    using sha256_hasher = hasher<sha256_algorithm>;
    [[nodiscard]] constexpr sha256_digest sha256(byte_range auto&& input);
    [[nodiscard]] constexpr sha256_digest sha256(std::string_view input);
}
```

`sha1.hpp` provides this for `sha1`. `sha2.hpp` provides it for `sha224`,
`sha256`, `sha384`, `sha512`, `sha512_224`, and `sha512_256`.

### String literals and the terminating NUL

A string literal is a character array whose final element is a NUL. Hashing it
as a plain range would consume one byte more than every published test vector
produces, silently.

**Rule:** raw arrays of `char` and `char8_t`, the two element types that carry
a NUL-termination convention, are excluded from `byte_range`. Arrays of
`signed char`, `unsigned char`, and `std::byte` carry no such convention and
are accepted whole. String literals therefore bind to the `std::string_view`
overload, which drops the terminator and yields the published vectors.

**Residual hazard, stated deliberately.** A non-literal `char` buffer holding
binary data also binds to the `std::string_view` overload, whose pointer
constructor stops at the first NUL. Binary data must be passed as a
`std::span`, or as a container of `unsigned char` or `std::byte`. The
alternative, making every raw `char` array ill-formed, would break
`alt::sha1("abc")`, which is the single most common call this library will
receive. This is the same trade-off `std::string_view` itself makes, so the
behavior is idiomatic rather than novel. It is stated in the Doxygen comments
and in the component documentation.

## Error handling

Hashing cannot fail, so nothing on the hashing path returns a status and the
hashing entry points are `noexcept` where the input range permits.

Hex parsing can fail, so `from_hex` returns
`std::expected<digest<A>, hex_error>`, per the project's error-handling rule,
with `hex_error` distinguishing:

- `invalid_length` — the input is not exactly `2 * digest_size` characters.
- `invalid_character` — the input contains a character outside `[0-9a-fA-F]`.

Uppercase hex input is accepted on parse; `to_hex` always emits lowercase.

### Message length ceilings

FIPS 180-4 defines each algorithm over a bounded message domain, and the bound
differs by family because the padding block carries the message length in a
fixed-width field.

| Family | Length field | Spec ceiling | Byte counter | Binding limit |
|---|---|---|---|---|
| SHA-1, SHA-224, SHA-256 | 64 bits | 2^61 bytes | 64-bit | the spec |
| SHA-384, SHA-512, SHA-512/224, SHA-512/256 | 128 bits | 2^125 bytes | 128-bit | the spec |

The counter width is chosen per family so that **the specification is always
the binding limit, never an artifact of this implementation.**

For the 512-bit-block family a 64-bit byte counter overshoots the spec's own
domain: the standard stops defining behavior at 2^61 bytes, long before the
counter could wrap at 2^64. No conformance gap exists, so a 64-bit counter is
used.

For the 1024-bit-block family the reasoning inverts. A 64-bit byte counter
would wrap at 2^64 bytes, which lies **inside** the domain FIPS 180-4 defines.
Those are messages the standard specifies an answer for and that this library
would answer incorrectly. The gap is unreachable in practice, but it is a
conformance shortfall rather than an out-of-domain input, so these algorithms
carry a 128-bit byte counter built from a pair of 64-bit words. The cost is a
carry-add per `update` and a second word written during padding.

Exceeding a ceiling remains a documented precondition, not a checked condition.
A per-`update` comparison would be a branch on the hot path that no execution
ever takes; at one gigabyte per second, 2^61 bytes is roughly seventy-three
years of continuous hashing, and this is a portable C++ engine that will not
reach that rate.

### Testing the ceilings

Unreachable by a real message is not the same as untestable. The ceiling
arithmetic is reachable directly if it is factored out of the streaming path,
which is the same test-seam argument the sister project's `detail/sha1.hpp`
makes about its own placement. Two helpers in `alt::detail` provide that seam:

```cpp
template<std::size_t LengthSize>
class byte_counter;                       // 64- or 128-bit, constexpr carry-add

template<std::size_t LengthSize>
constexpr std::array<std::uint8_t, LengthSize>
encode_bit_length(byte_counter<LengthSize> bytes) noexcept;
```

A test sets a counter to any value it likes and inspects the encoded field, so
boundary behavior is pinned by assertion rather than left to inference. What
this seam verifies is the length encoding, not a full digest of a hypothetical
exabyte message, which remains uncomputable. That limit is stated here so the
test group is not later mistaken for broader coverage than it has.

## Testing

Files: `tests/test_digest.cpp`, `tests/test_sha1.cpp`, `tests/test_sha2.cpp`,
each registered in the `alt_tests` source list in `tests/CMakeLists.txt`.

1. **Specification vectors.** For all seven algorithms: the FIPS one-block
   message `"abc"`, the corresponding multi-block message (the 448-bit
   `"abcdbcde..."` message for the 512-bit-block algorithms and the 896-bit
   `"abcdefghbcdefghi..."` message for the 1024-bit-block ones), and the
   one-million-character `"aaa..."` message. The first two are asserted with
   `static_assert` so a regression fails the build rather than the test run.
   The million-character vector runs at runtime to keep constexpr step limits
   sane.

2. **Padding boundaries.** Input lengths straddling every block edge: 0, 1, 55,
   56, 63, 64, 65, 119, 120, 127, 128, and 129 bytes for the 512-bit engine;
   0, 1, 111, 112, 127, 128, 129, 239, 240, 255, and 256 bytes for the
   1024-bit engine.

3. **Streaming equivalence.** Each vector split at every possible offset, and
   fed as two or three `update` calls, must equal the one-shot result. This is
   the property that catches partial-buffer bugs.

4. **Derived initial values.** The SHA-512/t IV generation function of section
   5.3.6 is implemented and evaluated at compile time, asserting that it
   reproduces the published SHA-512/224 and SHA-512/256 initial values. This
   checks the transcription against the standard rather than against itself.

5. **Digest behavior.** Hex round trips, both `hex_error` cases, uppercase
   input, ordering, `std::hash` usability in an unordered container, and
   `std::formatter` output.

6. **Range coverage.** The same input hashed as a contiguous container, as a
   `std::span<const std::byte>`, and as a non-contiguous lazy view must produce
   identical digests, exercising both `update` paths.

7. **Length ceilings.** Through the `byte_counter` and `encode_bit_length`
   seam described under Error handling, at compile time: the 64-bit field at
   2^61 minus one bytes, which encodes the largest length the spec defines, and
   at 2^61 bytes, which is the first value outside it; the 128-bit field at
   2^64 bytes, the value a 64-bit counter would have wrapped at, and at 2^125
   bytes; and carry propagation across the 64-bit word boundary of the 128-bit
   counter.

All of the above must pass under the ASan and UBSan preset, clang-tidy, and
cppcheck, per the project's quality gates.

## Documentation

`docs/sha1.md` and `docs/sha2.md`, following the existing per-component
documentation pattern. The SHA-1 page leads with its deprecation status and the
narrow reason it is still provided.

## Out of scope

- HMAC, PBKDF2, and any keyed construction.
- SHA-3 and Keccak, which are FIPS 202 rather than FIPS 180-4.
- SHA-512/t for values of t other than the approved 224 and 256.
- Hardware acceleration intrinsics. The engine is portable C++23; a caller
  needing throughput should reach for a dedicated cryptographic library.
- Constant-time digest comparison. These are unkeyed hashes and nothing here
  compares a secret.

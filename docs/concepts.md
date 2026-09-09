# concepts

**Header:** `<alt/concepts.hpp>`

Provides three concepts used throughout the library and available for use in consuming code.

## `any_enum<T>`

Satisfied by any enumeration type — both unscoped (`enum`) and scoped (`enum class` / `enum struct`).

```cpp
enum Color { Red, Green, Blue };
enum class Direction { North, South };

static_assert(alt::any_enum<Color>);
static_assert(alt::any_enum<Direction>);
static_assert(!alt::any_enum<int>);
```

## `scoped_enum<T>`

Satisfied only by scoped enumerations (`enum class` or `enum struct`). Unscoped enums do not satisfy it.

```cpp
enum Unscoped { A };
enum class Scoped { B };

static_assert(!alt::scoped_enum<Unscoped>);
static_assert(alt::scoped_enum<Scoped>);
```

## `flag_enum<T>`

Satisfied by any enumeration, scoped or unscoped, whose underlying type has an unsigned counterpart to hold the bits in. That excludes an underlying type of `bool`, which `std::make_unsigned` cannot convert.

```cpp
enum Unscoped { A = 1 };
enum class Scoped : uint8_t { B = 1 };
enum class Flagless : bool { C = true };

static_assert(alt::flag_enum<Unscoped>);
static_assert(alt::flag_enum<Scoped>);
static_assert(!alt::flag_enum<Flagless>);
static_assert(!alt::flag_enum<int>);
```

`flag_enum` is the constraint used by [`alt::flags`](flags.md). Scoping is deliberately not required: it is a property of the enumeration itself, and nothing `alt::flags` depends on.

## `bool_condition<T>`

Satisfied when `T` can act as a boolean condition. A type satisfies `bool_condition` when either:

1. It is implicitly convertible to `bool` **and** is not a callable type (no `operator()`, not a function pointer). This excludes lambdas and function pointers so they remain available as *transformers* in the functional combinators.
2. It is a nullary invocable (callable with no arguments) whose return value is convertible to `bool`.

```cpp
// Scalars and booleans
static_assert(alt::bool_condition<bool>);
static_assert(alt::bool_condition<int>);
static_assert(alt::bool_condition<int*>);   // pointer — convertible to bool

// Nullary lambdas — satisfied as invocables
auto cond = [] { return true; };
static_assert(alt::bool_condition<decltype(cond)>);

// Lambdas with parameters — NOT bool_condition (they are transformers)
auto pred = [](int x) { return x > 0; };
static_assert(!alt::bool_condition<decltype(pred)>);
```

This concept drives the overload resolution in `<alt/functional.hpp>`, ensuring that a callable that looks like a condition is treated as one and a callable that takes arguments is treated as a transformer.

## `byte_range<T>`

Satisfied by an input range of raw bytes. The element type must be `char`, `signed char`, `unsigned char`, `char8_t`, or `std::byte`. Contiguity is not required, so a lazy view of bytes qualifies just as a `std::vector` or `std::span` does.

```cpp
static_assert(alt::byte_range<std::string_view>);
static_assert(alt::byte_range<std::vector<std::byte>>);
static_assert(alt::byte_range<std::span<const unsigned char>>);
static_assert(alt::byte_range<std::deque<char>>);       // not contiguous

static_assert(!alt::byte_range<std::vector<int>>);
static_assert(!alt::byte_range<std::wstring>);
```

Raw arrays of `char` and `char8_t` are deliberately excluded. Those are the element types carrying a NUL-termination convention, and a string literal is such an array, so treating one as a plain range would process the terminator as data:

```cpp
static_assert(!alt::byte_range<char[4]>);               // a string literal
static_assert(alt::byte_range<unsigned char[4]>);       // no NUL convention
static_assert(alt::byte_range<std::array<char, 4>>);    // not a raw array
```

This concept constrains the input of the hashing components in [`<alt/sha1.hpp>`](sha1.md) and [`<alt/sha2.hpp>`](sha2.md), which pair it with a `std::string_view` overload so string literals still hash the characters they represent and nothing more.

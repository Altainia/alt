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

`scoped_enum` is the constraint used by `alt::flags`.

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

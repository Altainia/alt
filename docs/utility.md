# utility

**Header:** `<alt/utility.hpp>`

## `to_underlying`

```cpp
namespace alt {
    template<typename T>
    constexpr underlying_int_t<T> to_underlying(T value) noexcept;
}
```

Converts an enum or integral value to its underlying integer type. For enums this is equivalent to `static_cast<std::underlying_type_t<T>>(value)`; for integral types the value is returned unchanged. The function is `constexpr` and `noexcept`.

`T` must be either an enum type or an integral type. Passing any other type is a compile error.

## Example

```cpp
#include <alt/utility.hpp>

enum class Color : int { Red = 1, Green = 2, Blue = 3 };
enum Flags { Read = 1, Write = 2 };  // unscoped

constexpr int r = alt::to_underlying(Color::Red);   // 1
constexpr int w = alt::to_underlying(Write);         // 2
constexpr int n = alt::to_underlying(42);            // 42 (integral passthrough)

static_assert(r == 1);
static_assert(n == 42);
```

## Why not `std::to_underlying`?

`std::to_underlying` (C++23) only accepts enum types. `alt::to_underlying` also accepts integrals, returning them unchanged. This is convenient in generic code that may receive either an enum or a plain integer.

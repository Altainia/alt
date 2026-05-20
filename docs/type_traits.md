# type_traits

**Header:** `<alt/type_traits.hpp>`

Provides a type trait for extracting the underlying integer type of an enum or integral.

## `underlying_int<T>`

A trait struct with a member type `type` that yields:

- For **enum types**: `std::underlying_type_t<T>`
- For **integral types**: `T` unchanged

Instantiating with any other type is a compile error.

```cpp
enum class Status : short { Ok, Fail };

alt::underlying_int<Status>::type   // short
alt::underlying_int<int>::type      // int
alt::underlying_int<unsigned>::type // unsigned
```

## `underlying_int_t<T>`

Convenience alias for `underlying_int<T>::type`.

```cpp
alt::underlying_int_t<Status>  // short
alt::underlying_int_t<long>    // long
```

## Example

```cpp
#include <alt/type_traits.hpp>
#include <type_traits>

enum class Flags : uint8_t { A = 1, B = 2 };

static_assert(std::is_same_v<alt::underlying_int_t<Flags>, uint8_t>);
static_assert(std::is_same_v<alt::underlying_int_t<int>, int>);
```

`underlying_int_t` is the return type of `alt::to_underlying` and is used internally by `alt::flags`.

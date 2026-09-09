# projection

**Header:** `<alt/projection.hpp>`

Small function objects that select part of a composite value. They are ordinary callables, so they work anywhere a projection or a unary callable is accepted: `alt::erase`, `alt::erase_if`, the ranges algorithms, and the range adaptors.

```cpp
namespace alt {
    inline constexpr /* unspecified */ key{};    // t.first
    inline constexpr /* unspecified */ value{};  // t.second

    template<std::size_t N>
    inline constexpr /* unspecified */ element{};  // get<N>(t)
}
```

## `key` and `value`

`key(t)` returns `t.first` and `value(t)` returns `t.second`. They are named for their most common use, the two halves of an associative container's elements, but they accept any type with those members.

```cpp
#include <alt/projection.hpp>
#include <map>
#include <string>

std::map<int, std::string> m{{1, "one"}, {2, "two"}};

alt::key(*m.begin());    // 1
alt::value(*m.begin());  // "one"
```

## `element<N>`

`element<N>(t)` returns the Nth element of a tuple-like value. It calls `get<N>` unqualified, so it works with `std::tuple`, `std::pair`, `std::array`, and any user type that opts in by providing a `get()` reachable through argument-dependent lookup.

```cpp
#include <alt/projection.hpp>
#include <tuple>

std::tuple<int, char, double> t{1, 'x', 2.5};

alt::element<0>(t);  // 1
alt::element<1>(t);  // 'x'
```

For a pair, `element<0>` and `key` are interchangeable, as are `element<1>` and `value`. Prefer `key` and `value` when the pair really is a key and a mapped value, because the names say so.

## Constness and value category

All three return `decltype(auto)`, so a projection never silently copies. Projecting a mutable lvalue yields a mutable reference, a const lvalue yields a const reference, and an rvalue yields an rvalue reference.

```cpp
std::pair<int, std::string> p{1, "a"};
const std::pair<int, std::string> cp{1, "a"};

alt::value(p);              // std::string&
alt::value(cp);             // const std::string&
alt::value(std::move(p));   // std::string&&

alt::value(p) = "changed";  // writes through to p.second
```

The same caveat applies as to `std::get`: projecting a temporary yields a reference into that temporary, which dangles once the full expression ends. Range projections take references to elements the range owns, so this does not arise in normal use.

Each object is rejected at compile time for types it cannot handle. `key` and `value` require the corresponding member, and `element<N>` requires a usable `get<N>`, including a bounds check where the tuple-like type provides one.

## Examples

### Erasing from a map

```cpp
#include <alt/algorithm.hpp>
#include <alt/projection.hpp>
#include <map>
#include <string>

std::map<int, std::string> m{{1, "a"}, {2, "b"}, {3, "a"}};

alt::erase(m, std::string{"a"}, alt::value);            // removes 1 and 3
alt::erase_if(m, [](int k) { return k > 1; }, alt::key);
```

### Ranges algorithms and adaptors

```cpp
#include <alt/projection.hpp>
#include <algorithm>
#include <ranges>
#include <vector>

std::vector<std::pair<int, std::string>> rows{{3, "c"}, {1, "a"}, {2, "b"}};

auto it = std::ranges::find(rows, 2, alt::key);
std::ranges::sort(rows, {}, alt::key);

for (const auto& name : rows | std::views::transform(alt::value)) {
    // "a", "b", "c"
}
```

### Tuples

```cpp
#include <alt/projection.hpp>
#include <algorithm>
#include <tuple>
#include <vector>

std::vector<std::tuple<int, char>> rows{{1, 'a'}, {2, 'b'}, {3, 'c'}};

auto it = std::ranges::find(rows, 'b', alt::element<1>);
```

## Why not a lambda?

`[](const auto& p) -> decltype(auto) { return (p.second); }` is the equivalent lambda, and it is easy to get wrong: without the parentheses `decltype(auto)` deduces the member's declared type and copies it. These objects get it right once, read better at the call site, and being objects rather than templates they can be passed around and stored without spelling out a type.

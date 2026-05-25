# algorithm

**Header:** `<alt/algorithm.hpp>`

## `erase`

```cpp
namespace alt {
    template<typename Container, typename Value, typename Proj = std::identity>
    constexpr auto erase(Container& c, const Value& value, Proj proj = {})
        -> typename Container::size_type;
}
```

Erases all elements from `c` for which `proj(element) == value`. Returns the number of elements removed.

This mirrors C++20's `std::erase`, but adds a projection parameter (like the C++ ranges algorithms) so the comparison can be made against a transformed view of each element rather than the element itself. The default projection is `std::identity`, which makes it identical in behavior to `std::erase`.

Implemented via the erase-remove idiom: `std::remove_if` shuffles matching elements to the end of the range, then `Container::erase` excises them. Works with any sequence container that provides `begin()`, `end()`, `erase(iterator, iterator)`, and a `size_type` typedef — including `std::vector`, `std::deque`, and `std::string`.

### Template parameters

| Parameter | Description |
|-----------|-------------|
| `Container` | Sequence container with `erase` and iterator support. |
| `Value` | Type to compare projected elements against. |
| `Proj` | Callable projection; defaults to `std::identity`. |

### Parameters

| Parameter | Description |
|-----------|-------------|
| `c` | The container to modify in-place. |
| `value` | The value to compare projected elements against. |
| `proj` | Projection applied to each element before comparison. |

### Return value

The number of elements erased.

## Examples

### Identity projection (no projection)

```cpp
#include <alt/algorithm.hpp>
#include <vector>

std::vector<int> v{1, 2, 3, 2, 4, 2};
auto removed = alt::erase(v, 2);
// removed == 3
// v == {1, 3, 4}
```

### Member-pointer projection

```cpp
#include <alt/algorithm.hpp>
#include <vector>
#include <string>

struct employee { std::string name; int department_id; };

std::vector<employee> staff{{"Alice", 1}, {"Bob", 2}, {"Carol", 1}};
auto removed = alt::erase(staff, 1, &employee::department_id);
// removed == 2
// staff == {{"Bob", 2}}
```

### Lambda projection

```cpp
#include <alt/algorithm.hpp>
#include <vector>
#include <string>

std::vector<std::string> words{"hi", "hello", "hey", "world"};
auto removed = alt::erase(words, 2u, [](const std::string& s) { return s.size(); });
// removed == 2  (removed "hi" and "hey")
// words == {"hello", "world"}
```

## Why not `std::erase`?

C++20's `std::erase` only compares elements directly against the value. There is no standard equivalent that accepts a projection. `alt::erase` fills that gap, allowing removal based on a member, a transformation, or any other callable — without requiring a manual erase-remove idiom at the call site.

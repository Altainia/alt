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

Works with sequence containers and with the associative and unordered containers. See [Container support](#container-support) for how the removal strategy is chosen.

### Template parameters

| Parameter | Description |
|-----------|-------------|
| `Container` | Any container listed under [Container support](#container-support). |
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

## `erase_if`

```cpp
namespace alt {
    template<typename Container, typename Pred, typename Proj = std::identity>
    constexpr auto erase_if(Container& c, Pred pred, Proj proj = {})
        -> typename Container::size_type;
}
```

Erases all elements from `c` for which `pred(proj(element))` is true. Returns the number of elements removed.

This mirrors C++20's `std::erase_if`, but adds a projection parameter. The predicate sees the **projected** value rather than the element, matching how the ranges algorithms pair a predicate with a projection. With the default `std::identity` projection it behaves exactly like `std::erase_if`.

### Parameters

| Parameter | Description |
|-----------|-------------|
| `c` | The container to modify in-place. |
| `pred` | Predicate deciding whether a projected element is removed. |
| `proj` | Projection applied to each element before the predicate. |

### Return value

The number of elements erased.

## Container support

Both `erase` and `erase_if` accept two families of container and pick the removal strategy for each.

| Containers | Strategy | Cost |
|---|---|---|
| `vector`, `deque`, `string`, `list` | Erase-remove idiom: `std::remove_if` shuffles survivors forward, then a range `erase` excises the tail. | Linear |
| `map`, `multimap`, `set`, `multiset`, and the `unordered_` variants | Erased one node at a time, since their elements cannot be permuted. | Linear in the number of elements, plus the container's own per-erase cost |

The choice is made at compile time on whether the container's iterators are permutable, so a sequence container never falls back to the slower path.

`std::forward_list` is not supported and is rejected by the constraint rather than failing inside the body. It offers neither a range `erase` nor a single-element `erase`, splicing with `erase_after` instead.

### Map-like containers

For a map, an element is a `pair`, so the default identity projection compares against the whole pair. Pair `erase` and `erase_if` with [`alt::key` or `alt::value`](projection.md) to work on one half.

```cpp
#include <alt/algorithm.hpp>
#include <alt/projection.hpp>
#include <map>
#include <string>

std::map<int, std::string> m{{1, "a"}, {2, "b"}, {3, "a"}};

alt::erase(m, std::string{"a"}, alt::value);  // removes entries 1 and 3
alt::erase(m, 2, alt::key);                   // removes entry 2

alt::erase_if(m, [](int k) { return k > 10; }, alt::key);
```

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

### erase_if with a projection

```cpp
#include <alt/algorithm.hpp>
#include <vector>
#include <string>

struct employee { std::string name; int department_id; };

std::vector<employee> staff{{"Alice", 1}, {"Bob", 2}, {"Carol", 3}};
auto removed = alt::erase_if(staff, [](int id) { return id > 1; }, &employee::department_id);
// removed == 2
// staff == {{"Alice", 1}}
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

C++20's `std::erase` and `std::erase_if` compare or test elements directly. There is no standard equivalent that accepts a projection. `alt::erase` and `alt::erase_if` fill that gap, allowing removal based on a member, a transformation, or any other callable — without a manual erase-remove idiom at the call site.

`std::erase` also has no overload for the associative containers at all; only `std::erase_if` does. `alt::erase` covers both families through one signature.

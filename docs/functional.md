# functional

**Header:** `<alt/functional.hpp>`

Boolean combinators for testing multiple conditions or applying a predicate to multiple arguments. All functions are `constexpr`, `[[nodiscard]]`, and short-circuit — evaluation stops as soon as the result is determined.

## Two calling styles

Every combinator has two overloads:

**Condition overload** — pass conditions directly. Each argument must satisfy [`bool_condition`](concepts.md): a value convertible to `bool`, or a nullary callable returning `bool`.

```cpp
alt::any_of(a, b, c);
```

**Transformer overload** — pass a callable first, then the arguments it will be applied to. The first argument must *not* satisfy `bool_condition` (it cannot be a nullary lambda or plain bool value), which is how the compiler disambiguates the two overloads.

```cpp
alt::any_of([](int x) { return x > 0; }, -1, 2, 3);
```

In the transformer overload, the callable `f` is applied to each argument. If `f(arg)` is not valid but `f(arg())` is (i.e. the argument is itself a nullary callable), then `arg()` is called first and the result is passed to `f`.

---

## `any_of`

Returns `true` if **at least one** condition is true (or `f` returns true for at least one argument). Returns `false` for an empty argument list.

```cpp
bool r1 = alt::any_of(false, false, true);   // true
bool r2 = alt::any_of(false, false, false);  // false
bool r3 = alt::any_of([] { return false; }, [] { return true; });  // true

// Transformer
bool r4 = alt::any_of([](int x) { return x < 0; }, 1, 2, -3);  // true
```

## `all_of`

Returns `true` if **every** condition is true (or `f` returns true for every argument). Vacuously true for an empty argument list.

```cpp
bool r1 = alt::all_of(true, true, true);    // true
bool r2 = alt::all_of(true, false, true);   // false
bool r3 = alt::all_of();                    // true (vacuous)

// Transformer
bool r4 = alt::all_of([](int x) { return x > 0; }, 1, 2, 3);   // true
bool r5 = alt::all_of([](int x) { return x > 0; }, 1, -2, 3);  // false
```

## `none_of`

Returns `true` if **no** condition is true (or `f` returns true for no argument). Vacuously true for an empty argument list.

```cpp
bool r1 = alt::none_of(false, false, false);  // true
bool r2 = alt::none_of(false, true, false);   // false

// Transformer
bool r3 = alt::none_of([](int x) { return x < 0; }, 1, 2, 3);  // true
```

## `not_all_of`

Returns `true` if **at least one** condition is false. Equivalent to `!all_of(...)`. Evaluation short-circuits on the first false condition.

```cpp
bool r1 = alt::not_all_of(true, false, true);  // true
bool r2 = alt::not_all_of(true, true, true);   // false

// Transformer
bool r3 = alt::not_all_of([](int x) { return x > 0; }, 1, -2, 3);  // true
```

---

## Counting combinators

These take a non-type template parameter `N` specifying a count threshold.

### `at_least<N>`

Returns `true` if at least `N` conditions are true. Always `true` when `N == 0`. Short-circuits once `N` true conditions have been seen.

```cpp
alt::at_least<2>(true, false, true, true);   // true  (3 >= 2)
alt::at_least<2>(true, false, false);        // false (1 < 2)
alt::at_least<0>(false, false);             // true  (vacuous)

// Transformer
alt::at_least<2>([](int x) { return x > 0; }, -1, 2, 3);  // true
```

### `at_most<N>`

Returns `true` if at most `N` conditions are true. Vacuously true for an empty argument list. Short-circuits once more than `N` true conditions have been seen.

```cpp
alt::at_most<1>(true, false, false);  // true  (1 <= 1)
alt::at_most<1>(true, true, false);   // false (2 > 1)
alt::at_most<0>();                    // true  (vacuous)

// Transformer
alt::at_most<1>([](int x) { return x < 0; }, -1, 2, 3);  // true
```

### `exactly<N>`

Returns `true` if exactly `N` conditions are true. Short-circuits once more than `N` true conditions have been seen.

```cpp
alt::exactly<2>(true, false, true, false);  // true
alt::exactly<2>(true, true, true);          // false (3 != 2)
alt::exactly<0>();                          // true

// Transformer
alt::exactly<1>([](int x) { return x < 0; }, -1, 2, 3);  // true
```

---

## Nullary callable disambiguation

A nullary lambda that returns `bool` satisfies `bool_condition` and is therefore always treated as a **condition**, not as a transformer:

```cpp
auto cond = [] { return true; };

// cond is a bool_condition — condition overload is selected
alt::any_of(cond, false, true);

// To use cond as a transformer (applying it to other nullary callables),
// wrap it so it no longer satisfies bool_condition yourself, or redesign.
```

A lambda that takes parameters does *not* satisfy `bool_condition` and is always treated as a transformer:

```cpp
auto pred = [](int x) { return x > 0; };
alt::any_of(pred, -1, 2, 3);  // transformer overload — applies pred to each int
```

---

## Partial comparison functors

These functors capture a single value at construction and expose a call operator that compares an incoming argument against the stored value. They are designed to be passed directly to range adaptors and algorithms.

The value is **stored by value**. For reference semantics, wrap with `std::ref` or `std::cref`.

```cpp
std::vector<int> v{1, 2, 3, 2, 1};
auto twos = v | std::views::filter(alt::equals{2});        // {2, 2}
auto small = v | std::views::filter(alt::less{3});         // {1, 2, 2, 1, 1}
auto big   = v | std::views::filter(alt::greater_equal{3}); // {3}
```

The functors also compose naturally with the combinators above:

```cpp
alt::all_of(alt::greater{0}, 1, 2, 3);   // true
alt::any_of(alt::equals{9}, 1, 2, 3);    // false
```

### `equals`

Returns `true` when the argument compares equal (`==`) to the stored value.

```cpp
alt::equals{2}(2)   // true
alt::equals{2}(3)   // false
```

### `not_equals`

Returns `true` when the argument does not compare equal (`!=`) to the stored value.

```cpp
alt::not_equals{2}(3)   // true
alt::not_equals{2}(2)   // false
```

### `less`

Returns `true` when the argument is less than (`<`) the stored value.

```cpp
alt::less{5}(3)   // true  — 3 < 5
alt::less{5}(5)   // false — 5 is not < 5
alt::less{5}(7)   // false
```

### `greater`

Returns `true` when the argument is greater than (`>`) the stored value.

```cpp
alt::greater{5}(7)   // true  — 7 > 5
alt::greater{5}(5)   // false
alt::greater{5}(3)   // false
```

### `less_equal`

Returns `true` when the argument is less than or equal to (`<=`) the stored value.

```cpp
alt::less_equal{5}(3)   // true
alt::less_equal{5}(5)   // true
alt::less_equal{5}(7)   // false
```

### `greater_equal`

Returns `true` when the argument is greater than or equal to (`>=`) the stored value.

```cpp
alt::greater_equal{5}(5)   // true
alt::greater_equal{5}(7)   // true
alt::greater_equal{5}(3)   // false
```

---

## Short-circuit guarantees

| Combinator | Stops evaluating when… |
|---|---|
| `any_of` | First `true` found |
| `all_of` | First `false` found |
| `none_of` | First `true` found |
| `not_all_of` | First `false` found |
| `at_least<N>` | N-th `true` found |
| `at_most<N>` | (N+1)-th `true` found |
| `exactly<N>` | (N+1)-th `true` found |

Arguments to the left are evaluated before arguments to the right, in declaration order.

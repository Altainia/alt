# flags

**Header:** `<alt/flags.hpp>`

`alt::flags<Enum>` is a type-safe bitfield wrapper over a scoped enum. It stores a combination of enumerator bits as an unsigned integer while keeping all operations in terms of the enum type, preventing accidental mixing of unrelated enumerations or raw integers.

`Enum` must satisfy [`alt::scoped_enum`](concepts.md) (`enum class` or `enum struct`). All methods are `constexpr` and `noexcept`.

## Defining flag enumerators

Enumerators should represent individual bits. Use powers of two for the values:

```cpp
enum class Permission : uint8_t
{
    Read    = 0x01,
    Write   = 0x02,
    Execute = 0x04,
};

using Permissions = alt::flags<Permission>;
```

## Construction

```cpp
Permissions empty{};                               // all bits clear
Permissions r{Permission::Read};                   // single bit
Permissions rw = Permissions{Permission::Read}
               | Permissions{Permission::Write};   // two bits
```

## Bitwise operators

All return a new `flags` value.

```cpp
Permissions a{Permission::Read};
Permissions b{Permission::Write};

Permissions u = a | b;   // union:               Read | Write
Permissions i = a & b;   // intersection:        (empty)
Permissions x = a ^ b;   // symmetric difference: Read | Write
Permissions c = ~a;      // complement:           all bits except Read
```

In-place variants: `|=`, `&=`, `^=`.

## Mutation methods

```cpp
Permissions p{Permission::Read};

p.set(Permissions{Permission::Write});    // add Write
p.clear(Permissions{Permission::Read});   // remove Read
p.toggle(Permissions{Permission::Execute}); // flip Execute
```

`set`, `clear`, and `toggle` return `*this` for chaining.

## Query methods

```cpp
Permissions p = Permissions{Permission::Read} | Permissions{Permission::Write};

p.has_all(Permissions{Permission::Read} | Permissions{Permission::Write});  // true
p.has_all(Permissions{Permission::Execute});                                // false

p.has_any(Permissions{Permission::Write} | Permissions{Permission::Execute}); // true
p.has_none(Permissions{Permission::Execute});                               // true

p.empty();    // false
p.count();    // 2  (number of set bits)

p.matches(p); // true — same as operator==
```

`has_all` with an empty mask is vacuously `true`. `has_none` with an empty mask is vacuously `true`.

## Conversions

```cpp
Permissions p{Permission::Read};

if (p) { /* any bit is set */ }                       // explicit operator bool
auto raw = static_cast<Permissions::value_type>(p);  // unsigned integer
auto e   = static_cast<Permission>(p);               // enum (undefined if multiple bits set)
auto v   = p.value();                                 // same as value_type cast, non-explicit
```

`from_value` constructs a `flags` directly from a raw integer (useful when deserialising):

```cpp
Permissions p = Permissions::from_value(0x03u);  // Read | Write
```

## Equality

```cpp
Permissions a{Permission::Read};
Permissions b{Permission::Read};
assert(a == b);
```

## Full example

```cpp
#include <alt/flags.hpp>

enum class Option : uint16_t
{
    Verbose  = 0x0001,
    Dry      = 0x0002,
    Force    = 0x0004,
};

using Options = alt::flags<Option>;

void run(Options opts)
{
    if (opts.has_any(Options{Option::Verbose}))
        std::cout << "verbose mode\n";

    if (opts.has_all(Options{Option::Dry} | Options{Option::Force}))
        std::cout << "dry+force is unsupported\n";
}

int main()
{
    Options o = Options{Option::Verbose} | Options{Option::Dry};
    run(o);
}
```

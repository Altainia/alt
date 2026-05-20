# alt

A generic C++23 utility library. Namespace: `alt`.

## Requirements

- CMake 3.25 or later
- GCC 13+ or Clang 17+

## Build

### Debug build with tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Release build without tests

```bash
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DALT_BUILD_TESTS=OFF
cmake --build build/release
```

## Running the tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Installation

### System install (CMake)

The following installs headers, the static library, and CMake config files to `/usr/local`:

```bash
cmake -B build/local \
    -DCMAKE_BUILD_TYPE=Release \
    -DALT_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/local --parallel
sudo cmake --install build/local
```

Pass a different `--prefix` to install elsewhere.

### Debian package

Produces `altlib_0.1.0_amd64.deb` in the project root:

```bash
bash packaging/build-deb.sh
sudo dpkg -i altlib_0.1.0_amd64.deb
```

## Using in your project

After installing, other CMake projects can consume the library via `find_package`:

```cmake
find_package(Alt 0.1.0 REQUIRED)
target_link_libraries(my_target PRIVATE alt::alt)
```

Then include headers as needed:

```cpp
#include <alt/flags.hpp>
#include <alt/functional.hpp>
#include <alt/memory.hpp>
#include <alt/scope.hpp>
#include <alt/utility.hpp>
// etc.
```

## Components

| Header | Description | Reference |
|--------|-------------|-----------|
| `<alt/concepts.hpp>` | Concepts: `any_enum`, `scoped_enum`, `bool_condition` | [docs/concepts.md](docs/concepts.md) |
| `<alt/type_traits.hpp>` | `underlying_int<T>` trait and `underlying_int_t<T>` alias | [docs/type_traits.md](docs/type_traits.md) |
| `<alt/utility.hpp>` | `to_underlying()` — converts enums/integers to their underlying type | [docs/utility.md](docs/utility.md) |
| `<alt/functional.hpp>` | Boolean combinators: `any_of`, `all_of`, `none_of`, `not_all_of`, `at_least`, `at_most`, `exactly` | [docs/functional.md](docs/functional.md) |
| `<alt/flags.hpp>` | Type-safe bitfield wrapper over scoped enums | [docs/flags.md](docs/flags.md) |
| `<alt/memory.hpp>` | Secure memory erasure: `clear_memory`, `clearing_allocator`, and a `std::string` specialization | [docs/memory.md](docs/memory.md) |
| `<alt/scope.hpp>` | Scope guards and RAII resource wrapper: `scope_exit`, `scope_fail`, `scope_success`, `unique_resource` | [docs/scope.md](docs/scope.md) |

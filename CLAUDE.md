# alt

A generic C++23 utility library. Namespace: `alt`. Version: `1.2.0`.

## Directory layout

| Path | Purpose |
|------|---------|
| `include/alt/` | Public headers (header-only and compiled components) |
| `src/` | Compiled translation units |
| `tests/` | GoogleTest suite |
| `cmake/` | CMake helper modules and find_package template |
| `packaging/` | `.deb` build script and control file template |
| `scripts/` | Developer convenience scripts |

## Build

Prefer CMake presets for all local work:

```bash
# Debug build with tests (preferred)
cmake --preset debug && cmake --build --preset debug

# Release build without tests
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DALT_BUILD_TESTS=OFF
cmake --build build/release
```

Requires GCC 13+ or Clang 17+.

## Tests

```bash
ctest --preset debug --output-on-failure
```

## Quality checks

Run these before committing. All must pass with zero errors.

### AddressSanitizer + UBSan

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
```

### clang-tidy

Requires `clang-tidy` on `PATH`. Warnings are errors — the build fails on any finding.

```bash
cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build/tidy
```

### cppcheck

```bash
cmake -B build/check -DALT_CPPCHECK=ON
cmake --build build/check --target cppcheck
```

### Code coverage

Requires GCC and `lcov`. Report lands in `build/debug-coverage/coverage/index.html`.

```bash
cmake --preset debug-coverage && cmake --build --preset debug-coverage
ctest --preset debug-coverage --output-on-failure
cmake --build --preset debug-coverage --target coverage
```

## Local install

Installs headers, library, and CMake config files to `/usr/local`:

```bash
bash scripts/install-local.sh
```

Optional: pass a custom build directory as the first argument.

## Build .deb package

Produces `altlib_<version>_amd64.deb` in the project root:

```bash
bash packaging/build-deb.sh
```

Install with `sudo dpkg -i altlib_1.2.0_amd64.deb`.

## Consuming the library

After installing, other CMake projects can use:

```cmake
find_package(Alt 1.2.0 REQUIRED)
target_link_libraries(mytarget PRIVATE alt::alt)
```

## Coding guidelines

**Naming**
- Use `snake_case` for everything: variables, functions, types, files.
- Exception: template typenames use `PascalCase` (e.g. `template <typename ValueType>`).
- Private and protected member variables are prefixed with `m_` (e.g. `m_size`, `m_data`).

**Const by default** — declare variables `const` unless mutation is specifically required.

**Error handling** — prefer `std::expected` for recoverable errors over exceptions or raw error codes.

**Code quality** — prefer clean interfaces and readable code over brevity. Code should be self-explanatory.

**Doxygen**
- Add Doxygen `/** */` comments on all public functions, classes, structs, enums, and unions.
- When modifying a public symbol that already has a Doxygen comment, update the comment if the change affects its meaning.

**Tests** — be thorough; account for all edge cases, not just the happy path.

## Git workflow

- Use feature branches for new work (`feature/<name>`) and bug branches for fixes (`bug/<name>`).
- Commit once tests pass. No need to ask for permission to commit.
- Do not merge to `main` until explicitly asked to.

## Conventions

**Header-only component** — add a single `.hpp` to `include/alt/`.

**Compiled component** — add a `.hpp` to `include/alt/` (declaration) and a `.cpp` to `src/` (definition). The root `CMakeLists.txt` uses `GLOB_RECURSE` on `src/`, so new `.cpp` files are picked up automatically after re-running CMake configure.

**Tests** — add a `test_<component>.cpp` to `tests/` and register it in `tests/CMakeLists.txt` by adding it to the `alt_tests` sources list.

**Namespace** — all public symbols live in `namespace alt`. Avoid nested namespaces unless there is a clear reason.

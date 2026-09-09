# alt

A generic C++23 utility library. Namespace: `alt`. Version: `1.4.0`.

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

Requires GCC 13+ or Clang 19+.

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

Only analyzes `src/*.cpp`. Header-only components are not covered.

### clang-format

The codebase conforms to `.clang-format`. Run it on changed files before committing.

```bash
clang-format -i <changed files>
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
cmake -B build/local -DCMAKE_BUILD_TYPE=Release \
    -DALT_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/local --parallel
sudo cmake --install build/local
```

Pass a different `--prefix` to install elsewhere.

## Build .deb package

Produces `altlib_<version>_amd64.deb` in the project root:

```bash
bash packaging/build-deb.sh
```

Install with `sudo dpkg -i altlib_1.4.0_amd64.deb`.

## Consuming the library

After installing, other CMake projects can use:

```cmake
find_package(Alt 1.4.0 REQUIRED)
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

All work reaches `main` through a pull request. `main` is never pushed to directly.

1. Branch: `feature/<name>` for new work, `bug/<name>` for fixes.
2. Commit once tests pass. No need to ask for permission to commit. CI runs on every branch push, so a broken branch shows up before the pull request exists.
3. Open a pull request against `main` once the work is ready.
4. Add the version bump as its own commit on the branch. See below.
5. Merge with `gh pr merge --merge` after CI is green.

Do not merge to `main` until explicitly asked to.

### Version bump

The bump is a standalone commit on the pull request branch, never bundled into a
feature or merge commit. Commit message: `Bump version to x.y.z`. Bumping on the
branch means CI verifies the new version before it reaches `main`.

If a version bump has not been mentioned, ask whether to bump and which part to
increase. All six of these files change together:

| File | What to change |
|------|---------------|
| `CMakeLists.txt` | `project(Alt VERSION x.y.z ...)` |
| `include/alt/version.hpp` | `ALT_VERSION_MAJOR/MINOR/PATCH` macros and constexpr variables |
| `src/version.cpp` | String returned by `version()` |
| `tests/test_version.cpp` | Expected major/minor/patch values and the version string |
| `README.md` | All occurrences of the version string |
| `CLAUDE.md` | All occurrences of the version string |

## Conventions

**Header-only component** — add a single `.hpp` to `include/alt/`.

**Helper-only header** — a header no caller would include without also including a public component belongs in `include/alt/detail/`. Public headers stay directly in `include/alt/`. The subdirectory describes includability, not visibility: those symbols are still public API in `namespace alt`, with only engine internals in `alt::detail`. `install(DIRECTORY include/alt)` is recursive, so a new subdirectory needs no CMake change.

**Compiled component** — add a `.hpp` to `include/alt/` (declaration) and a `.cpp` to `src/` (definition). The root `CMakeLists.txt` uses `GLOB_RECURSE` on `src/`, so new `.cpp` files are picked up automatically after re-running CMake configure.

**Tests** — add a `test_<component>.cpp` to `tests/` and register it in `tests/CMakeLists.txt` by adding it to the `alt_tests` sources list.

**Namespace** — all public symbols live in `namespace alt`. Avoid nested namespaces unless there is a clear reason.

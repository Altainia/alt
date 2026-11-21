# alt

A generic C++23 utility library. Namespace: `alt`. Version: `1.16.0`.

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

Requires GCC 13+, Clang 19+, or MSVC 19.50+ (Visual Studio 2026). The
same presets work on Windows — see "MSVC" under Quality checks.

## Tests

```bash
ctest --preset debug --output-on-failure
```

## Quality checks

Run these before committing. All must pass with zero errors.

CI runs **eleven** jobs on every pull request and on every push to `main`, and every
one of them corresponds to a check below: `Build & Test (GCC)`, `Build & Test (Clang)`,
`Build & Test (optimized)`, `AddressSanitizer + UBSan`, `ThreadSanitizer`,
`clang-tidy`, `cppcheck`, `Coverage`, and the three `Build & Test (MSVC, <preset>)`
jobs for `debug`, `release-tests` and `debug-asan`. A local sweep that skips one is not a
green branch, however it reports itself — a sweep that omitted `Coverage` and a
sweep that ran a newer `clang-tidy` than CI's have each produced a red build after
reporting clean.

### AddressSanitizer + UBSan

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
```

### ThreadSanitizer

The coroutine components are the only concurrent code in the library, so this is
the build that covers them. AddressSanitizer does not find data races.

```bash
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure
```

### Optimized test run

One property cannot be checked in a debug build: `task<T>`'s constant stack use
for deep chains needs the tail call GCC only emits from `-O2` upward, so the test
for it skips in every `Debug` preset. This preset is where it actually runs.

```bash
cmake --preset release-tests && cmake --build --preset release-tests
ctest --preset release-tests --output-on-failure
```

### MSVC

Windows only. CMake's default generator there is the newest installed Visual Studio,
which is multi-config and needs no Developer Command Prompt, so the presets run
unchanged; the build and test presets carry a `configuration` for that reason. CI runs
these three, with `-DALT_WARNINGS_AS_ERRORS=ON`:

```powershell
foreach ($p in "debug", "release-tests", "debug-asan") {
    cmake --preset $p -DALT_WARNINGS_AS_ERRORS=ON; cmake --build --preset $p --parallel
    ctest --preset $p --output-on-failure
}
```

All three are needed, because each caught something the others did not. With MSVC,
`debug-asan` is AddressSanitizer alone — MSVC has no UBSan — applied build-wide rather
than per target, because MSVC's STL refuses to link instrumented and uninstrumented
translation units together. `debug-tsan` is refused at configure.

Libraries differ in what they leave unspecified, and libstdc++ has hidden real defects
here. Its `std::stop_source::request_stop()` is `const` where the standard's is not, and
it skips relocking the callback list after the last callback where MSVC's does not —
which is what kept a use-after-free in `detail::stop_forwarder` invisible on Linux,
AddressSanitizer included. A cancellation test that hangs only on MSVC is that class of
bug, not a flaky test.

That early return hides the defect only while the abandoning callback is the sole
registration on the token. A second one on the same token leaves the walk somewhere to
go, and then Linux sees it too:
`CancellationRefTest.ATeardownWithASecondCallbackOnTheChainsTokenIsSafe` is built that
way on purpose, and with the keepalive removed it fails every Linux preset — an
AddressSanitizer `heap-use-after-free` under `debug-asan`, and 0 passes in 40 runs of
plain `debug` (26 segfaults, 14 hangs). So reach for that shape when a defect in this
area looks MSVC-only; it is usually reproducible here. Both libraries walk the list in
reverse registration order, which is what makes the arrangement portable — MSVC's STL
attaches at the head and pops the head in `_Request_stop`.

### clang-tidy

Warnings are errors — the build fails on any finding.

**CI uses `clang-tidy-19`, not whatever `clang-tidy` resolves to on `PATH`.** The
findings differ between versions: `bugprone-crtp-constructor-accessibility` reports a
public *deleted* copy constructor in 19 and no longer does in 21. Pin the tool and the
compiler to CI's, or the gate you run is not the gate that will fail:

```bash
cmake -B build/tidy -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCLANG_TIDY_PATH=/usr/bin/clang-tidy-19 -DCMAKE_BUILD_TYPE=Debug \
    -DALT_CLANG_TIDY=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/tidy
```

That is CI's own invocation, flag for flag — see the `clang-tidy` job in
`.github/workflows/ci.yml`. `CMAKE_BUILD_TYPE` is part of it: findings in
assert-dependent code change with the build type.

The whole-project gate compiles every test into one target, so a finding in an early
file aborts the build before later files are analyzed. To check one file you changed,
run `clang-tidy-19` on it directly and pass the GoogleTest headers with `-isystem`
rather than `-I`, which otherwise reports spurious findings from GoogleTest's own
macros. `tests/.clang-tidy` carries the suite's suppression list — append to it, never
rewrite it.

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

**This is a CI gate, not an optional report.** The `Coverage` job fails the build if
`lcov` cannot produce the report, so run it before opening a pull request.

Requires GCC and `lcov`. Report lands in `build/debug-coverage/coverage/index.html`.

```bash
cmake --preset debug-coverage && cmake --build --preset debug-coverage
cmake --build --preset debug-coverage --target coverage
```

There is deliberately no `ctest` step. The `coverage` target zeroes the counters and
then runs the whole suite itself, with `--output-on-failure`, so a run of your own ahead
of it buys no extra signal and costs a second pass over all the tests. See
`cmake/coverage.cmake`, where the `lcov --zerocounters` and `ctest` commands sit in that
order.

A separate run is also the one test run in the flow that no zeroing precedes, so against
a reused build directory it reports `libgcov profiling error: ... overwriting an existing
profile data with a different checksum` for every translation unit whose code changed
since that directory was last populated. Those lines are harmless -- the report is built
from the target's own run, not from that data -- but they read like a broken gate, and
diagnosing them cost a full investigation once. Issue #32 carries the measurements.

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

Install with `sudo dpkg -i altlib_1.16.0_amd64.deb`.

## Consuming the library

After installing, other CMake projects can use:

```cmake
find_package(Alt 1.16.0 REQUIRED)
target_link_libraries(mytarget PRIVATE alt::alt)
```

Headers and the installed library must come from the same minor release: the inline
sizes of the async primitives (`async_event`, `async_mutex`, `task_group`, ...) can
change between minors. The package config enforces this: `find_package(Alt 1.16.0)`
accepts 1.16.0 and later 1.16.x releases, but rejects 1.17.0 and 1.15.x, and a request for
a higher patch than the installed one (1.16.1 against 1.16.0) also fails. When upgrading
across a minor release, update the version your project requests.

A refused request prints which minor series the installed package accepts and why, so a
deliberate refusal is distinguishable from a broken install.

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
2. Commit once tests pass. No need to ask for permission to commit. A bare branch push does not
   run CI — only pull requests and `main` do. To see a branch go green before the pull request
   is ready, open it as a draft (a draft PR fires the full eleven jobs) or run the suite on
   demand with `gh workflow run CI --ref <branch>`.
3. Open a pull request against `main` once the work is ready.
4. Add the version bump as its own commit on the branch. See below.
5. Merge with `gh pr merge --merge` after CI is green.

Do not merge to `main` until explicitly asked to.

### Deferrals

A finding deferred rather than fixed is filed as a GitHub issue with `gh issue create`,
never left only in a plan ledger or a review report: those live in the plan workspace,
which is deleted when the branch finishes. PR #19's deferred-minors list went with it and
could not be recovered. Reference the issue number wherever the deferral is recorded, and
re-verify a deferred item against the code before acting on it later — most of PR #19's
recoverable ones had already been fixed by the work that followed.

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
| `tests/test_version.cpp` | Expected major/minor/patch values and the version string. **Three places, not two:** the `EXPECT_EQ` macros, the `static_assert`s on `alt::version_patch` and friends, and the string. The `static_assert` spells its number bare, so a grep for the old version string misses it and the build — not `ctest` — is what reports it |
| `README.md` | Project version occurrences. The `find_package` compatibility example's operands illustrate the rule, not this release: adjust them so the prose still agrees with the snippet above it |
| `CLAUDE.md` | Same as `README.md`: project version occurrences move; keep the compatibility example consistent with the `find_package` snippet above it |

## Conventions

**Header-only component** — add a single `.hpp` to `include/alt/`.

**Helper-only header** — a header no caller would include without also including a public component belongs in `include/alt/detail/`. Public headers stay directly in `include/alt/`. The subdirectory describes includability, not visibility: those symbols are still public API in `namespace alt`, with only engine internals in `alt::detail`. `install(DIRECTORY include/alt)` is recursive, so a new subdirectory needs no CMake change.

**Compiled component** — add a `.hpp` to `include/alt/` (declaration) and a `.cpp` to `src/` (definition). The root `CMakeLists.txt` uses `GLOB_RECURSE` on `src/`, so new `.cpp` files are picked up automatically after re-running CMake configure.

**Tests** — add a `test_<component>.cpp` to `tests/` and register it in `tests/CMakeLists.txt` by adding it to the `alt_tests` sources list.

**Namespace** — all public symbols live in `namespace alt`. Avoid nested namespaces unless there is a clear reason.

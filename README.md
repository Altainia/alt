# alt

A generic C++23 utility library. Namespace: `alt`.

## Requirements

- CMake 3.25 or later
- GCC 13+, Clang 19+, or MSVC 19.50+ (Visual Studio 2026)
- Linux only: liburing 2.1 or later and `pkg-config` (`liburing-dev` on Debian and Ubuntu)

## Build

### Using CMake presets (recommended)

The project ships with named presets. Configure, build, and test in one line:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Available presets:

| Preset | Compiler | Sanitizers | Notes |
|--------|----------|------------|-------|
| `debug` | default | none | Standard debug build |
| `debug-asan` | default | ASan + UBSan | AddressSanitizer + UndefinedBehaviorSanitizer |
| `debug-tsan` | default | TSan | ThreadSanitizer, for the coroutine components. Mutually exclusive with ASan |
| `debug-msan` | Clang | MSan | Requires MSan-instrumented libc++ — see note below |
| `debug-coverage` | GCC | none | gcov instrumentation for lcov reports |
| `release` | default | none | Optimized release build, no tests |
| `release-tests` | default | none | Optimized build that still builds the tests. The only preset in which the deep-chain task test runs on GCC |

> **MSan note:** MemorySanitizer requires a fully instrumented C++ standard library.
> The `debug-msan` preset is provided for use with a custom MSan-instrumented
> libc++ build. It will not work correctly with the system libstdc++.

### Windows (MSVC)

The same presets work on Windows. CMake's default generator there is the newest
installed Visual Studio, and no Developer Command Prompt is needed:

```powershell
cmake --preset debug; cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`debug`, `release`, `release-tests` and `debug-asan` are supported with MSVC. MSVC has
AddressSanitizer but no UndefinedBehaviorSanitizer, so `debug-asan` runs ASan alone
there. `debug-tsan` stops at configure with an error naming the reason, and
`debug-msan` and `debug-coverage` are not MSVC presets: they select Clang and GCC
themselves. The build copies the ASan runtime DLL next to the test
executable, so the tests run from any shell.

### Manual build

```bash
# Debug build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Release build without tests
cmake -B build/release -DCMAKE_BUILD_TYPE=Release -DALT_BUILD_TESTS=OFF
cmake --build build/release
```

## Running the tests

```bash
ctest --preset debug --output-on-failure
```

Or without presets:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Quality checks

### Sanitizers

Run the test suite under AddressSanitizer and UndefinedBehaviorSanitizer (AddressSanitizer
alone with MSVC):

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
```

### Static analysis — clang-tidy

Runs clang-tidy on every compiled translation unit (library and tests).
Requires `clang-tidy` on `PATH`.

```bash
cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build/tidy
```

### Static analysis — cppcheck

```bash
cmake -B build/check -DALT_CPPCHECK=ON
cmake --build build/check --target cppcheck
```

### Code coverage

Requires GCC and `lcov`/`genhtml`. Produces an HTML report in `build/debug-coverage/coverage/`.

```bash
cmake --preset debug-coverage && cmake --build --preset debug-coverage
cmake --build --preset debug-coverage --target coverage
xdg-open build/debug-coverage/coverage/index.html
```

The `coverage` target zeroes the counters and runs the suite itself, so there is no
separate `ctest` step.

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

Produces `altlib_1.17.0_amd64.deb` in the project root:

```bash
bash packaging/build-deb.sh
sudo dpkg -i altlib_1.17.0_amd64.deb
```

## Using in your project

After installing, other CMake projects can consume the library via `find_package`:

```cmake
find_package(Alt 1.17.0 REQUIRED)
target_link_libraries(my_target PRIVATE alt::alt)
```

Headers and the installed library must come from the same minor release, because the
inline sizes of the async primitives can change between minors. The package config
enforces this: requesting `1.17.0` accepts 1.17.0 and later 1.17.x releases, but rejects
1.18.0 and 1.16.x, and a request for a higher patch than the installed one (1.17.1
against 1.17.0) also fails. Update the requested version when you upgrade across a minor
release.

A refused request prints which minor series the installed package accepts and why, so a
deliberate refusal is distinguishable from a broken install.

On Linux a consumer needs `liburing-dev` and `pkg-config` installed too, not only the
machine that built alt. alt is a static library, so it passes its liburing link
dependency on to your target, and the package config rediscovers it through
`pkg-config` when `find_package(Alt)` runs.

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
| `<alt/concepts.hpp>` | Concepts: `any_enum`, `scoped_enum`, `flag_enum`, `bool_condition`, `byte_range` | [docs/concepts.md](docs/concepts.md) |
| `<alt/type_traits.hpp>` | `underlying_int<T>` trait and `underlying_int_t<T>` alias | [docs/type_traits.md](docs/type_traits.md) |
| `<alt/utility.hpp>` | `to_underlying()` — converts enums/integers to their underlying type | [docs/utility.md](docs/utility.md) |
| `<alt/functional.hpp>` | Boolean combinators: `any_of`, `all_of`, `none_of`, `not_all_of`, `at_least`, `at_most`, `exactly` | [docs/functional.md](docs/functional.md) |
| `<alt/algorithm.hpp>` | Container erasure with projections: `erase`, `erase_if` | [docs/algorithm.md](docs/algorithm.md) |
| `<alt/flags.hpp>` | Type-safe bitfield wrapper over enumerations | [docs/flags.md](docs/flags.md) |
| `<alt/memory.hpp>` | Secure memory erasure: `clear_memory`, `clearing_allocator`, and a `std::string` specialization | [docs/memory.md](docs/memory.md) |
| `<alt/projection.hpp>` | Projection objects for composite values: `key`, `value`, `element<N>` | [docs/projection.md](docs/projection.md) |
| `<alt/scope.hpp>` | Scope guards and RAII resource wrapper: `scope_exit`, `scope_fail`, `scope_success`, `unique_resource` | [docs/scope.md](docs/scope.md) |
| `<alt/transcode.hpp>` | Lazy UTF-8/16/32 conversion: `transcode`, `encode_one`, `decode_one` | [docs/transcode.md](docs/transcode.md) |
| `<alt/base64.hpp>` | Base 64 encoding and decoding (RFC 4648): `base64_encode`, `base64_decode`, alphabets, and lazy views | [docs/base64.md](docs/base64.md) |
| `<alt/sha1.hpp>` | SHA-1 hashing (deprecated; FIPS 180-4): `sha1`, `sha1_hasher`, `sha1_digest` | [docs/sha1.md](docs/sha1.md) |
| `<alt/sha2.hpp>` | SHA-2 hashing (FIPS 180-4): `sha224`, `sha256`, `sha384`, `sha512`, `sha512_224`, `sha512_256` | [docs/sha2.md](docs/sha2.md) |
| `<alt/awaitable.hpp>` | Awaitable concepts and traits: `awaiter`, `awaitable`, `awaitable_of`, `await_result_t`, `get_awaiter`, `as_expected` | [docs/awaitable.md](docs/awaitable.md) |
| `<alt/task.hpp>` | Lazy, move-only coroutine result: `task<T>` | [docs/task.md](docs/task.md) |
| `<alt/scheduler.hpp>` | Scheduling primitive and schedulers: `work_item`, `scheduler`, `drivable_scheduler`, `scheduler_ref`, `trampoline_scheduler`, `manual_scheduler`, `resume_on` | [docs/scheduler.md](docs/scheduler.md) |
| `<alt/run_loop.hpp>` | Serial executor for an owned thread or a platform loop: `run_loop`, `run_loop_options` | [docs/run_loop.md](docs/run_loop.md) |
| `<alt/thread_pool.hpp>` | Fixed-size thread pool built by a factory returning `std::expected`: `thread_pool` | [docs/thread_pool.md](docs/thread_pool.md) |
| `<alt/sync_wait.hpp>` | Blocking and driving entry points for running an awaitable to completion: `sync_wait` | [docs/sync_wait.md](docs/sync_wait.md) |
| `<alt/cancellable_task.hpp>` | Self-owning, cancellable root coroutine: `cancellable_task`, `cancellation_handle`, `completion_handler`, `start`, `cancellation_point` | [docs/cancellable_task.md](docs/cancellable_task.md) |
| `<alt/cancellation_ref.hpp>` | Non-owning view of an await chain's cancellation state, for a hand-written awaiter: `cancellation_ref`, `get_cancellation_ref` | [docs/cancellation_ref.md](docs/cancellation_ref.md) |
| `<alt/async_event.hpp>` | Manual-reset signal coroutines await: `async_event`, `wait_result` | [docs/async_event.md](docs/async_event.md) |
| `<alt/async_latch.hpp>` | Single-use countdown coroutines await: `async_latch`, `wait_result` | [docs/async_latch.md](docs/async_latch.md) |
| `<alt/async_semaphore.hpp>` | Counting semaphore coroutines await: `async_semaphore`, `wait_result` | [docs/async_semaphore.md](docs/async_semaphore.md) |
| `<alt/async_mutex.hpp>` | Mutual exclusion that blocks no thread, with an RAII guard: `async_mutex`, `async_mutex_lock` | [docs/async_mutex.md](docs/async_mutex.md) |
| `<alt/task_completion_source.hpp>` | The bridge from a callback to an await: `task_completion_source`, `completion_result` | [docs/task_completion_source.md](docs/task_completion_source.md) |
| `<alt/task_group.hpp>` | The owner's teardown point for spawned tasks: `task_group` | [docs/task_group.md](docs/task_group.md) |
| `<alt/when_all.hpp>` | Awaits several awaitables concurrently and yields every result: `when_all` | [docs/when_all.md](docs/when_all.md) |
| `<alt/when_any.hpp>` | Races several awaitables and yields the first to complete: `when_any` | [docs/when_any.md](docs/when_any.md) |
| `<alt/io_buffer.hpp>` | Refcounted byte buffer that outlives an abandoned operation: `io_buffer` | [docs/io_buffer.md](docs/io_buffer.md) |
| `<alt/io_backend.hpp>` | The seam between the file layer and an I/O engine, with a hand-driven test backend and a thread-pool backend: `io_backend`, `io_request`, `io_completion`, `io_token`, `manual_io_backend`, `make_thread_pool_backend` | [docs/io_backend.md](docs/io_backend.md) |
| `<alt/file_io.hpp>` | Service for awaitable file I/O with backpressure: `file_io`, `file_io_options`; `open`, `stat`, `rename`, `unlink`, `list_directory` and the `run` escape hatch | [docs/file_io.md](docs/file_io.md) |
| `<alt/async_file.hpp>` | An open file with awaitable positional operations: `async_file`, `open_flags`, `sync_mode` | [docs/async_file.md](docs/async_file.md) |

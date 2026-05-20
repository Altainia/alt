# scope

**Header:** `<alt/scope.hpp>`

RAII scope guards and a generic resource wrapper. Implements [P0052r10](https://wg21.link/p0052r10) "Generic Scope Guard and RAII Wrapper for the Standard Library".

---

## Scope guards

All three scope guard types share the same basic shape:

- Constructed with a callable (the *exit function*).
- The exit function is called on destruction, subject to the guard's policy.
- `release()` disables the call — useful when the guarded operation succeeded and cleanup is no longer needed.
- Copy construction, copy assignment, and move assignment are deleted.
- Move construction transfers ownership and calls `release()` on the source.

### `scope_exit<EF>`

Calls the exit function **unconditionally** on destruction, unless `release()` has been called.

```cpp
#include <alt/scope.hpp>

{
    alt::scope_exit guard{[] { std::cout << "cleanup\n"; }};
    // ... do work ...
}  // prints "cleanup" whether or not an exception was thrown
```

Disable cleanup when work succeeds:

```cpp
alt::scope_exit guard{[] { rollback(); }};
do_work();      // if this throws, rollback() runs on unwind
guard.release(); // success — no rollback needed
```

### `scope_fail<EF>`

Calls the exit function **only when the scope exits via an exception** (i.e., `std::uncaught_exceptions()` is greater at destruction than it was at construction).

```cpp
alt::scope_fail on_fail{[] { log("operation failed"); }};
risky_operation();  // if this throws, the log runs; on normal exit it does not
```

### `scope_success<EF>`

Calls the exit function **only when the scope exits normally** (no new exception active).

```cpp
alt::scope_success on_ok{[] { commit(); }};
prepare();      // if this throws, commit() is not called
finalize();     // if this throws, commit() is not called
// on_ok destructs normally — commit() runs
```

Note: unlike `scope_exit` and `scope_fail`, if constructing the exit function throws, `scope_success` does *not* call the function before rethrowing.

### Common interface

```cpp
template<typename EF>
class scope_exit   { /* scope_fail, scope_success same shape */ };

explicit scope_exit(EF&& f);    // construct with exit function
scope_exit(scope_exit&& rhs);   // move — releases rhs
void release() noexcept;        // disable the exit call
~scope_exit() noexcept;         // calls exit function per policy
```

CTAD (class template argument deduction) is supported:

```cpp
alt::scope_exit guard{[] { cleanup(); }};  // no need to write scope_exit<decltype(...)>
```

---

## `unique_resource<R, D>`

A universal RAII wrapper that ties one resource handle to a cleanup function (deleter). Analogous to `std::unique_ptr` but works with any resource type, not just pointers.

```cpp
template<typename R, typename D>
class unique_resource;
```

| Template parameter | Description |
|---|---|
| `R` | Resource type. May be a value type or an lvalue reference type. References are stored via `std::reference_wrapper`. |
| `D` | Deleter type. Must be callable as `d(resource)`. |

### Construction

```cpp
// From resource and deleter — execute_on_reset is true
alt::unique_resource res{open_handle(), [](Handle h) { close_handle(h); }};

// Default (R and D must be default-constructible) — execute_on_reset is false
alt::unique_resource<Handle, Deleter> res{};
```

Both resource and deleter initialization are exception-safe: if either constructor throws, the deleter is invoked on the resource before rethrowing to prevent a resource leak.

### Member functions

```cpp
res.get();          // returns the resource
*res;               // dereferences the resource (pointer types only)
res->;              // member access (pointer types only)
res.get_deleter();  // returns const ref to the deleter

res.reset();        // calls deleter if active, then marks as released
res.reset(new_r);   // calls deleter on current, adopts new_r — strong exception guarantee
res.release();      // disables cleanup — caller takes ownership
```

### Move semantics

`unique_resource` is movable but not copyable. Moving transfers ownership and marks the source as released.

```cpp
auto a = alt::unique_resource{open_file("f.txt"), &fclose};
auto b = std::move(a);   // b owns the file; a no longer calls fclose
```

---

## `make_unique_resource_checked`

```cpp
template<typename R, typename D, typename S = std::decay_t<R>>
unique_resource<decay_t<R>, decay_t<D>>
    make_unique_resource_checked(R&& resource, const S& invalid, D&& d) noexcept(...);
```

Factory function for resources that have a sentinel "invalid" value (such as a null pointer or `-1` file descriptor). If `resource == invalid`, the returned `unique_resource` has its cleanup disabled — the deleter is never called on an invalid handle.

```cpp
// fopen returns nullptr on failure
auto file = alt::make_unique_resource_checked(
    std::fopen("data.txt", "r"),
    static_cast<FILE*>(nullptr),
    &std::fclose
);

if (file.get() != nullptr)
{
    // file is valid and will be fclose'd on destruction
}
// if fopen returned nullptr, fclose is never called
```

---

## Complete example

```cpp
#include <alt/scope.hpp>
#include <cstdio>

void process_file(const char* path)
{
    auto file = alt::make_unique_resource_checked(
        std::fopen(path, "r"),
        static_cast<FILE*>(nullptr),
        &std::fclose
    );

    if (!file.get())
        throw std::runtime_error{"cannot open file"};

    alt::scope_fail log_on_error{[&] {
        std::fprintf(stderr, "error while processing %s\n", path);
    }};

    // read and process file ...

    // log_on_error runs only if an exception escapes
    // file is always fclose'd (if valid)
}
```

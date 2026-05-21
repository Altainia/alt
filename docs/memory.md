# memory

**Header:** `<alt/memory.hpp>`

Secure memory handling. Provides a guaranteed-erasure memory-zero function, an allocator adaptor that zeroes memory before releasing it, and a `std::basic_string` partial specialization that closes the SSO (short-string optimisation) security gap.

---

## `clear_memory`

```cpp
namespace alt {
    void clear_memory(void* p, std::size_t n) noexcept;
}
```

Zeroes `n` bytes starting at `p` in a way that the compiler will not optimize away. A plain `memset` can be eliminated by the optimizer when the memory is never read again; `clear_memory` prevents this.

- `n == 0` is a no-op. `p` may be null when `n == 0`.
- Uses the best platform primitive available: `SecureZeroMemory` on Windows, `explicit_bzero` on glibc 2.25+/macOS, or a volatile loop fallback.

```cpp
char buf[64];
// ... fill buf with a password ...
alt::clear_memory(buf, sizeof(buf));  // guaranteed zero before buf leaves scope
```

---

## `clearing_allocator<T, Alloc>`

```cpp
namespace alt {
    template<typename T, typename Alloc = std::allocator<T>>
    class clearing_allocator;
}
```

A C++ *Allocator* conforming adaptor. Forwards `allocate` to the underlying `Alloc` unchanged, but calls `clear_memory` on the block before forwarding `deallocate`. This ensures any heap memory returned to the system has been zeroed first.

### Template parameters

| Parameter | Description |
|-----------|-------------|
| `T` | The value type to allocate. |
| `Alloc` | The underlying allocator. Defaults to `std::allocator<T>`. |

### Member types

- `value_type`, `allocator_type`
- `propagate_on_container_copy_assignment`, `propagate_on_container_move_assignment`, `propagate_on_container_swap`, `is_always_equal` — forwarded from `Alloc`
- `rebind<U>::other` — `clearing_allocator<U, rebind_alloc<U>>`

### Constructors

```cpp
clearing_allocator();                       // default-constructs Alloc
clearing_allocator(Alloc const& alloc);     // copies Alloc
clearing_allocator(Alloc&& alloc);          // moves Alloc
// Converting constructor for rebound types (used internally by containers)
```

### Key methods

```cpp
T*   allocate(std::size_t n);                    // delegates to Alloc
void deallocate(T* p, std::size_t n) noexcept;   // clear_memory then delegates
Alloc& underlying() noexcept;                    // access the wrapped allocator
```

### Using with standard containers

Any standard container that accepts an allocator can be given `clearing_allocator` to ensure heap memory is zeroed on deallocation:

```cpp
#include <alt/memory.hpp>
#include <vector>

using SecureVec = std::vector<char, alt::clearing_allocator<char>>;

SecureVec password(32);
// fill password ...
// on destruction, the heap block is zeroed before being returned to the OS
```

---

## Secure string: `std::basic_string` partial specialization

When `<alt/memory.hpp>` is included, the following partial specialization becomes available:

```cpp
std::basic_string<CharT, Traits, alt::clearing_allocator<CharT, Alloc>>
```

This provides the full `std::string` interface with one additional guarantee: **character data is zeroed via `clear_memory` whenever the string is destroyed or moved from**.

Convenience aliases:

```cpp
using secure_string    = std::basic_string<char,    std::char_traits<char>,
                                           alt::clearing_allocator<char>>;
using secure_wstring   = std::basic_string<wchar_t, std::char_traits<wchar_t>,
                                           alt::clearing_allocator<wchar_t>>;
```

(These aliases are not defined by the library; define them yourself as needed.)

### Security guarantees

| Scenario | Guarantee |
|----------|-----------|
| Heap-allocated string destroyed | `clearing_allocator::deallocate` zeroes the heap block |
| SSO string destroyed | Destructor calls `clear_memory` on the internal buffer |
| String moved from | Source's SSO buffer is zeroed immediately after the move |
| Heap string destroyed | `clear_memory` is called on the used portion before deallocate |

The specialization handles the SSO case explicitly because the SSO buffer lives inside the string object itself — when the string is short enough to fit in the inline buffer, no heap allocation is made and `clearing_allocator` never fires. The specialization's destructor closes this gap.

### Example

```cpp
#include <alt/memory.hpp>
#include <string>

int main()
{
    std::basic_string<char, std::char_traits<char>,
                      alt::clearing_allocator<char>> password = "hunter2";

    // use password ...

    // On destruction:
    //   - SSO buffer (or heap block) is zeroed via clear_memory
    //   - For heap strings, clearing_allocator::deallocate also zeroes before releasing
}
```

### API notes

The specialization exposes the same interface as `std::string`. A few members are re-implemented to preserve the specialization type:

- `get_allocator()` returns `alt::clearing_allocator<CharT, Alloc>`.
- `substr()` returns an instance of the specialization (not a plain `std::basic_string`).
- `swap()` accepts only another instance of the same specialization.

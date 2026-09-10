# alt Coroutines: `task<T>` and `cancellable_task` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a generic coroutine `task<T>` with its supporting machinery, plus `cancellable_task`, a self-owning root coroutine that destroys its entire await chain at the next suspension point once cancellation is requested.

**Architecture:** Five layers, each usable without the ones above it: an awaitable concepts layer, a scheduler layer whose single primitive is `post(work_item)`, a cancellation layer holding the context and the checking awaiter, `task<T>` for lazy child coroutines, and `cancellable_task` as the root that owns the cancellation state. Cancellation is checked on entry to every suspension point, before that suspension point starts anything, so nothing is ever in flight when frames are destroyed.

**Tech Stack:** C++23, header-only, GoogleTest, CMake presets. No third-party dependencies. `std::stop_token`, `std::expected`, `std::coroutine_handle`, `std::binary_semaphore`.

**Spec:** `docs/superpowers/specs/2026-09-10-coroutine-task-and-cancellation-design.md`

**Roadmap:** `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md`

## Global Constraints

- **Branch:** `feature/coroutines`. Never push to `main`. Work reaches `main` through a pull request only.
- **Language:** C++23. Compilers: GCC 13 or later, Clang 19 or later. Do not use anything outside that intersection.
- **Namespace:** all public symbols in `namespace alt`. Engine internals in `namespace alt::detail`.
- **Naming:** `snake_case` for variables, functions, types, and files. Template typenames use `PascalCase`. Private and protected member variables are prefixed `m_`.
- **Const by default:** declare variables `const` unless mutation is required.
- **Doxygen:** `/** */` comments on every public function, class, struct, concept, and enum.
- **Spelling:** American English, with one confirmed exception: the double-l forms `cancelled`, `cancelling`, `cancellable`, and `cancellation` are correct for this project and must not be "corrected".
- **Formatting:** run `clang-format -i` on every changed file before committing. The config uses tabs for indentation, `IndentWidth: 2`, Allman braces, `NamespaceIndentation: All`, and no column limit.
- **Header placement:** public headers go directly in `include/alt/`. A header no caller would include without also including a public component goes in `include/alt/detail/`. Both are installed; the subdirectory describes includability, not visibility.
- **Tests:** every test file must be added to the `alt_tests` sources list in `tests/CMakeLists.txt`. Be thorough; cover edge cases, not just the happy path.
- **Shared test helpers:** the coroutine test helpers live in one header, `tests/coroutine_test_support.hpp`, created in Task 4 and included by Tasks 5 and 7. Task bodies below show those helpers inline for readability; do not redefine them per test file. This follows the existing `tests/sha_test_support.hpp` precedent.
- **Version:** the bump to `1.6.0` is Task 9, a standalone commit, never bundled into a feature commit.
- **No CMake change needed for headers:** `install(DIRECTORY include/alt)` is recursive and the `src/` glob finds nothing new. All seven headers are header-only.

## Verification commands

Run the first two after every task. Run all five before opening the pull request.

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure

cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
```

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `include/alt/awaitable.hpp` | Concepts, `await_result_t`, `get_awaiter`, `as_expected`. No dependency on any other new header. | 1 |
| `include/alt/detail/coroutine.hpp` | `result_store<T>`: holds a value, a reference, `void`, or an exception. Nothing else. | 2 |
| `include/alt/scheduler.hpp` | `work_item`, the `scheduler` and `drivable_scheduler` concepts, `scheduler_ref`, `trampoline_scheduler`, `manual_scheduler`, `resume_on`. | 3 |
| `include/alt/detail/cancellation.hpp` | `cancellation_context` and `cancellation_checked<Inner>`. The single delicate function lives here. | 4 |
| `include/alt/task.hpp` | `task<T>`, its promise, its awaiter, the context relay, and `await_transform`. | 5 |
| `include/alt/sync_wait.hpp` | Both `sync_wait` overloads and the internal runner coroutine. | 6 |
| `include/alt/cancellable_task.hpp` | `cancellable_task`, `cancellation_handle`, `exception_handler`, `start_options`, `start`, `cancellation_point`. | 7 |

Task order is dependency order. `detail/cancellation.hpp` precedes `task.hpp` because the task promise's `await_transform` returns the checking awaiter, and it precedes nothing else, so it can be tested against a root coroutine defined inside its own test file.

---

### Task 1: The awaitable layer

**Files:**
- Create: `include/alt/awaitable.hpp`
- Test: `tests/test_awaitable.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `alt::awaiter<T>`, `alt::awaitable<T>`, `alt::awaitable_of<T, R>`, `alt::detail::awaiter_t<T>`, `alt::await_result_t<A>`, `alt::get_awaiter(A&&)`, `alt::as_expected(A&&)`. Every later task uses `awaitable` and `await_result_t`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_awaitable.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/awaitable.hpp>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <type_traits>

namespace
{
	// A bare awaiter: satisfies the three await members directly.
	struct ready_awaiter
	{
		bool await_ready() const noexcept { return true; }
		void await_suspend(std::coroutine_handle<>) const noexcept {}
		int  await_resume() const noexcept { return 42; }
	};

	// An awaitable that is not itself an awaiter: it has a member operator co_await.
	struct member_awaitable
	{
		ready_awaiter operator co_await() const noexcept { return {}; }
	};

	// An awaitable reached through a free operator co_await.
	struct free_awaitable
	{
	};

	ready_awaiter operator co_await(free_awaitable) noexcept { return {}; }

	// An awaiter whose await_resume throws, for as_expected.
	struct throwing_awaiter
	{
		bool await_ready() const noexcept { return true; }
		void await_suspend(std::coroutine_handle<>) const noexcept {}
		int  await_resume() const { throw std::runtime_error{"boom"}; }
	};

	struct not_awaitable
	{
		int value = 0;
	};
}

TEST(AwaitableConcepts, BareAwaiterSatisfiesBothConcepts)
{
	static_assert(alt::awaiter<ready_awaiter>);
	static_assert(alt::awaitable<ready_awaiter>);
}

TEST(AwaitableConcepts, MemberAndFreeOperatorCoAwaitAreAwaitable)
{
	static_assert(alt::awaitable<member_awaitable>);
	static_assert(alt::awaitable<free_awaitable>);
	static_assert(!alt::awaiter<member_awaitable>);
}

TEST(AwaitableConcepts, PlainTypeIsNotAwaitable)
{
	static_assert(!alt::awaitable<not_awaitable>);
	static_assert(!alt::awaiter<not_awaitable>);
}

TEST(AwaitableConcepts, AwaitResultIsTheResumeType)
{
	static_assert(std::is_same_v<alt::await_result_t<ready_awaiter>, int>);
	static_assert(std::is_same_v<alt::await_result_t<member_awaitable>, int>);
	static_assert(std::is_same_v<alt::await_result_t<free_awaitable>, int>);
	static_assert(alt::awaitable_of<member_awaitable, int>);
	static_assert(!alt::awaitable_of<member_awaitable, long>);
}

TEST(GetAwaiter, ResolvesEachForm)
{
	ready_awaiter    bare{};
	member_awaitable member{};
	free_awaitable   freely{};

	EXPECT_EQ(alt::get_awaiter(bare).await_resume(), 42);
	EXPECT_EQ(alt::get_awaiter(member).await_resume(), 42);
	EXPECT_EQ(alt::get_awaiter(freely).await_resume(), 42);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/awaitable.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/awaitable.hpp`:

```cpp
#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <expected>
#include <type_traits>
#include <utility>

namespace alt
{

	/**
	 * @brief Satisfied by a type usable directly as a coroutine awaiter.
	 *
	 * Requires the three await member functions. await_suspend is probed with an
	 * erased std::coroutine_handle<>, so an awaiter that only accepts a typed
	 * handle does not satisfy this concept even though co_await would accept it.
	 * That restriction keeps the concept usable; the library's own internal
	 * awaiters take typed handles and are not required to satisfy it.
	 */
	template<typename T>
	concept awaiter = requires(T& a, std::coroutine_handle<> h) {
		{ a.await_ready() } -> std::convertible_to<bool>;
		a.await_suspend(h);
		a.await_resume();
	};

	namespace detail
	{

		template<typename T>
		concept has_member_co_await = requires(T&& t) { std::forward<T>(t).operator co_await(); };

		template<typename T>
		concept has_free_co_await = requires(T&& t) { operator co_await(std::forward<T>(t)); };

	}

	/**
	 * @brief Resolves an awaitable to its awaiter.
	 *
	 * Applies a member operator co_await, then a free one, and otherwise returns
	 * the argument unchanged on the assumption that it is already an awaiter. The
	 * result may be a reference into the argument, so the argument must outlive it.
	 */
	template<typename A>
	constexpr decltype(auto) get_awaiter(A&& a)
	{
		if constexpr(detail::has_member_co_await<A>)
		{
			return std::forward<A>(a).operator co_await();
		}
		else if constexpr(detail::has_free_co_await<A>)
		{
			return operator co_await(std::forward<A>(a));
		}
		else
		{
			return std::forward<A>(a);
		}
	}

	namespace detail
	{

		/** The awaiter type that alt::get_awaiter yields for A, with references stripped. */
		template<typename A>
		using awaiter_t = std::remove_reference_t<decltype(alt::get_awaiter(std::declval<A&&>()))>;

	}

	/**
	 * @brief Satisfied by anything co_await can be applied to outside a promise context.
	 *
	 * A promise type's await_transform can make an expression awaitable in one
	 * coroutine and not in another, and no concept can observe that. This concept
	 * therefore describes context-free awaitables only. alt::task uses
	 * await_transform, so this limitation is real rather than theoretical.
	 */
	template<typename T>
	concept awaitable = awaiter<detail::awaiter_t<T>>;

	/** @brief The type produced by awaiting A. */
	template<awaitable A>
	using await_result_t = decltype(std::declval<detail::awaiter_t<A>&>().await_resume());

	/** @brief Satisfied by an awaitable whose result is exactly R. */
	template<typename T, typename R>
	concept awaitable_of = awaitable<T> && std::same_as<await_result_t<T>, R>;

	namespace detail
	{

		/** The std::expected type that as_expected yields for a result of type R. */
		template<typename R>
		using expected_result_t = std::expected<std::conditional_t<std::is_void_v<R>, void, std::decay_t<R>>, std::exception_ptr>;

		/** Awaiter that converts a thrown exception from await_resume into an unexpected value. */
		template<typename Inner>
		class expected_awaiter
		{
		public:
			using result_type   = decltype(std::declval<Inner&>().await_resume());
			using expected_type = expected_result_t<result_type>;

			explicit expected_awaiter(Inner inner) : m_inner(std::move(inner)) {}

			bool await_ready() { return m_inner.await_ready(); }

			template<typename Promise>
			decltype(auto) await_suspend(std::coroutine_handle<Promise> h)
			{
				return m_inner.await_suspend(h);
			}

			expected_type await_resume()
			{
				try
				{
					if constexpr(std::is_void_v<result_type>)
					{
						m_inner.await_resume();
						return expected_type{};
					}
					else
					{
						return expected_type{m_inner.await_resume()};
					}
				}
				catch(...)
				{
					return expected_type{std::unexpect, std::current_exception()};
				}
			}

		private:
			Inner m_inner;
		};

		/** Owns the wrapped awaitable so a temporary survives until the co_await completes. */
		template<typename A>
		class expected_awaitable
		{
		public:
			explicit expected_awaitable(A&& a) : m_awaitable(std::forward<A>(a)) {}

			auto operator co_await() &&
			{
				return expected_awaiter<awaiter_t<A>>{alt::get_awaiter(std::forward<A>(m_awaitable))};
			}

		private:
			A m_awaitable;
		};

	}

	/**
	 * @brief Adapts an awaitable so that awaiting it yields std::expected instead of throwing.
	 *
	 * The result is std::expected<std::decay_t<R>, std::exception_ptr>, or
	 * std::expected<void, std::exception_ptr> when R is void. A reference result is
	 * decayed to a value.
	 *
	 * Only exceptions thrown by await_resume are captured, which is where
	 * alt::task reports the result of its coroutine body. An exception thrown by
	 * await_suspend, meaning the awaited operation failed to start, still
	 * propagates at the await expression.
	 */
	template<awaitable A>
	[[nodiscard]] auto as_expected(A&& a)
	{
		return detail::expected_awaitable<A>{std::forward<A>(a)};
	}

}
```

- [ ] **Step 4: Add the `as_expected` tests**

Append to `tests/test_awaitable.cpp`:

```cpp
TEST(AsExpected, CarriesAValueOnSuccess)
{
	auto adapted = alt::as_expected(ready_awaiter{});
	auto awaiter = std::move(adapted).operator co_await();
	ASSERT_TRUE(awaiter.await_ready());
	const auto result = awaiter.await_resume();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, 42);
}

TEST(AsExpected, CapturesAThrownExceptionInsteadOfPropagating)
{
	auto adapted = alt::as_expected(throwing_awaiter{});
	auto awaiter = std::move(adapted).operator co_await();
	const auto result = awaiter.await_resume();
	ASSERT_FALSE(result.has_value());
	EXPECT_THROW(std::rethrow_exception(result.error()), std::runtime_error);
}

TEST(AsExpected, VoidResultBecomesExpectedVoid)
{
	struct void_awaiter
	{
		bool await_ready() const noexcept { return true; }
		void await_suspend(std::coroutine_handle<>) const noexcept {}
		void await_resume() const noexcept {}
	};

	auto adapted = alt::as_expected(void_awaiter{});
	auto awaiter = std::move(adapted).operator co_await();
	const auto result = awaiter.await_resume();
	static_assert(std::is_same_v<decltype(result), const std::expected<void, std::exception_ptr>>);
	EXPECT_TRUE(result.has_value());
}
```

- [ ] **Step 5: Register the test file**

In `tests/CMakeLists.txt`, add `test_awaitable.cpp` to the `alt_tests` source list.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R Awaitable
ctest --preset debug --output-on-failure -R AsExpected
ctest --preset debug --output-on-failure -R GetAwaiter
```

Expected: PASS.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i include/alt/awaitable.hpp tests/test_awaitable.cpp
git add include/alt/awaitable.hpp tests/test_awaitable.cpp tests/CMakeLists.txt
git commit -m "Add the awaitable concepts layer

Concepts for awaiters and awaitables, the awaiter resolution helper, the
await result trait, and the as_expected adaptor. The concepts describe
context-free awaitables only, since no concept can observe a promise
type's await_transform."
```

---

### Task 2: Coroutine result storage

**Files:**
- Create: `include/alt/detail/coroutine.hpp`
- Test: `tests/test_coroutine.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `alt::detail::result_store<T>` with `set_value`, `set_exception`, `is_set`, and `value`. Specialized for `void` and for `T&`. Used by Task 5 (`task<T>`) and Task 6 (`sync_wait`).

This is the reason `task<T>` needs no partial specialization of its own: all the variation between a value result, a reference result, and `void` is absorbed here.

- [ ] **Step 1: Write the failing test**

Create `tests/test_coroutine.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/detail/coroutine.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

TEST(ResultStore, StartsEmpty)
{
	const alt::detail::result_store<int> store;
	EXPECT_FALSE(store.is_set());
}

TEST(ResultStore, HoldsAValue)
{
	alt::detail::result_store<int> store;
	store.set_value(7);
	EXPECT_TRUE(store.is_set());
	EXPECT_EQ(store.value(), 7);
}

TEST(ResultStore, RethrowsAStoredException)
{
	alt::detail::result_store<int> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_TRUE(store.is_set());
	EXPECT_THROW(store.value(), std::runtime_error);
}

TEST(ResultStore, MovesAMoveOnlyValueOut)
{
	alt::detail::result_store<std::unique_ptr<int>> store;
	store.set_value(std::make_unique<int>(3));
	const auto taken = std::move(store).value();
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 3);
}

TEST(ResultStore, StoresExceptionPtrAsAValueWithoutAmbiguity)
{
	// T is itself std::exception_ptr: the value and error channels must stay distinct.
	alt::detail::result_store<std::exception_ptr> store;
	store.set_value(std::make_exception_ptr(std::runtime_error{"as a value"}));
	EXPECT_TRUE(store.is_set());
	EXPECT_NO_THROW(store.value());
	EXPECT_NE(store.value(), nullptr);
}

TEST(ResultStoreVoid, HoldsCompletion)
{
	alt::detail::result_store<void> store;
	EXPECT_FALSE(store.is_set());
	store.set_value();
	EXPECT_TRUE(store.is_set());
	EXPECT_NO_THROW(store.value());
}

TEST(ResultStoreVoid, RethrowsAStoredException)
{
	alt::detail::result_store<void> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_THROW(store.value(), std::runtime_error);
}

TEST(ResultStoreReference, RefersToTheOriginalObject)
{
	std::string subject{"hello"};
	alt::detail::result_store<std::string&> store;
	store.set_value(subject);
	EXPECT_EQ(&store.value(), &subject);
	store.value() += " there";
	EXPECT_EQ(subject, "hello there");
}

TEST(ResultStoreReference, RethrowsAStoredException)
{
	alt::detail::result_store<std::string&> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_THROW(store.value(), std::runtime_error);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/detail/coroutine.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/detail/coroutine.hpp`:

```cpp
#pragma once

#include <cassert>
#include <exception>
#include <utility>
#include <variant>

namespace alt::detail
{

	/**
	 * @brief Holds the outcome of a coroutine: a value or an exception.
	 *
	 * Access through value() rethrows a stored exception, so a caller never has to
	 * ask which channel is occupied. The alternatives are addressed by index
	 * rather than by type, so a store whose value type is itself
	 * std::exception_ptr keeps its two channels distinct.
	 *
	 * Specializations exist for void and for reference types, which is why
	 * alt::task needs no specialization of its own.
	 */
	template<typename T>
	class result_store
	{
	public:
		/** Stores a value, replacing whatever was held. */
		template<typename U>
		void set_value(U&& value)
		{
			m_state.template emplace<1>(std::forward<U>(value));
		}

		/** Stores an exception to be rethrown by value(). */
		void set_exception(std::exception_ptr error) noexcept
		{
			m_state.template emplace<2>(std::move(error));
		}

		/** True once either channel has been filled. */
		[[nodiscard]] bool is_set() const noexcept { return m_state.index() != 0; }

		/** The stored value, or a rethrow of the stored exception. */
		[[nodiscard]] T& value() &
		{
			rethrow_if_failed();
			return std::get<1>(m_state);
		}

		/** The stored value moved out, or a rethrow of the stored exception. */
		[[nodiscard]] T&& value() &&
		{
			rethrow_if_failed();
			return std::get<1>(std::move(m_state));
		}

		/** The stored value, or a rethrow of the stored exception. */
		[[nodiscard]] const T& value() const&
		{
			rethrow_if_failed();
			return std::get<1>(m_state);
		}

	private:
		void rethrow_if_failed() const
		{
			assert(is_set() && "reading an alt coroutine result before it was produced");
			if(m_state.index() == 2)
			{
				std::rethrow_exception(std::get<2>(m_state));
			}
		}

		std::variant<std::monostate, T, std::exception_ptr> m_state;
	};

	/** @brief result_store for a coroutine that produces no value. */
	template<>
	class result_store<void>
	{
	public:
		/** Records successful completion. */
		void set_value() noexcept { m_completed = true; }

		/** Stores an exception to be rethrown by value(). */
		void set_exception(std::exception_ptr error) noexcept
		{
			m_error     = std::move(error);
			m_completed = true;
		}

		/** True once completion has been recorded, successfully or not. */
		[[nodiscard]] bool is_set() const noexcept { return m_completed; }

		/** Returns nothing, or rethrows the stored exception. */
		void value() const
		{
			assert(m_completed && "reading an alt coroutine result before it was produced");
			if(m_error)
			{
				std::rethrow_exception(m_error);
			}
		}

	private:
		std::exception_ptr m_error{};
		bool               m_completed = false;
	};

	/** @brief result_store for a coroutine that produces a reference. */
	template<typename T>
	class result_store<T&>
	{
	public:
		/** Stores a reference to an object the caller keeps alive. */
		void set_value(T& value) noexcept { m_value = std::addressof(value); }

		/** Stores an exception to be rethrown by value(). */
		void set_exception(std::exception_ptr error) noexcept { m_error = std::move(error); }

		/** True once either channel has been filled. */
		[[nodiscard]] bool is_set() const noexcept { return m_value != nullptr || m_error != nullptr; }

		/** The referenced object, or a rethrow of the stored exception. */
		[[nodiscard]] T& value() const
		{
			assert(is_set() && "reading an alt coroutine result before it was produced");
			if(m_error)
			{
				std::rethrow_exception(m_error);
			}
			return *m_value;
		}

	private:
		T*                 m_value = nullptr;
		std::exception_ptr m_error{};
	};

}
```

Note: `result_store<T&>::set_value` uses `std::addressof`, so add `#include <memory>` to the header.

- [ ] **Step 4: Register the test file and run the tests**

Add `test_coroutine.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R ResultStore
```

Expected: PASS, nine tests.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i include/alt/detail/coroutine.hpp tests/test_coroutine.cpp
git add include/alt/detail/coroutine.hpp tests/test_coroutine.cpp tests/CMakeLists.txt
git commit -m "Add coroutine result storage

Holds a value, a reference, void, or an exception behind one interface, so
neither task<T> nor sync_wait needs a specialization per result shape. The
variant alternatives are addressed by index so that a result type of
std::exception_ptr keeps its value and error channels distinct."
```

---

### Task 3: Schedulers

**Files:**
- Create: `include/alt/scheduler.hpp`
- Test: `tests/test_scheduler.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::scope_exit` from the existing `include/alt/scope.hpp`. Nothing from Tasks 1 or 2: do not add an include for either.
- Produces: `alt::work_item{void (*invoke)(void*), void* state}`, `alt::scheduler<S>`, `alt::drivable_scheduler<S>`, `alt::scheduler_ref` (default-constructible, `explicit operator bool`, `post`), `alt::trampoline_scheduler`, `alt::manual_scheduler` (`post`, `run_one`, `run_one_or_wait`, `run_all`, `empty`, `wake`), and `alt::resume_on(S&)`. Task 4 posts a destroy trampoline through `scheduler_ref`; Task 6 drives a `drivable_scheduler`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_scheduler.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
	// A work_item that appends its tag to a shared log.
	struct logged_call
	{
		std::vector<int>* log;
		int               tag;
	};

	void log_trampoline(void* state)
	{
		auto* const call = static_cast<logged_call*>(state);
		call->log->push_back(call->tag);
	}

	alt::work_item item_for(logged_call& call) noexcept
	{
		return alt::work_item{&log_trampoline, &call};
	}
}

TEST(SchedulerConcepts, ManualAndTrampolineAreSchedulers)
{
	static_assert(alt::scheduler<alt::manual_scheduler>);
	static_assert(alt::scheduler<alt::trampoline_scheduler>);
	static_assert(alt::drivable_scheduler<alt::manual_scheduler>);
	static_assert(!alt::drivable_scheduler<alt::trampoline_scheduler>);
}

TEST(ManualScheduler, RunsNothingUntilPumped)
{
	std::vector<int>     log;
	logged_call          call{&log, 1};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(call));
	EXPECT_TRUE(log.empty());
	EXPECT_FALSE(scheduler.empty());

	EXPECT_TRUE(scheduler.run_one());
	EXPECT_EQ(log, std::vector<int>{1});
	EXPECT_TRUE(scheduler.empty());
	EXPECT_FALSE(scheduler.run_one());
}

TEST(ManualScheduler, RunsInPostOrder)
{
	std::vector<int>     log;
	logged_call          first{&log, 1};
	logged_call          second{&log, 2};
	logged_call          third{&log, 3};
	alt::manual_scheduler scheduler;

	scheduler.post(item_for(first));
	scheduler.post(item_for(second));
	scheduler.post(item_for(third));

	EXPECT_EQ(scheduler.run_all(), 3U);
	EXPECT_EQ(log, (std::vector<int>{1, 2, 3}));
}

TEST(ManualScheduler, AcceptsPostsFromAnotherThread)
{
	std::vector<int>     log;
	logged_call          call{&log, 9};
	alt::manual_scheduler scheduler;

	std::thread poster{[&] { scheduler.post(item_for(call)); }};
	poster.join();

	EXPECT_EQ(scheduler.run_all(), 1U);
	EXPECT_EQ(log, std::vector<int>{9});
}

TEST(ManualScheduler, RunOneOrWaitBlocksUntilAPostArrives)
{
	std::vector<int>     log;
	logged_call          call{&log, 4};
	alt::manual_scheduler scheduler;

	std::thread poster{[&] { scheduler.post(item_for(call)); }};
	EXPECT_TRUE(scheduler.run_one_or_wait());
	poster.join();
	EXPECT_EQ(log, std::vector<int>{4});
}

TEST(ManualScheduler, WakeReleasesABlockedPumpWithoutRunningAnything)
{
	alt::manual_scheduler scheduler;
	std::atomic<bool>     returned{false};

	std::thread pump{[&] {
		const bool ran = scheduler.run_one_or_wait();
		returned.store(true, std::memory_order_release);
		EXPECT_FALSE(ran);
	}};

	scheduler.wake();
	pump.join();
	EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(TrampolineScheduler, RunsANonReentrantPostSynchronously)
{
	std::vector<int>          log;
	logged_call               call{&log, 1};
	alt::trampoline_scheduler scheduler;

	scheduler.post(item_for(call));
	EXPECT_EQ(log, std::vector<int>{1});
}

TEST(TrampolineScheduler, DefersAReentrantPostToTheOutermostInvocation)
{
	// The outer item posts an inner item while it is running. The inner item must
	// not run inside the outer one, and must run before the outer post returns.
	alt::trampoline_scheduler scheduler;
	std::vector<int>          log;
	logged_call               inner{&log, 2};

	struct outer_state
	{
		alt::trampoline_scheduler* scheduler;
		std::vector<int>*          log;
		logged_call*               inner;
	};

	outer_state state{&scheduler, &log, &inner};

	const auto outer_trampoline = [](void* raw) {
		auto* const s = static_cast<outer_state*>(raw);
		s->log->push_back(1);
		s->scheduler->post(item_for(*s->inner));
		s->log->push_back(3);   // must precede the inner item
	};

	scheduler.post(alt::work_item{outer_trampoline, &state});
	EXPECT_EQ(log, (std::vector<int>{1, 3, 2}));
}

TEST(SchedulerRef, IsEmptyWhenDefaultConstructed)
{
	const alt::scheduler_ref empty;
	EXPECT_FALSE(static_cast<bool>(empty));
}

TEST(SchedulerRef, ForwardsPostsToTheReferencedScheduler)
{
	std::vector<int>     log;
	logged_call          call{&log, 5};
	alt::manual_scheduler scheduler;

	const alt::scheduler_ref reference{scheduler};
	EXPECT_TRUE(static_cast<bool>(reference));

	reference.post(item_for(call));
	EXPECT_EQ(scheduler.run_all(), 1U);
	EXPECT_EQ(log, std::vector<int>{5});
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/scheduler.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/scheduler.hpp`:

```cpp
#pragma once

#include <alt/scope.hpp>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace alt
{

	/**
	 * @brief A unit of work a scheduler can run, erased without allocating.
	 *
	 * A function pointer plus an opaque state pointer. The state must outlive the
	 * point at which the item runs, which for a coroutine resume means the frame
	 * itself supplies the storage.
	 */
	struct work_item
	{
		void (*invoke)(void* state) = nullptr;
		void* state                 = nullptr;
	};

	/**
	 * @brief Satisfied by anything that can run a work_item.
	 *
	 * This single operation is the whole scheduler contract. Resumption
	 * (alt::resume_on) and cancelled-frame destruction are both expressed as
	 * work items, so a scheduler does not need to know about coroutines at all.
	 */
	template<typename S>
	concept scheduler = requires(S& s, work_item w) { s.post(w); };

	/**
	 * @brief A scheduler whose queue a blocked thread can drive itself.
	 *
	 * Required by the driving overload of alt::sync_wait, which pumps rather than
	 * parking so that single-threaded work cannot deadlock.
	 */
	template<typename S>
	concept drivable_scheduler = scheduler<S> && requires(S& s) {
		{ s.run_one() } -> std::same_as<bool>;
		{ s.run_one_or_wait() } -> std::same_as<bool>;
		s.wake();
	};

	/**
	 * @brief A non-owning, type-erased reference to a scheduler.
	 *
	 * Holds a pointer and a post function pointer, so it allocates nothing and is
	 * trivially copyable. The referenced scheduler must outlive every use.
	 * A default-constructed reference is empty and must not be posted to.
	 */
	class scheduler_ref
	{
	public:
		/** Constructs an empty reference. */
		scheduler_ref() noexcept = default;

		/** Constructs a reference to s, which must outlive this object. */
		template<scheduler S>
		explicit scheduler_ref(S& s) noexcept :
		  m_scheduler(std::addressof(s)),
		  m_post(+[](void* target, work_item w) { static_cast<S*>(target)->post(w); })
		{
		}

		/** True when this reference names a scheduler. */
		[[nodiscard]] explicit operator bool() const noexcept { return m_scheduler != nullptr; }

		/** Posts to the referenced scheduler. Precondition: this reference is not empty. */
		void post(work_item w) const
		{
			assert(m_scheduler != nullptr && "posting to an empty alt::scheduler_ref");
			m_post(m_scheduler, w);
		}

	private:
		void* m_scheduler = nullptr;
		void (*m_post)(void*, work_item) = nullptr;
	};

	namespace detail
	{

		/** Per-thread trampoline state: the queue of deferred items and the reentrancy flag. */
		struct trampoline_state
		{
			std::deque<work_item> queue;
			bool                  running = false;
		};

		inline trampoline_state& this_thread_trampoline() noexcept
		{
			static thread_local trampoline_state state;
			return state;
		}

	}

	/**
	 * @brief Runs work items immediately, but never nested.
	 *
	 * A post made while this thread is already running a trampoline item is queued
	 * instead of invoked, and the outermost invocation drains the queue before
	 * returning. Stack depth is therefore bounded by a constant however long a
	 * resume chain grows, while a non-reentrant post still runs synchronously.
	 *
	 * The state is per-thread rather than per-instance, so two instances used on
	 * one thread share a drain. That is harmless, because a work item carries
	 * everything it needs.
	 */
	class trampoline_scheduler
	{
	public:
		/** Runs w now, or queues it if this thread is already draining. */
		void post(work_item w) const
		{
			auto& state = detail::this_thread_trampoline();

			if(state.running)
			{
				state.queue.push_back(w);
				return;
			}

			state.running = true;
			const alt::scope_exit reset{[&state] { state.running = false; }};

			w.invoke(w.state);

			while(!state.queue.empty())
			{
				const work_item next = state.queue.front();
				state.queue.pop_front();
				next.invoke(next.state);
			}
		}
	};

	/**
	 * @brief A scheduler that runs nothing until it is pumped by hand.
	 *
	 * The deterministic scheduler for tests: post work, then decide exactly when
	 * it runs. Thread-safe, so a worker thread may post to a scheduler another
	 * thread pumps.
	 */
	class manual_scheduler
	{
	public:
		/** Queues w. Safe to call from any thread. */
		void post(work_item w)
		{
			{
				const std::lock_guard lock{m_mutex};
				m_queue.push_back(w);
			}
			m_signal.notify_one();
		}

		/** Runs the oldest queued item. Returns false when the queue was empty. */
		bool run_one()
		{
			work_item next{};
			{
				const std::lock_guard lock{m_mutex};
				if(m_queue.empty())
				{
					return false;
				}
				next = m_queue.front();
				m_queue.pop_front();
			}
			next.invoke(next.state);
			return true;
		}

		/**
		 * @brief Blocks until an item can be run, then runs it.
		 *
		 * Returns false when wake() released the wait instead, which is how a
		 * driving alt::sync_wait learns that the work it was waiting on finished.
		 */
		bool run_one_or_wait()
		{
			work_item next{};
			{
				std::unique_lock lock{m_mutex};
				m_signal.wait(lock, [this] { return !m_queue.empty() || m_woken; });

				// Consume the wake on every path. Leaving it set after running a
				// queued item would make a later call return false on a stale
				// signal, which a driving sync_wait would read as completion.
				m_woken = false;

				if(m_queue.empty())
				{
					return false;
				}
				next = m_queue.front();
				m_queue.pop_front();
			}
			next.invoke(next.state);
			return true;
		}

		/** Runs queued items until the queue is empty. Returns how many ran. */
		std::size_t run_all()
		{
			std::size_t count = 0;
			while(run_one())
			{
				++count;
			}
			return count;
		}

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const
		{
			const std::lock_guard lock{m_mutex};
			return m_queue.empty();
		}

		/** Releases a thread blocked in run_one_or_wait without queueing work. */
		void wake()
		{
			{
				const std::lock_guard lock{m_mutex};
				m_woken = true;
			}
			m_signal.notify_all();
		}

	private:
		mutable std::mutex      m_mutex;
		std::condition_variable m_signal;
		std::deque<work_item>   m_queue;
		bool                    m_woken = false;
	};

	namespace detail
	{

		inline void resume_trampoline(void* address)
		{
			std::coroutine_handle<>::from_address(address).resume();
		}

		/** Awaiter that suspends the current coroutine and posts its resumption. */
		template<scheduler S>
		class resume_on_awaiter
		{
		public:
			explicit resume_on_awaiter(S& s) noexcept : m_scheduler(std::addressof(s)) {}

			bool await_ready() const noexcept { return false; }

			void await_suspend(std::coroutine_handle<> h) const
			{
				// The post may run on another thread and resume the coroutine before
				// this call returns, destroying this awaiter along with the frame.
				// It must therefore be the last statement, and nothing may be read
				// from *this afterwards.
				m_scheduler->post(work_item{&resume_trampoline, h.address()});
			}

			void await_resume() const noexcept {}

		private:
			S* m_scheduler;
		};

	}

	/**
	 * @brief Awaitable that resumes the awaiting coroutine on the given scheduler.
	 *
	 * The idiomatic thread hop: co_await alt::resume_on(pool) to move onto a worker,
	 * co_await alt::resume_on(ui) to come back.
	 *
	 * The scheduler must outlive the suspension.
	 */
	template<scheduler S>
	[[nodiscard]] auto resume_on(S& s) noexcept
	{
		return detail::resume_on_awaiter<S>{s};
	}

}
```

- [ ] **Step 4: Register the test file and link Threads**

Add `test_scheduler.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`. In the same file, add `find_package(Threads REQUIRED)` near the top and `Threads::Threads` to the `target_link_libraries(alt_tests PRIVATE ...)` list, since these tests and Task 7's use `std::thread`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R "Scheduler|Trampoline"
```

Expected: PASS, ten tests. The reentrancy test must produce the order `1 3 2`; an order of `1 2 3` means the deferral is not working and the recursion bound does not hold.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i include/alt/scheduler.hpp tests/test_scheduler.cpp
git add include/alt/scheduler.hpp tests/test_scheduler.cpp tests/CMakeLists.txt
git commit -m "Add the scheduler layer

One required operation, post(work_item), with resumption and cancelled-frame
destruction both expressed as work items so a scheduler never needs to know
about coroutines. Adds an allocation-free erased reference, a trampoline
scheduler that bounds stack depth by deferring reentrant posts, a
hand-pumped scheduler for deterministic tests, and resume_on."
```

---

### Task 4: The cancellation layer

**Files:**
- Create: `include/alt/detail/cancellation.hpp`
- Create: `tests/coroutine_test_support.hpp`
- Test: `tests/test_cancellation.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::get_awaiter`, `alt::detail::awaiter_t` (Task 1), `alt::work_item`, `alt::scheduler_ref` (Task 3).
- Produces for tests: `tests/coroutine_test_support.hpp`, holding `frame_tracker`, `park`, `park_by_transfer`, `park_by_bool`, the `parked` handle, `resume_parked()`, and `reset_coroutine_test_state()`. Tasks 5 and 7 include it rather than redefining these.

**Before Step 1, create `tests/coroutine_test_support.hpp`.** Move the helpers out of the test body below into it, in a namespace named `alt_test`, so Tasks 5 and 7 can include it:

- `int live_frames` and `std::vector<int> destruction_order` as `inline` variables
- `struct frame_tracker` taking an `int tag`, incrementing `live_frames` on construction, and on destruction decrementing it and pushing its tag onto `destruction_order`; copy and assignment deleted
- `inline std::coroutine_handle<> parked`
- `struct park` (void `await_suspend`), `struct park_by_transfer` (returns `std::noop_coroutine()`), and `struct park_by_bool` (returns `true`), each storing the handle into `parked`
- `inline void resume_parked()`, which copies `parked`, clears it, and resumes the copy
- `inline void reset_coroutine_test_state()`, which zeroes `live_frames`, clears `destruction_order`, and clears `parked`

The three test files then use `alt_test::` qualified names, or a `using namespace alt_test;` at file scope, and call `reset_coroutine_test_state()` in their fixture `SetUp`. Each test file still defines its own coroutines and its own root types; only these helpers are shared.
- Produces: `alt::detail::cancellation_context` (fields `root`, `token`, `home`; member `stop_requested`), `alt::detail::abandon_chain(cancellation_context&)`, `alt::detail::suspend_result_t<Inner, Promise>`, `alt::detail::cancellation_checked<Inner>`, and `alt::detail::make_cancellation_checked(A&&, cancellation_context*)`. Task 5's `await_transform` calls `make_cancellation_checked`; Task 7 owns a `cancellation_context`.

This task contains the one delicate function in the whole design. Read the spec's "Cancellation" section before starting.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cancellation.cpp`. It defines its own root coroutine, standing in for Task 7's `cancellable_task`, so this header can be tested before that type exists.

```cpp
#include <gtest/gtest.h>

#include "coroutine_test_support.hpp"

#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <coroutine>
#include <exception>
#include <stop_token>
#include <utility>

namespace
{
	int live_trackers = 0;

	// Counts frames that were destroyed, however they were destroyed.
	struct tracker
	{
		tracker() { ++live_trackers; }
		~tracker() { --live_trackers; }
		tracker(const tracker&)            = delete;
		tracker& operator=(const tracker&) = delete;
	};

	std::coroutine_handle<> parked{};

	// Suspends and hands its handle to the test, standing in for off-thread work.
	struct park
	{
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> h) const noexcept { parked = h; }
		void await_resume() const noexcept {}
	};

	// An inner awaiter whose await_suspend returns a handle, covering the
	// symmetric-transfer path where the wrapper must return a no-op handle.
	struct park_by_transfer
	{
		bool await_ready() const noexcept { return false; }

		std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
			return std::noop_coroutine();
		}

		void await_resume() const noexcept {}
	};

	// An inner awaiter whose await_suspend returns bool, covering the path where
	// the wrapper must return true rather than false.
	struct park_by_bool
	{
		bool await_ready() const noexcept { return false; }

		bool await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
			return true;
		}

		void await_resume() const noexcept {}
	};

	// A minimal self-owning root, standing in for alt::cancellable_task.
	struct test_root
	{
		struct promise_type
		{
			alt::detail::cancellation_context context{};
			std::stop_source                  source{};

			promise_type() { context.token = source.get_token(); }

			test_root get_return_object() noexcept
			{
				context.root = std::coroutine_handle<promise_type>::from_promise(*this);
				return test_root{source, &context};
			}

			std::suspend_never initial_suspend() noexcept { return {}; }
			std::suspend_never final_suspend() noexcept { return {}; }
			void               return_void() noexcept {}
			void               unhandled_exception() { std::terminate(); }

			template<typename A>
			auto await_transform(A&& a)
			{
				return alt::detail::make_cancellation_checked(std::forward<A>(a), &context);
			}
		};

		std::stop_source                   source;
		alt::detail::cancellation_context* context;
	};

	bool body_ran_past_cancellation = false;

	template<typename Park>
	test_root parking_root()
	{
		const tracker t;
		co_await Park{};
		co_await Park{};
		body_ran_past_cancellation = true;
	}

	test_root parking_root_with_home()
	{
		const tracker t;
		co_await park{};
		co_await park{};
		body_ran_past_cancellation = true;
	}

	void resume_parked()
	{
		const auto handle = parked;
		parked            = {};
		handle.resume();
	}
}

class CancellationTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		live_trackers              = 0;
		parked                     = {};
		body_ran_past_cancellation = false;
	}
};

TEST_F(CancellationTest, ContextReportsTheToken)
{
	std::stop_source                  source;
	alt::detail::cancellation_context context;
	context.token = source.get_token();

	EXPECT_FALSE(context.stop_requested());
	source.request_stop();
	EXPECT_TRUE(context.stop_requested());
}

TEST_F(CancellationTest, UncancelledCoroutineRunsToCompletion)
{
	auto root = parking_root<park>();
	EXPECT_EQ(live_trackers, 1);

	resume_parked();   // past the first await, parks at the second
	resume_parked();   // past the second await, runs to completion

	EXPECT_TRUE(body_ran_past_cancellation);
	EXPECT_EQ(live_trackers, 0);
}

TEST_F(CancellationTest, CancelledCoroutineSelfDestructsAtItsNextSuspendPoint)
{
	auto root = parking_root<park>();
	EXPECT_EQ(live_trackers, 1);

	root.source.request_stop();
	resume_parked();   // observes cancellation at the second await

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_trackers, 0);
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterUsesSymmetricTransfer)
{
	auto root = parking_root<park_by_transfer>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_trackers, 0);
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterReturnsBool)
{
	auto root = parking_root<park_by_bool>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_trackers, 0);
}

TEST_F(CancellationTest, ATokenSwappedInWhileParkedIsObservedOnResume)
{
	// White-box: the context is the single source of truth for the signal, so
	// replacing its token mid-flight must change what the next suspend point sees.
	std::stop_source external;
	external.request_stop();

	auto root = parking_root<park>();
	root.context->token = external.get_token();

	// The root already parked at its first await, so the swapped-in token must be
	// observed at the second one.
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_trackers, 0);
}

TEST_F(CancellationTest, HomeSchedulerDefersDestructionUntilItIsPumped)
{
	alt::manual_scheduler home;
	auto                  root = parking_root_with_home();
	root.context->home         = alt::scheduler_ref{home};

	root.source.request_stop();
	resume_parked();

	// The destroy was posted, not performed: the frame is still alive.
	EXPECT_EQ(live_trackers, 1);
	EXPECT_FALSE(home.empty());

	EXPECT_EQ(home.run_all(), 1U);
	EXPECT_EQ(live_trackers, 0);
	EXPECT_FALSE(body_ran_past_cancellation);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/detail/cancellation.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/detail/cancellation.hpp`:

```cpp
#pragma once

#include <alt/awaitable.hpp>
#include <alt/scheduler.hpp>
#include <coroutine>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace alt::detail
{

	inline void destroy_trampoline(void* address) noexcept
	{
		std::coroutine_handle<>::from_address(address).destroy();
	}

	/**
	 * @brief The cancellation state of one await chain.
	 *
	 * Lives in the root coroutine's frame, so nothing here is reference counted.
	 * Every alt::task in the chain holds a pointer to the same object, relayed
	 * downwards as each child is started.
	 *
	 * The std::stop_token is the exception: the standard library reference counts
	 * the stop state, which is what lets an owner request a stop after the frame
	 * holding this object is gone.
	 */
	struct cancellation_context
	{
		/** The root of the chain. Destroying this destroys every frame below it. */
		std::coroutine_handle<> root{};

		/** The cancellation signal. An empty token can never be stopped. */
		std::stop_token token{};

		/** Where to destroy the chain. Empty means destroy in place. */
		scheduler_ref home{};

		/** True once cancellation has been requested. */
		[[nodiscard]] bool stop_requested() const noexcept { return token.stop_requested(); }
	};

	/**
	 * @brief Tears down the await chain rooted at ctx.
	 *
	 * DELICATE. Read this before changing it.
	 *
	 * When there is no home scheduler, root.destroy() destroys the root frame,
	 * which runs the destructor of the await temporary in scope there, which owns
	 * the child alt::task, which destroys the child frame, and so on down to the
	 * leaf. The leaf's await_suspend is on the stack at that moment, so this
	 * function's own arguments and every object reachable through ctx are gone the
	 * instant destroy() returns. Both values it needs are therefore copied to the
	 * stack first, and the caller must touch nothing afterwards.
	 *
	 * Marked noexcept deliberately. If posting to the home scheduler threw, the
	 * exception would propagate into a coroutine that cancellation has already
	 * decided must not run again, so terminating is preferable to resuming it.
	 */
	inline void abandon_chain(cancellation_context& ctx) noexcept
	{
		const std::coroutine_handle<> root = ctx.root;
		const scheduler_ref           home = ctx.home;

		if(home)
		{
			// The frames stay suspended until the post runs, which is what keeps
			// UI-affine state from being destroyed on a worker thread.
			home.post(work_item{&destroy_trampoline, root.address()});
			return;
		}

		root.destroy();
	}

	/** The type Inner::await_suspend returns for a coroutine whose promise is Promise. */
	template<typename Inner, typename Promise>
	using suspend_result_t = decltype(std::declval<Inner&>().await_suspend(std::declval<std::coroutine_handle<Promise>>()));

	/**
	 * @brief Wraps an awaiter so that cancellation is observed before it starts.
	 *
	 * The check happens on entry to the suspension point rather than on
	 * resumption. That ordering is what makes destruction safe: the inner awaiter
	 * has not yet registered a callback, started a thread, or queued anything, so
	 * nothing is in flight when the frames are destroyed.
	 */
	template<typename Inner>
	class cancellation_checked
	{
	public:
		cancellation_checked(Inner inner, cancellation_context* context) :
		  m_inner(std::move(inner)),
		  m_context(context)
		{
		}

		/**
		 * Forces a suspension when cancellation has been requested, so that
		 * await_suspend gets the chance to destroy. Otherwise this delegates, so an
		 * already-ready awaiter still costs nothing.
		 */
		bool await_ready()
		{
			if(m_context != nullptr && m_context->stop_requested())
			{
				return false;
			}
			return m_inner.await_ready();
		}

		template<typename Promise>
		suspend_result_t<Inner, Promise> await_suspend(std::coroutine_handle<Promise> h)
		{
			using result = suspend_result_t<Inner, Promise>;

			if(m_context != nullptr && m_context->stop_requested())
			{
				abandon_chain(*m_context);

				// *this, m_inner and m_context may all be destroyed now. Nothing below
				// may read them. The return value must also not resume this frame,
				// which is why the bool case returns true and the handle case returns
				// a no-op handle rather than the inner awaiter's answer.
				if constexpr(std::is_void_v<result>)
				{
					return;
				}
				else if constexpr(std::is_same_v<result, bool>)
				{
					return true;
				}
				else
				{
					return std::noop_coroutine();
				}
			}

			return m_inner.await_suspend(h);
		}

		decltype(auto) await_resume() { return m_inner.await_resume(); }

	private:
		Inner                 m_inner;
		cancellation_context* m_context;
	};

	/** Owns the wrapped awaitable so a temporary survives until the co_await completes. */
	template<typename A>
	class cancellation_checked_awaitable
	{
	public:
		cancellation_checked_awaitable(A&& a, cancellation_context* context) :
		  m_awaitable(std::forward<A>(a)),
		  m_context(context)
		{
		}

		auto operator co_await() &&
		{
			return cancellation_checked<awaiter_t<A>>{alt::get_awaiter(std::forward<A>(m_awaitable)), m_context};
		}

	private:
		A                     m_awaitable;
		cancellation_context* m_context;
	};

	/** Wraps any awaitable in the cancellation check. A null context disables it. */
	template<typename A>
	auto make_cancellation_checked(A&& a, cancellation_context* context)
	{
		return cancellation_checked_awaitable<A>{std::forward<A>(a), context};
	}

}
```

- [ ] **Step 4: Register the test file and run the tests**

Add `test_cancellation.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R CancellationTest
```

Expected: PASS, seven tests.

- [ ] **Step 5: Run under AddressSanitizer**

This is not optional for this task. The cascade destroys the frame whose `await_suspend` is executing, and a use-after-free there is exactly what this build catches.

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R CancellationTest
```

Expected: PASS with no sanitizer reports.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i include/alt/detail/cancellation.hpp tests/test_cancellation.cpp
git add include/alt/detail/cancellation.hpp tests/test_cancellation.cpp tests/CMakeLists.txt
git commit -m "Add the cancellation context and checking awaiter

The context lives in the root frame and is relayed down the chain, so no
reference counting is needed; the std::stop_token carries its own. The
checking awaiter observes cancellation on entry to a suspension point,
before the inner awaiter starts anything, which is what makes destroying
the chain safe. Verified under AddressSanitizer."
```

---

### Task 5: `task<T>`

**Files:**
- Create: `include/alt/task.hpp`
- Test: `tests/test_task.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::detail::result_store` (Task 2), `alt::detail::make_cancellation_checked` and `cancellation_context` (Task 4).
- Produces: `alt::task<T>` with `promise_type`, `value_type`, `is_ready()`, and both `operator co_await` overloads. Its promise exposes `set_continuation`, `set_cancellation_context`, `cancellation_context_pointer`, `mark_awaited`, `was_awaited`, and `result`. Task 6 awaits tasks; Task 7's promise reuses the same `await_transform` shape.

- [ ] **Step 1: Write the failing test**

Create `tests/test_task.cpp`:

```cpp
#include <gtest/gtest.h>

#include "coroutine_test_support.hpp"

#include <alt/detail/cancellation.hpp>
#include <alt/task.hpp>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace
{
	// Minimal eager root, used to drive tasks before sync_wait exists (Task 6).
	struct drive
	{
		struct promise_type
		{
			drive              get_return_object() noexcept { return {}; }
			std::suspend_never initial_suspend() noexcept { return {}; }
			std::suspend_never final_suspend() noexcept { return {}; }
			void               return_void() noexcept {}
			void               unhandled_exception() { std::terminate(); }
		};
	};

	template<typename T>
	drive run_into(alt::task<T> t, std::optional<T>& out, std::exception_ptr& error)
	{
		try
		{
			out = co_await std::move(t);
		}
		catch(...)
		{
			error = std::current_exception();
		}
	}

	drive run_void(alt::task<void> t, bool& completed, std::exception_ptr& error)
	{
		try
		{
			co_await std::move(t);
			completed = true;
		}
		catch(...)
		{
			error = std::current_exception();
		}
	}

	alt::task<int>          answer() { co_return 42; }
	alt::task<void>         nothing() { co_return; }
	alt::task<std::string&> borrow(std::string& subject) { co_return subject; }

	alt::task<std::unique_ptr<int>> owned()
	{
		co_return std::make_unique<int>(3);
	}

	alt::task<int> thrower()
	{
		throw std::runtime_error{"boom"};
		co_return 0;   // unreachable; makes this a coroutine
	}

	alt::task<int> countdown(int n)
	{
		if(n == 0)
		{
			co_return 0;
		}
		co_return co_await countdown(n - 1);
	}

	bool           body_ran = false;
	alt::task<int> records_that_it_ran()
	{
		body_ran = true;
		co_return 1;
	}
}

TEST(Task, DoesNotRunUntilAwaited)
{
	body_ran = false;
	{
		auto t = records_that_it_ran();
		EXPECT_FALSE(body_ran);
		EXPECT_FALSE(t.is_ready());
	}
	// Destroyed without ever being awaited: the body must never have run.
	EXPECT_FALSE(body_ran);
}

TEST(Task, ProducesAValue)
{
	std::optional<int> out;
	std::exception_ptr error;
	run_into(answer(), out, error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(*out, 42);
}

TEST(Task, SupportsAVoidResult)
{
	bool               completed = false;
	std::exception_ptr error;
	run_void(nothing(), completed, error);
	EXPECT_FALSE(error);
	EXPECT_TRUE(completed);
}

TEST(Task, SupportsAReferenceResult)
{
	std::string subject{"hello"};
	std::string* observed = nullptr;

	[](alt::task<std::string&> t, std::string** out) -> drive {
		*out = std::addressof(co_await std::move(t));
	}(borrow(subject), &observed);

	EXPECT_EQ(observed, &subject);
}

TEST(Task, MovesAMoveOnlyResultOut)
{
	std::optional<std::unique_ptr<int>> out;
	std::exception_ptr                  error;
	run_into(owned(), out, error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	ASSERT_NE(*out, nullptr);
	EXPECT_EQ(**out, 3);
}

TEST(Task, RethrowsAnExceptionFromTheBody)
{
	std::optional<int> out;
	std::exception_ptr error;
	run_into(thrower(), out, error);
	EXPECT_FALSE(out.has_value());
	ASSERT_TRUE(error);
	EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
}

TEST(Task, RethrowsAThrowFromTheResultsMoveConstructor)
{
	// The value is constructed into the promise as the coroutine returns, so a
	// throwing move must be reported through the same channel as a body throw.
	struct throws_on_move
	{
		throws_on_move() = default;

		throws_on_move(throws_on_move&&) { throw std::runtime_error{"move"}; }

		throws_on_move(const throws_on_move&)            = delete;
		throws_on_move& operator=(throws_on_move&&)      = delete;
		throws_on_move& operator=(const throws_on_move&) = delete;
	};

	const auto make = []() -> alt::task<throws_on_move> { co_return throws_on_move{}; };

	std::exception_ptr error;

	[](alt::task<throws_on_move> t, std::exception_ptr& out) -> drive {
		try
		{
			co_await std::move(t);
		}
		catch(...)
		{
			out = std::current_exception();
		}
	}(make(), error);

	ASSERT_TRUE(error);
	EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
}

TEST(Task, MovedFromTaskIsInert)
{
	auto original = answer();
	auto moved    = std::move(original);
	EXPECT_FALSE(original.is_ready());
	// original's destructor must not touch the frame moved is now holding.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(std::move(moved), out, error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(*out, 42);
}

TEST(Task, DefaultConstructedTaskIsInert)
{
	const alt::task<int> empty;
	EXPECT_FALSE(empty.is_ready());
}

TEST(Task, MoveAssignmentDestroysTheOverwrittenFrame)
{
	body_ran   = false;
	auto first = records_that_it_ran();
	first      = answer();   // the first frame must be destroyed here
	EXPECT_FALSE(body_ran);

	std::optional<int> out;
	std::exception_ptr error;
	run_into(std::move(first), out, error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(*out, 42);
}

TEST(Task, DeepChainRunsInConstantStack)
{
	// Without symmetric transfer this overflows the stack.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(countdown(50000), out, error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(*out, 0);
}

TEST(Task, IsReadyOnlyAfterCompletion)
{
	auto               t = answer();
	std::optional<int> out;
	std::exception_ptr error;
	EXPECT_FALSE(t.is_ready());
	run_into(std::move(t), out, error);
	EXPECT_TRUE(out.has_value());
}
```

Add the cancellation relay test to the same file. It is the reason `task<T>` carries a context pointer at all.

```cpp
namespace
{
	std::coroutine_handle<> parked{};

	struct park
	{
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> h) const noexcept { parked = h; }
		void await_resume() const noexcept {}
	};

	int  live_frames         = 0;
	bool child_ran_past_park = false;

	struct frame_tracker
	{
		frame_tracker() { ++live_frames; }
		~frame_tracker() { --live_frames; }
		frame_tracker(const frame_tracker&)            = delete;
		frame_tracker& operator=(const frame_tracker&) = delete;
	};

	alt::task<void> parking_child()
	{
		const frame_tracker t;
		co_await park{};
		co_await park{};
		child_ran_past_park = true;
	}

	// A root that owns a cancellation context, standing in for cancellable_task.
	struct relay_root
	{
		struct promise_type
		{
			alt::detail::cancellation_context context{};
			std::stop_source                  source{};

			promise_type() { context.token = source.get_token(); }

			relay_root get_return_object() noexcept
			{
				context.root = std::coroutine_handle<promise_type>::from_promise(*this);
				return relay_root{source};
			}

			std::suspend_never initial_suspend() noexcept { return {}; }
			std::suspend_never final_suspend() noexcept { return {}; }
			void               return_void() noexcept {}
			void               unhandled_exception() { std::terminate(); }

			[[nodiscard]] alt::detail::cancellation_context* cancellation_context_pointer() noexcept
			{
				return &context;
			}

			template<typename A>
			auto await_transform(A&& a)
			{
				return alt::detail::make_cancellation_checked(std::forward<A>(a), &context);
			}
		};

		std::stop_source source;
	};

	relay_root relay_root_awaiting_child()
	{
		const frame_tracker t;
		co_await parking_child();
	}
}

TEST(TaskCancellationRelay, ChildObservesCancellationRequestedOnTheRoot)
{
	live_frames         = 0;
	child_ran_past_park = false;
	parked              = {};

	auto root = relay_root_awaiting_child();
	EXPECT_EQ(live_frames, 2);   // root frame and child frame

	root.source.request_stop();

	const auto handle = parked;
	parked            = {};
	handle.resume();   // child resumes, observes cancellation at its second await

	EXPECT_FALSE(child_ran_past_park);
	EXPECT_EQ(live_frames, 0);   // the child destroyed the whole chain
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/task.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/task.hpp`:

```cpp
#pragma once

#include <alt/awaitable.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/detail/coroutine.hpp>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

namespace alt
{

	template<typename T = void>
	class task;

	namespace detail
	{

		/** The parts of a task promise that do not depend on how the result is returned. */
		template<typename T>
		class task_promise_base
		{
		public:
			std::suspend_always initial_suspend() noexcept { return {}; }

			/** Transfers control straight to the continuation, so a chain unwinds in constant stack. */
			struct final_awaiter
			{
				task_promise_base* self;

				bool await_ready() const noexcept { return false; }

				std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept
				{
					return self->m_continuation ? self->m_continuation : std::noop_coroutine();
				}

				void await_resume() const noexcept {}
			};

			final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

			void unhandled_exception() noexcept { m_result.set_exception(std::current_exception()); }

			/** Injects the cancellation check ahead of every suspension point in this coroutine. */
			template<typename A>
			auto await_transform(A&& a)
			{
				return make_cancellation_checked(std::forward<A>(a), m_context);
			}

			void set_continuation(std::coroutine_handle<> continuation) noexcept { m_continuation = continuation; }

			void set_cancellation_context(cancellation_context* context) noexcept { m_context = context; }

			[[nodiscard]] cancellation_context* cancellation_context_pointer() const noexcept { return m_context; }

			void mark_awaited() noexcept { m_awaited = true; }

			[[nodiscard]] bool was_awaited() const noexcept { return m_awaited; }

			[[nodiscard]] result_store<T>& result() noexcept { return m_result; }

		private:
			result_store<T>         m_result;
			std::coroutine_handle<> m_continuation{};
			cancellation_context*   m_context = nullptr;
			bool                    m_awaited = false;
		};

		template<typename T>
		class task_promise final : public task_promise_base<T>
		{
		public:
			alt::task<T> get_return_object() noexcept;

			template<typename U = T>
			void return_value(U&& value)
			{
				this->result().set_value(std::forward<U>(value));
			}
		};

		template<>
		class task_promise<void> final : public task_promise_base<void>
		{
		public:
			alt::task<void> get_return_object() noexcept;

			void return_void() noexcept { result().set_value(); }
		};

		template<typename T>
		class task_promise<T&> final : public task_promise_base<T&>
		{
		public:
			alt::task<T&> get_return_object() noexcept;

			void return_value(T& value) noexcept { this->result().set_value(value); }
		};

	}

	/**
	 * @brief A lazy, single-consumer coroutine result.
	 *
	 * The coroutine does not start until the task is awaited, and the awaiting
	 * coroutine becomes its continuation. Completion transfers control straight
	 * back to that continuation, so a chain of any depth unwinds in constant
	 * stack space.
	 *
	 * The task owns the coroutine frame and destroys it in its destructor. That
	 * ownership is what lets alt::cancellable_task tear a whole chain down by
	 * destroying only its root.
	 *
	 * Move-only, and awaitable exactly once. Awaiting an lvalue yields a
	 * reference to the result; awaiting an rvalue moves the result out. T may be
	 * void or a reference type. Arrays are not supported.
	 *
	 * Discarding a task is nearly always a bug, since a lazy task nobody awaits
	 * never runs, hence [[nodiscard]].
	 */
	template<typename T>
	class [[nodiscard]] task
	{
	public:
		using promise_type = detail::task_promise<T>;
		using value_type   = T;

		/** Constructs an empty task that owns no coroutine. */
		task() noexcept = default;

		/** Adopts a coroutine frame. Called by the promise, not by users. */
		explicit task(std::coroutine_handle<promise_type> handle) noexcept : m_handle(handle) {}

		task(task&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

		task& operator=(task&& other) noexcept
		{
			if(this != std::addressof(other))
			{
				if(m_handle)
				{
					m_handle.destroy();
				}
				m_handle = std::exchange(other.m_handle, {});
			}
			return *this;
		}

		task(const task&)            = delete;
		task& operator=(const task&) = delete;

		~task()
		{
			if(m_handle)
			{
				m_handle.destroy();
			}
		}

		/**
		 * @brief True once the coroutine has run to completion.
		 *
		 * False for an empty, unstarted, or still-running task. A ready task's
		 * result can be taken without suspending.
		 */
		[[nodiscard]] bool is_ready() const noexcept { return m_handle && m_handle.done(); }

		/** Awaits the task, yielding a reference to its result. */
		[[nodiscard]] auto operator co_await() const& noexcept { return task_awaiter<false>{m_handle}; }

		/** Awaits the task, moving its result out. */
		[[nodiscard]] auto operator co_await() && noexcept { return task_awaiter<true>{m_handle}; }

	private:
		template<bool Move>
		struct task_awaiter
		{
			std::coroutine_handle<promise_type> handle;

			bool await_ready() const noexcept
			{
				assert(handle && "awaiting an empty or moved-from alt::task");
				assert(!handle.promise().was_awaited() && "an alt::task may only be awaited once");
				return handle.done();
			}

			template<typename Promise>
			std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
			{
				handle.promise().set_continuation(awaiting);
				handle.promise().mark_awaited();

				// Relay the cancellation context down the chain when the awaiting
				// coroutine carries one. A plain coroutine carries none, and then
				// nothing is relayed and no check is ever performed below here.
				if constexpr(requires { awaiting.promise().cancellation_context_pointer(); })
				{
					handle.promise().set_cancellation_context(awaiting.promise().cancellation_context_pointer());
				}

				return handle;
			}

			decltype(auto) await_resume() const
			{
				if constexpr(Move)
				{
					return std::move(handle.promise().result()).value();
				}
				else
				{
					return handle.promise().result().value();
				}
			}
		};

		std::coroutine_handle<promise_type> m_handle{};
	};

	namespace detail
	{

		template<typename T>
		alt::task<T> task_promise<T>::get_return_object() noexcept
		{
			return alt::task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
		}

		inline alt::task<void> task_promise<void>::get_return_object() noexcept
		{
			return alt::task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
		}

		template<typename T>
		alt::task<T&> task_promise<T&>::get_return_object() noexcept
		{
			return alt::task<T&>{std::coroutine_handle<task_promise<T&>>::from_promise(*this)};
		}

	}

}
```

- [ ] **Step 4: Register the test file and run the tests**

Add `test_task.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R "Task"
```

Expected: PASS, thirteen tests. If `DeepChainRunsInConstantStack` crashes with a stack overflow, `final_awaiter::await_suspend` is not returning the continuation handle and symmetric transfer is not happening.

Three preconditions in this header are enforced by `assert` and are deliberately not unit tested: awaiting an empty or moved-from task, awaiting the same task twice, and posting to an empty `scheduler_ref`. Each one is undefined behavior in a release build, so a test could only exercise it as a death test in a debug build, which this repository does not do elsewhere. The `was_awaited` flag exists so the second of those aborts loudly rather than silently double-moving a result.

- [ ] **Step 5: Run under AddressSanitizer and commit**

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R "Task"

clang-format -i include/alt/task.hpp tests/test_task.cpp
git add include/alt/task.hpp tests/test_task.cpp tests/CMakeLists.txt
git commit -m "Add the lazy coroutine task type

Single-consumer and move-only, with completion transferring control straight
to the continuation so a chain of any depth unwinds in constant stack. One
result store absorbs the value, reference, and void cases, so the task
template itself is not specialized. The promise relays a cancellation
context to every child it awaits, which is what makes a whole chain
cancellable without any link opting in."
```

---

### Task 6: `sync_wait`

**Files:**
- Create: `include/alt/sync_wait.hpp`
- Test: `tests/test_sync_wait.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::await_result_t` (Task 1), `alt::detail::result_store` (Task 2), `alt::work_item` and `alt::drivable_scheduler` (Task 3).
- Produces: `alt::sync_wait(A&&)` and `alt::sync_wait(A&&, S&)`, plus `alt::detail::sync_wait_result_t<R>`. Task 7's tests use both.

- [ ] **Step 1: Write the failing test**

Create `tests/test_sync_wait.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace
{
	alt::task<int>  answer() { co_return 42; }
	alt::task<void> nothing() { co_return; }

	alt::task<std::unique_ptr<int>> owned() { co_return std::make_unique<int>(5); }

	alt::task<int> thrower()
	{
		throw std::runtime_error{"boom"};
		co_return 0;
	}

	alt::task<std::string&> borrow(std::string& subject) { co_return subject; }

	// Hops onto the scheduler and back, so it cannot complete without pumping.
	alt::task<int> hops(alt::manual_scheduler& scheduler)
	{
		co_await alt::resume_on(scheduler);
		co_return 7;
	}
}

TEST(SyncWait, ReturnsAValue)
{
	EXPECT_EQ(alt::sync_wait(answer()), 42);
}

TEST(SyncWait, ReturnTypeIsAValueNotAnRvalueReference)
{
	static_assert(std::is_same_v<decltype(alt::sync_wait(answer())), int>);
}

TEST(SyncWait, HandlesAVoidResult)
{
	EXPECT_NO_THROW(alt::sync_wait(nothing()));
}

TEST(SyncWait, RethrowsAnExceptionFromTheBody)
{
	EXPECT_THROW(alt::sync_wait(thrower()), std::runtime_error);
}

TEST(SyncWait, MovesAMoveOnlyResultOut)
{
	const auto taken = alt::sync_wait(owned());
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 5);
}

TEST(SyncWait, PreservesAReferenceResult)
{
	std::string subject{"hello"};
	auto&       observed = alt::sync_wait(borrow(subject));
	EXPECT_EQ(&observed, &subject);
}

TEST(SyncWait, BlockingOverloadWaitsForAnotherThread)
{
	alt::manual_scheduler scheduler;

	// A second thread pumps, so parking is correct here rather than deadlocking.
	std::jthread pump{[&] {
		while(scheduler.run_one_or_wait())
		{
		}
	}};

	EXPECT_EQ(alt::sync_wait(hops(scheduler)), 7);
	scheduler.wake();
}

TEST(SyncWait, DrivingOverloadPumpsWithoutASecondThread)
{
	// The single-threaded case. The blocking overload would deadlock here.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(hops(scheduler), scheduler), 7);
	EXPECT_TRUE(scheduler.empty());
}

TEST(SyncWait, DrivingOverloadRethrows)
{
	alt::manual_scheduler scheduler;
	EXPECT_THROW(alt::sync_wait(thrower(), scheduler), std::runtime_error);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/sync_wait.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/sync_wait.hpp`:

```cpp
#pragma once

#include <alt/awaitable.hpp>
#include <alt/detail/coroutine.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <coroutine>
#include <exception>
#include <semaphore>
#include <type_traits>
#include <utility>

namespace alt
{

	namespace detail
	{

		/**
		 * The type sync_wait returns for an await result of type R.
		 *
		 * An rvalue reference becomes a value, because the store it refers to dies
		 * when sync_wait returns. An lvalue reference is preserved, so awaiting a
		 * task<T&> still yields a reference to the original object.
		 */
		template<typename R>
		using sync_wait_result_t = std::conditional_t<std::is_rvalue_reference_v<R>, std::remove_reference_t<R>, R>;

		/**
		 * A root coroutine that runs one awaitable, stores the outcome in a store
		 * the caller owns, and then signals a work_item.
		 *
		 * It destroys its own frame at its final suspension point, before
		 * signalling. Signalling first would let the waiting thread wake up and
		 * destroy the frame while this coroutine is still inside await_suspend.
		 */
		class sync_wait_task
		{
		public:
			class promise_type
			{
			public:
				sync_wait_task get_return_object() noexcept
				{
					return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
				}

				std::suspend_always initial_suspend() noexcept { return {}; }

				struct final_awaiter
				{
					bool await_ready() const noexcept { return false; }

					void await_suspend(std::coroutine_handle<promise_type> h) const noexcept
					{
						const work_item done = h.promise().m_on_done;
						h.destroy();
						// The frame is gone. Nothing above may be read again.
						done.invoke(done.state);
					}

					void await_resume() const noexcept {}
				};

				final_awaiter final_suspend() noexcept { return {}; }

				void return_void() noexcept {}

				/** Unreachable: the coroutine body catches everything into the store. */
				void unhandled_exception() noexcept { std::terminate(); }

				void set_on_done(work_item on_done) noexcept { m_on_done = on_done; }

			private:
				work_item m_on_done{};
			};

			explicit sync_wait_task(std::coroutine_handle<promise_type> handle) noexcept : m_handle(handle) {}

			// Movable, not copyable. The frame's get_return_object returns this by
			// value and make_sync_wait_task returns it again, so a real move
			// constructor is safer than relying on elision through a coroutine
			// return object across the whole GCC 13 to Clang 19 range.
			sync_wait_task(sync_wait_task&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

			sync_wait_task(const sync_wait_task&)            = delete;
			sync_wait_task& operator=(const sync_wait_task&) = delete;
			sync_wait_task& operator=(sync_wait_task&&)      = delete;

			// No destructor work: the coroutine destroys its own frame at final suspend.
			~sync_wait_task() = default;

			/** Starts the coroutine. It owns and destroys its own frame from here on. */
			void start(work_item on_done) noexcept
			{
				const auto handle = std::exchange(m_handle, {});
				handle.promise().set_on_done(on_done);
				handle.resume();
			}

		private:
			std::coroutine_handle<promise_type> m_handle{};
		};

		template<typename A, typename T>
		sync_wait_task make_sync_wait_task(A&& a, result_store<T>& store)
		{
			try
			{
				if constexpr(std::is_void_v<T>)
				{
					co_await std::forward<A>(a);
					store.set_value();
				}
				else
				{
					store.set_value(co_await std::forward<A>(a));
				}
			}
			catch(...)
			{
				store.set_exception(std::current_exception());
			}
		}

		inline void release_semaphore(void* state) noexcept
		{
			static_cast<std::binary_semaphore*>(state)->release();
		}

		/** Completion signal for the driving overload: sets a flag and wakes the pump. */
		template<typename S>
		class drive_signal
		{
		public:
			explicit drive_signal(S& scheduler) noexcept : m_scheduler(std::addressof(scheduler)) {}

			[[nodiscard]] bool finished() const noexcept { return m_finished.load(std::memory_order_acquire); }

			static void finish(void* state) noexcept
			{
				auto* const self = static_cast<drive_signal*>(state);
				self->m_finished.store(true, std::memory_order_release);
				self->m_scheduler->wake();
			}

		private:
			S*                m_scheduler;
			std::atomic<bool> m_finished{false};
		};

	}

	/**
	 * @brief Runs an awaitable to completion, blocking the calling thread.
	 *
	 * Returns its result or rethrows its exception. This is the only blocking
	 * entry point in the library; alt::task deliberately has no get().
	 *
	 * The calling thread parks, so it cannot pump anything while it waits. Work
	 * that needs the calling thread to make progress, which includes anything
	 * driven by an alt::manual_scheduler nothing else pumps, deadlocks here. Use
	 * the overload taking a scheduler for that case.
	 */
	template<awaitable A>
	detail::sync_wait_result_t<await_result_t<A>> sync_wait(A&& a)
	{
		using result_type = await_result_t<A>;
		using stored_type = detail::sync_wait_result_t<result_type>;

		detail::result_store<stored_type> store;
		std::binary_semaphore             done{0};

		auto runner = detail::make_sync_wait_task(std::forward<A>(a), store);
		runner.start(work_item{&detail::release_semaphore, &done});
		done.acquire();

		if constexpr(std::is_void_v<stored_type>)
		{
			store.value();
		}
		else
		{
			return std::move(store).value();
		}
	}

	/**
	 * @brief Runs an awaitable to completion, pumping the given scheduler while it waits.
	 *
	 * The calling thread drives the scheduler's queue instead of parking, so
	 * single-threaded work completes rather than deadlocking. This is the
	 * overload to reach for in any test built around an alt::manual_scheduler.
	 */
	template<awaitable A, drivable_scheduler S>
	detail::sync_wait_result_t<await_result_t<A>> sync_wait(A&& a, S& scheduler)
	{
		using result_type = await_result_t<A>;
		using stored_type = detail::sync_wait_result_t<result_type>;

		detail::result_store<stored_type> store;
		detail::drive_signal<S>           signal{scheduler};

		auto runner = detail::make_sync_wait_task(std::forward<A>(a), store);
		runner.start(work_item{&detail::drive_signal<S>::finish, &signal});

		while(!signal.finished())
		{
			scheduler.run_one_or_wait();
		}

		if constexpr(std::is_void_v<stored_type>)
		{
			store.value();
		}
		else
		{
			return std::move(store).value();
		}
	}

}
```

- [ ] **Step 4: Register the test file and run the tests**

Add `test_sync_wait.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R SyncWait
```

Expected: PASS, nine tests. If `DrivingOverloadPumpsWithoutASecondThread` hangs, the completion signal is not calling `wake()` and the pump is blocked on an empty queue.

- [ ] **Step 5: Run under AddressSanitizer and commit**

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R SyncWait

clang-format -i include/alt/sync_wait.hpp tests/test_sync_wait.cpp
git add include/alt/sync_wait.hpp tests/test_sync_wait.cpp tests/CMakeLists.txt
git commit -m "Add sync_wait, the library's only blocking entry point

Two overloads: one parks the calling thread, the other pumps a scheduler so
that single-threaded work cannot deadlock. The internal runner destroys its
own frame before signalling completion, since signalling first would let the
waiting thread destroy that frame while the runner was still inside
await_suspend."
```

---

### Task 7: `cancellable_task`

**Files:**
- Create: `include/alt/cancellable_task.hpp`
- Test: `tests/test_cancellable_task.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 1 through 6.
- Produces: `alt::cancellable_task`, `alt::cancellation_handle`, `alt::exception_handler`, `alt::start_options`, `alt::start(cancellable_task, start_options)`, `alt::cancellation_point`.

Two design points to preserve while implementing:

- `start` must build the `cancellation_handle` **before** calling `resume`. A body that runs to completion without suspending destroys its own frame inside that `resume`, so reading the promise afterwards is a use-after-free.
- An external token is **linked**, not adopted. The promise always owns a `std::stop_source`, and an external token is forwarded into it through a `std::stop_callback`. That keeps `cancellation_handle::request_stop` working in every case, and `std::stop_callback`'s destructor blocks until a concurrently running callback finishes, which is the synchronization the teardown needs.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cancellable_task.cpp`:

```cpp
#include <gtest/gtest.h>

#include "coroutine_test_support.hpp"

#include <alt/cancellable_task.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace
{
	int              live_frames = 0;
	bool             reached_end = false;
	std::vector<int> destruction_order{};

	// Tagged so a test can assert which frame was destroyed first.
	struct frame_tracker
	{
		int tag;

		explicit frame_tracker(int t) : tag(t) { ++live_frames; }
		~frame_tracker()
		{
			--live_frames;
			destruction_order.push_back(tag);
		}
		frame_tracker(const frame_tracker&)            = delete;
		frame_tracker& operator=(const frame_tracker&) = delete;
	};

	std::coroutine_handle<> parked{};

	struct park
	{
		bool await_ready() const noexcept { return false; }
		void await_suspend(std::coroutine_handle<> h) const noexcept { parked = h; }
		void await_resume() const noexcept {}
	};

	void resume_parked()
	{
		const auto handle = parked;
		parked            = {};
		handle.resume();
	}

	alt::task<void> parking_leaf()
	{
		const frame_tracker t{3};
		co_await park{};
		co_await park{};
		reached_end = true;
	}

	alt::task<void> middle()
	{
		const frame_tracker t{2};
		co_await parking_leaf();
	}

	alt::cancellable_task root_over_a_chain()
	{
		const frame_tracker t{1};
		co_await middle();
	}

	alt::cancellable_task root_that_parks_then_awaits_a_child()
	{
		const frame_tracker t{1};
		co_await park{};
		co_await parking_leaf();   // cancellation observed here, before the child starts
		reached_end = true;
	}

	alt::cancellable_task root_that_completes()
	{
		const frame_tracker t{1};
		co_await park{};
		reached_end = true;
	}

	alt::cancellable_task root_that_throws()
	{
		const frame_tracker t{1};
		co_await park{};
		throw std::runtime_error{"boom"};
	}

	alt::cancellable_task compute_loop(int iterations)
	{
		const frame_tracker t{1};
		for(int i = 0; i < iterations; ++i)
		{
			co_await alt::cancellation_point{};
		}
		reached_end = true;
	}

	std::exception_ptr captured_error{};

	void capture_error(std::exception_ptr error, void* state)
	{
		*static_cast<std::exception_ptr*>(state) = std::move(error);
	}
}

class CancellableTaskTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		live_frames    = 0;
		reached_end    = false;
		parked         = {};
		captured_error = {};
		destruction_order.clear();
	}
};

TEST_F(CancellableTaskTest, DoesNotRunUntilStarted)
{
	{
		auto t = root_that_completes();
		EXPECT_EQ(live_frames, 0);
	}
	// Destroyed without being started: the body never ran and the frame is gone.
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(reached_end);
}

TEST_F(CancellableTaskTest, RunsToCompletionWhenNotCancelled)
{
	auto handle = alt::start(root_that_completes());
	EXPECT_EQ(live_frames, 1);
	resume_parked();
	EXPECT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, SelfDestructsTheWholeChainFromTheLeaf)
{
	auto handle = alt::start(root_over_a_chain());
	EXPECT_EQ(live_frames, 3);   // root, middle, leaf

	handle.request_stop();
	resume_parked();   // the leaf observes cancellation at its second await

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);

	// Root frame destruction reaches the leaf through the owning task destructors,
	// so leaf locals are destroyed before their parents'.
	EXPECT_EQ(destruction_order, (std::vector<int>{3, 2, 1}));
}

TEST_F(CancellableTaskTest, CancellationObservedAtTheRootNeverStartsTheChild)
{
	auto handle = alt::start(root_that_parks_then_awaits_a_child());
	EXPECT_EQ(live_frames, 1);

	handle.request_stop();
	resume_parked();

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);   // the child coroutine was never created
}

TEST_F(CancellableTaskTest, AnExternalTokenCancelsTheTask)
{
	std::stop_source external;
	auto             handle = alt::start(root_over_a_chain(), {.token = external.get_token()});
	EXPECT_EQ(live_frames, 3);

	external.request_stop();
	EXPECT_TRUE(handle.stop_requested());   // linked, so the handle sees it too

	resume_parked();
	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, AnAlreadyStoppedExternalTokenIsObservedAtTheFirstSuspendPoint)
{
	std::stop_source external;
	external.request_stop();

	auto handle = alt::start(root_that_completes(), {.token = external.get_token()});

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);   // destroyed at its first co_await
	EXPECT_TRUE(parked == std::coroutine_handle<>{});
}

TEST_F(CancellableTaskTest, HomeSchedulerDefersDestructionOffTheObservingThread)
{
	alt::manual_scheduler home;
	auto                  handle = alt::start(root_over_a_chain(), {.home = alt::scheduler_ref{home}});

	handle.request_stop();
	resume_parked();

	EXPECT_EQ(live_frames, 3);   // posted, not destroyed
	EXPECT_EQ(home.run_all(), 1U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(reached_end);
}

TEST_F(CancellableTaskTest, CancellationPointCreatesASuspendPointInAComputeLoop)
{
	auto handle = alt::start(compute_loop(1000000));
	// The loop has no natural suspension point, so without cancellation_point it
	// would run to completion. It suspends only when cancellation is requested,
	// so an uncancelled loop still finishes.
	EXPECT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancellationPointObservesAPreRequestedStop)
{
	std::stop_source external;
	external.request_stop();
	auto handle = alt::start(compute_loop(1000000), {.token = external.get_token()});

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancellationPointIsInertWithNoRootAboveIt)
{
	// A helper carrying a cancellation point must be safe to call from a plain
	// coroutine that no cancellable_task started.
	const auto helper = []() -> alt::task<int> {
		co_await alt::cancellation_point{};
		co_return 11;
	};

	EXPECT_EQ(alt::sync_wait(helper()), 11);
}

TEST_F(CancellableTaskTest, TheHandleIsSafeToUseAfterTheTaskIsGone)
{
	auto handle = alt::start(root_that_completes());
	resume_parked();
	ASSERT_TRUE(reached_end);
	EXPECT_EQ(live_frames, 0);

	// The frame is gone; every handle operation must still be valid.
	EXPECT_FALSE(handle.stop_requested());
	EXPECT_NO_THROW(handle.request_stop());
	EXPECT_TRUE(handle.stop_requested());
	EXPECT_NO_THROW(handle.request_stop());   // idempotent
	EXPECT_TRUE(handle.get_token().stop_requested());
}

TEST_F(CancellableTaskTest, AnUnhandledExceptionReachesTheHandler)
{
	const alt::exception_handler handler{&capture_error, &captured_error};
	auto                         task_handle = alt::start(root_that_throws(), {.on_exception = handler});

	resume_parked();

	ASSERT_TRUE(captured_error);
	EXPECT_THROW(std::rethrow_exception(captured_error), std::runtime_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellableTaskTest, CancelOnOneThreadAndSelfDestructOnAnother)
{
	// The motivating scenario: the window closes on this thread while the
	// coroutine is mid-flight, and the worker thread destroys the chain.
	auto handle = alt::start(root_over_a_chain());
	ASSERT_EQ(live_frames, 3);

	handle.request_stop();

	const auto resume_me = parked;
	parked               = {};
	std::thread worker{[resume_me] { resume_me.resume(); }};
	worker.join();

	EXPECT_FALSE(reached_end);
	EXPECT_EQ(live_frames, 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/cancellable_task.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/cancellable_task.hpp`:

```cpp
#pragma once

#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace alt
{

	/**
	 * @brief Where an alt::cancellable_task sends an exception that escaped its body.
	 *
	 * A function pointer plus an opaque state pointer, erased without allocating,
	 * in the same shape as alt::work_item. An empty handler means std::terminate.
	 */
	struct exception_handler
	{
		void (*invoke)(std::exception_ptr error, void* state) = nullptr;
		void* state                                           = nullptr;
	};

	/**
	 * @brief An owner's handle to a running alt::cancellable_task.
	 *
	 * Safe to use for as long as the handle exists, including after the task has
	 * finished or destroyed itself, because the underlying stop state is reference
	 * counted by the standard library rather than living in the coroutine frame.
	 *
	 * Copyable, so a window can keep one and hand copies elsewhere.
	 */
	class cancellation_handle
	{
	public:
		/** Constructs an empty handle that refers to no task. */
		cancellation_handle() noexcept = default;

		/** Constructs a handle over an existing stop source. */
		explicit cancellation_handle(std::stop_source source) noexcept : m_source(std::move(source)) {}

		/**
		 * @brief Asks the task to stop.
		 *
		 * Returns immediately. The task stops at its next suspension point, so
		 * nothing about the task's frames is guaranteed to have happened yet when
		 * this returns. Returns true when this call was the one that requested it.
		 */
		bool request_stop() noexcept { return m_source.request_stop(); }

		/** True once a stop has been requested, by this handle or by a linked token. */
		[[nodiscard]] bool stop_requested() const noexcept { return m_source.stop_requested(); }

		/** A token for the same stop state, for onward propagation. */
		[[nodiscard]] std::stop_token get_token() const noexcept { return m_source.get_token(); }

		/** A copy of the stop source, for an owner that wants to share the signal. */
		[[nodiscard]] std::stop_source get_source() const noexcept { return m_source; }

	private:
		std::stop_source m_source{std::nostopstate};
	};

	/** @brief Configuration for alt::start. Every field is optional. */
	struct start_options
	{
		/**
		 * An external cancellation signal to link in, so that a whole window's
		 * worth of tasks can be cancelled from one source. Empty means the task
		 * relies solely on the handle alt::start returns.
		 */
		std::stop_token token{};

		/**
		 * Where to destroy the task's frames once cancellation is observed. Empty
		 * means destroy them in place, on whichever thread observed it. Supply a
		 * scheduler when the frames hold state that must not be destroyed off its
		 * own thread, such as anything owned by a UI toolkit.
		 */
		scheduler_ref home{};

		/** Where an escaped exception goes. Empty means std::terminate. */
		exception_handler on_exception{};
	};

	class cancellable_task;

	namespace detail
	{

		/** Forwards an external stop request into the task's own source. */
		struct stop_forwarder
		{
			std::stop_source* target;

			void operator()() const noexcept { target->request_stop(); }
		};

		class cancellable_task_promise
		{
		public:
			alt::cancellable_task get_return_object() noexcept;

			std::suspend_always initial_suspend() noexcept { return {}; }

			/** Self-owning: the frame is destroyed once the body finishes. */
			struct final_awaiter
			{
				bool await_ready() const noexcept { return false; }

				void await_suspend(std::coroutine_handle<cancellable_task_promise> h) const noexcept
				{
					h.destroy();
				}

				void await_resume() const noexcept {}
			};

			final_awaiter final_suspend() noexcept { return {}; }

			void return_void() noexcept {}

			/**
			 * A root has nobody to propagate to, so an escaped exception goes to the
			 * configured handler, or terminates the process. Marked noexcept, so a
			 * handler that throws terminates too.
			 */
			void unhandled_exception() noexcept
			{
				if(m_handler.invoke == nullptr)
				{
					std::terminate();
				}
				m_handler.invoke(std::current_exception(), m_handler.state);
			}

			/** Injects the cancellation check ahead of every suspension point. */
			template<typename A>
			auto await_transform(A&& a)
			{
				return make_cancellation_checked(std::forward<A>(a), &m_context);
			}

			/** Lets an awaited alt::task relay this context to its own children. */
			[[nodiscard]] cancellation_context* cancellation_context_pointer() noexcept { return &m_context; }

			/** Installs the root handle and the options. Must run before the body starts. */
			void configure(std::coroutine_handle<> root, const alt::start_options& options)
			{
				m_context.root  = root;
				m_context.home  = options.home;
				m_context.token = m_source.get_token();
				m_handler       = options.on_exception;

				// The external token is linked rather than adopted, so that the handle
				// returned by alt::start keeps working in every case. Emplacing this
				// callback over an already-stopped token invokes it at once.
				if(options.token.stop_possible())
				{
					m_external.emplace(options.token, stop_forwarder{&m_source});
				}
			}

			[[nodiscard]] alt::cancellation_handle make_handle() const
			{
				return alt::cancellation_handle{m_source};
			}

		private:
			cancellation_context                        m_context{};
			std::stop_source                            m_source{};
			exception_handler                           m_handler{};
			std::optional<std::stop_callback<stop_forwarder>> m_external{};
		};

	}

	/**
	 * @brief A self-owning root coroutine that destroys itself when cancelled.
	 *
	 * Lazy, like alt::task: the body does not run until alt::start is called, and
	 * from that point the coroutine owns its own frame. It returns void and is not
	 * awaitable, because a root has nobody to return a value to.
	 *
	 * Once a stop has been requested, the coroutine destroys its entire await
	 * chain at the next suspension point any frame in that chain reaches. No
	 * statement of any coroutine body runs after that, so a window that has closed
	 * cannot be touched. Only destructors of live locals and of coroutine
	 * parameters run, and by default they run on the thread that observed the
	 * cancellation. Pass a home scheduler in alt::start_options to move that
	 * teardown onto a thread of your choosing.
	 *
	 * A coroutine whose body reaches no further suspension point runs to
	 * completion regardless. Insert co_await alt::cancellation_point{} into a long
	 * computation to give it somewhere to stop.
	 */
	class [[nodiscard]] cancellable_task
	{
	public:
		using promise_type = detail::cancellable_task_promise;

		/** Adopts a coroutine frame. Called by the promise, not by users. */
		explicit cancellable_task(std::coroutine_handle<promise_type> handle) noexcept : m_handle(handle) {}

		cancellable_task(cancellable_task&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}

		cancellable_task& operator=(cancellable_task&&)      = delete;
		cancellable_task(const cancellable_task&)            = delete;
		cancellable_task& operator=(const cancellable_task&) = delete;

		/** Destroys the frame if the task was never started. */
		~cancellable_task()
		{
			if(m_handle)
			{
				m_handle.destroy();
			}
		}

	private:
		friend cancellation_handle start(cancellable_task t, start_options options);

		std::coroutine_handle<promise_type> m_handle{};
	};

	/**
	 * @brief Starts a cancellable task and returns the owner's handle to it.
	 *
	 * The task owns its own frame from here on, so the returned handle is the only
	 * thing the caller needs to keep, and it is safe to keep for as long as the
	 * caller likes.
	 */
	[[nodiscard]] inline cancellation_handle start(cancellable_task t, start_options options = {})
	{
		const auto handle = std::exchange(t.m_handle, {});
		assert(handle && "starting an empty alt::cancellable_task");

		handle.promise().configure(handle, options);

		// The handle must be built before the resume. A body that completes without
		// suspending destroys its own frame inside resume(), and reading the promise
		// afterwards would be a use-after-free.
		cancellation_handle result = handle.promise().make_handle();

		handle.resume();
		return result;
	}

	/**
	 * @brief A suspension point for a computation that has none of its own.
	 *
	 * Awaiting this inside an alt::cancellable_task or alt::task costs one atomic
	 * load and never suspends unless cancellation has been requested, in which
	 * case the chain is torn down there. Awaited in any other coroutine it is an
	 * inert no-op, so a helper carrying one is safe to call from anywhere.
	 */
	struct cancellation_point
	{
		bool await_ready() const noexcept { return true; }
		void await_suspend(std::coroutine_handle<>) const noexcept {}
		void await_resume() const noexcept {}
	};

	namespace detail
	{

		inline alt::cancellable_task cancellable_task_promise::get_return_object() noexcept
		{
			return alt::cancellable_task{std::coroutine_handle<cancellable_task_promise>::from_promise(*this)};
		}

	}

}
```

- [ ] **Step 4: Register the test file and run the tests**

Add `test_cancellable_task.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R CancellableTaskTest
```

Expected: PASS, fourteen tests.

- [ ] **Step 5: Run the whole suite under AddressSanitizer**

```bash
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure
```

Expected: PASS with no sanitizer reports. This is the gate for the whole design: every teardown path destroys a frame whose `await_suspend` is on the stack.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i include/alt/cancellable_task.hpp tests/test_cancellable_task.cpp
git add include/alt/cancellable_task.hpp tests/test_cancellable_task.cpp tests/CMakeLists.txt
git commit -m "Add cancellable_task, a coroutine that destroys itself when cancelled

A self-owning root: lazy until alt::start, which installs the cancellation
context and returns the owner's handle. Once a stop is requested the chain is
destroyed at the next suspension point any frame reaches, so no coroutine
body statement runs afterwards and a closed window cannot be touched. An
external token is linked through a stop callback rather than adopted, which
keeps the returned handle working in every case."
```

---

### Task 8: Documentation

**Files:**
- Create: `docs/awaitable.md`, `docs/task.md`, `docs/scheduler.md`, `docs/sync_wait.md`, `docs/cancellable_task.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: the finished public API from Tasks 1 through 7.
- Produces: nothing code depends on.

Every existing component in this library has a page in `docs/`. Read `docs/scope.md` and `docs/base64.md` first and match their structure.

- [ ] **Step 1: Write the five component pages**

Each page covers: what the component is, a worked example, the full public API with signatures, and its hazards. The hazards are not optional, and each page must carry the ones that apply:

| Page | Hazards it must document |
|------|--------------------------|
| `docs/awaitable.md` | The concepts describe context-free awaitables only, since no concept can observe a promise type's `await_transform`. `as_expected` captures only what `await_resume` throws. |
| `docs/task.md` | Awaitable exactly once. Discarding a task means it never runs. A `task<T&>` result refers to something the coroutine must keep alive. |
| `docs/scheduler.md` | The scheduler and any object a work item points at must outlive the suspension. `trampoline_scheduler` shares its drain across instances on one thread. |
| `docs/sync_wait.md` | The blocking overload deadlocks on work that needs the calling thread to pump; use the driving overload for anything built on `manual_scheduler`. |
| `docs/cancellable_task.md` | Cancellation is not acknowledged, so `request_stop` returning does not mean the frames are gone. Frames are destroyed on the observing thread unless a home scheduler is given. A body that reaches no further suspension point runs to completion. A task parked on something that never resumes never self-destructs, so a home scheduler that stops pumping leaks the frames. |

`docs/cancellable_task.md` must also carry the worked UI example from the spec's "Goal" section, since that scenario is the reason the component exists.

- [ ] **Step 2: Update the README**

Add the five components to the README's component listing, following the format already used there for the existing components.

- [ ] **Step 3: Verify the documented signatures against the headers**

```bash
grep -n "^\t\+\[\[nodiscard\]\]\|^\t\+[a-z].*(" include/alt/task.hpp include/alt/scheduler.hpp include/alt/cancellable_task.hpp
```

Read each documented signature against the header. A signature that has drifted is worse than no documentation.

- [ ] **Step 4: Commit**

```bash
git add docs/awaitable.md docs/task.md docs/scheduler.md docs/sync_wait.md docs/cancellable_task.md README.md
git commit -m "Document the coroutine components

A page per component, each carrying the hazards that apply to it. The
cancellable_task page leads with the UI scenario the component exists for."
```

---

### Task 9: Version bump to 1.6.0

**Files:**
- Modify: `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

A minor bump, because this adds five new public headers and breaks nothing. This is a standalone commit and must never be folded into a feature commit or a merge commit. Bumping on the branch means CI verifies the new version before it reaches `main`.

- [ ] **Step 1: Find every occurrence of the current version**

```bash
grep -rn "1\.5\.0\|VERSION_MINOR" CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
```

- [ ] **Step 2: Change all six files**

| File | Change |
|------|--------|
| `CMakeLists.txt` | `project(Alt VERSION 1.6.0 LANGUAGES CXX)` |
| `include/alt/version.hpp` | `ALT_VERSION_MINOR` macro and the matching constexpr variable become `6` |
| `src/version.cpp` | the returned string becomes `"1.6.0"` |
| `tests/test_version.cpp` | expected minor becomes `6`, expected string becomes `"1.6.0"` |
| `README.md` | every occurrence of the version string, including the `.deb` filename and the `find_package(Alt 1.5.0 REQUIRED)` example |
| `CLAUDE.md` | every occurrence of the version string, including the `dpkg -i` example |

- [ ] **Step 3: Verify nothing was missed**

```bash
grep -rn "1\.5\.0" --exclude-dir=build --exclude-dir=.git . | grep -v docs/superpowers
```

Expected: no output. Occurrences inside `docs/superpowers/` are historical records of what the version was when a document was written and must not be changed.

- [ ] **Step 4: Build and test**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R Version
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.6.0"
```

---

## Final verification before the pull request

- [ ] **Run every quality gate**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure

cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build/tidy

cmake -B build/check -DALT_CPPCHECK=ON
cmake --build build/check --target cppcheck
```

All four must pass with zero errors. clang-tidy treats warnings as errors, so the build fails on any finding.

- [ ] **Confirm the library still installs and consumes cleanly**

```bash
cmake -B build/local -DCMAKE_BUILD_TYPE=Release -DALT_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/alt-install
cmake --build build/local --parallel
cmake --install build/local
find /tmp/alt-install/include/alt -name '*.hpp' | sort
```

Expected: all seven new headers present, including both under `detail/`. `install(DIRECTORY include/alt)` is recursive, so no CMake change was needed for this.

- [ ] **Open the pull request against `main`**

Do not merge. Merging happens only when explicitly asked, with `gh pr merge --merge`, after CI is green.

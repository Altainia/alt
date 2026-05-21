#include <gtest/gtest.h>

#include <alt/scope.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

// ===========================================================================
// scope_exit
// ===========================================================================

// ---------------------------------------------------------------------------
// Compile-time constraints
// ---------------------------------------------------------------------------

TEST(ScopeExitConstraints, NotConstructibleFromSelf)
{
	using SE = alt::scope_exit<void (*)()>;
	static_assert(!std::is_copy_constructible_v<SE>);
	static_assert(!std::is_copy_assignable_v<SE>);
	static_assert(!std::is_move_assignable_v<SE>);
}

TEST(ScopeExitConstraints, DeductionGuide)
{
	auto f     = [] {};
	auto guard = alt::scope_exit(f);
	static_assert(std::is_same_v<decltype(guard), alt::scope_exit<decltype(f)>>);
	guard.release();
}

TEST(ScopeExitConstraints, ConstructorNoexceptWhenEFNothrowMoveConstructible)
{
	auto f = [] {};
	static_assert(std::is_nothrow_constructible_v<alt::scope_exit<decltype(f)>, decltype(f)>);
}

// ---------------------------------------------------------------------------
// Basic behavior
// ---------------------------------------------------------------------------

TEST(ScopeExit, CallsExitFunctionOnNormalExit)
{
	int calls = 0;
	{
		alt::scope_exit guard{[&] { ++calls; }};
	}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeExit, CallsExitFunctionOnExceptionExit)
{
	int calls = 0;
	try
	{
		alt::scope_exit guard{[&] { ++calls; }};
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeExit, ReleaseDisablesCallOnNormalExit)
{
	int calls = 0;
	{
		alt::scope_exit guard{[&] { ++calls; }};
		guard.release();
	}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeExit, ReleaseDisablesCallOnExceptionExit)
{
	int calls = 0;
	try
	{
		alt::scope_exit guard{[&] { ++calls; }};
		guard.release();
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeExit, ExitFunctionCalledExactlyOnce)
{
	int calls = 0;
	{
		alt::scope_exit guard{[&] { ++calls; }};
	}
	EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// Move constructor
// ---------------------------------------------------------------------------

TEST(ScopeExit, MoveTransfersOwnership)
{
	int calls = 0;
	{
		alt::scope_exit g1{[&] { ++calls; }};
		alt::scope_exit g2{std::move(g1)};
		// g1 is released; only g2 should fire
	}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeExit, MoveSourceDoesNotCall)
{
	int  calls = 0;
	auto g1    = std::make_unique<alt::scope_exit<std::function<void()>>>(
    std::function<void()>{[&] { ++calls; }});
	alt::scope_exit g2{std::move(*g1)};
	g1.reset(); // destroy g1 (already released)
	EXPECT_EQ(calls, 0);
	// g2 still owns; destroy it
	g2.release();
}

TEST(ScopeExit, MoveConstructorNoexcept)
{
	using SE = alt::scope_exit<void (*)()>;
	static_assert(std::is_nothrow_move_constructible_v<SE>);
}

// ---------------------------------------------------------------------------
// Constructor exception safety: if init of exit_function throws, f() is called
// ---------------------------------------------------------------------------

namespace
{

	struct ThrowOnCopy
	{
		int* counter;
		int* call_counter;

		ThrowOnCopy(int* ctr, int* cctr): counter{ctr}, call_counter{cctr}
		{}

		ThrowOnCopy(const ThrowOnCopy& other): counter{other.counter}, call_counter{other.call_counter}
		{
			++(*counter);
			throw std::runtime_error{"copy failed"};
		}

		ThrowOnCopy(ThrowOnCopy&&)                 = delete;
		ThrowOnCopy& operator=(const ThrowOnCopy&) = delete;
		ThrowOnCopy& operator=(ThrowOnCopy&&)      = delete;

		void operator()() const
		{
			++(*call_counter);
		}
	};

} // namespace

TEST(ScopeExit, ConstructorCallsFOnInitFailure)
{
	int         copy_attempts = 0;
	int         calls         = 0;
	ThrowOnCopy f{&copy_attempts, &calls};

	try
	{
		// f is an lvalue of a copy-only (throwing-copy) type, so the constructor
		// tries to copy it; the copy throws, so f() must be called.
		alt::scope_exit<ThrowOnCopy> guard{f};
		FAIL() << "Expected exception not thrown";
	}
	catch(const std::runtime_error&)
	{}

	EXPECT_EQ(copy_attempts, 1);
	EXPECT_EQ(calls, 1); // f() was called because initialization failed
}

// ===========================================================================
// scope_fail
// ===========================================================================

TEST(ScopeFail, DoesNotCallOnNormalExit)
{
	int calls = 0;
	{
		alt::scope_fail guard{[&] { ++calls; }};
	}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeFail, CallsOnExceptionExit)
{
	int calls = 0;
	try
	{
		alt::scope_fail guard{[&] { ++calls; }};
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeFail, ReleaseDisablesCallOnExceptionExit)
{
	int calls = 0;
	try
	{
		alt::scope_fail guard{[&] { ++calls; }};
		guard.release();
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeFail, NestedScopesCounting)
{
	// Inner scope_fail created while an exception is active (inside catch)
	// should NOT fire when it exits normally (inside the catch block).
	int calls = 0;
	try
	{
		throw std::runtime_error{"outer"};
	}
	catch(...)
	{
		// Inside this catch, uncaught_exceptions() is 0 (on most implementations
		// the exception is "caught", so the count goes back to 0).
		alt::scope_fail guard{[&] { ++calls; }};
		// guard exits normally (no new exception thrown here)
	}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeFail, ConstructorCallsFOnInitFailure)
{
	int         copy_attempts = 0;
	int         calls         = 0;
	ThrowOnCopy f{&copy_attempts, &calls};

	try
	{
		alt::scope_fail<ThrowOnCopy> guard{f};
		FAIL() << "Expected exception not thrown";
	}
	catch(const std::runtime_error&)
	{}

	EXPECT_EQ(calls, 1);
}

TEST(ScopeFail, MoveTransfersOwnership)
{
	int calls = 0;
	try
	{
		alt::scope_fail g1{[&] { ++calls; }};
		alt::scope_fail g2{std::move(g1)};
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeFail, NotConstructibleFromSelf)
{
	using SF = alt::scope_fail<void (*)()>;
	static_assert(!std::is_copy_constructible_v<SF>);
	static_assert(!std::is_copy_assignable_v<SF>);
	static_assert(!std::is_move_assignable_v<SF>);
}

TEST(ScopeFail, DeductionGuide)
{
	auto f     = [] {};
	auto guard = alt::scope_fail(f);
	static_assert(std::is_same_v<decltype(guard), alt::scope_fail<decltype(f)>>);
	guard.release();
}

// ===========================================================================
// scope_success
// ===========================================================================

TEST(ScopeSuccess, CallsOnNormalExit)
{
	int calls = 0;
	{
		alt::scope_success guard{[&] { ++calls; }};
	}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeSuccess, DoesNotCallOnExceptionExit)
{
	int calls = 0;
	try
	{
		alt::scope_success guard{[&] { ++calls; }};
		throw std::runtime_error{"test"};
	}
	catch(...)
	{}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeSuccess, ReleaseDisablesCallOnNormalExit)
{
	int calls = 0;
	{
		alt::scope_success guard{[&] { ++calls; }};
		guard.release();
	}
	EXPECT_EQ(calls, 0);
}

TEST(ScopeSuccess, DoesNotCallFOnInitFailure)
{
	// scope_success must NOT call f() if initialization of the exit function throws.
	int calls = 0;

	struct ThrowOnCopyNoCall
	{
		int* call_counter;
		explicit ThrowOnCopyNoCall(int* c): call_counter{c}
		{}
		ThrowOnCopyNoCall(const ThrowOnCopyNoCall& o): call_counter{o.call_counter}
		{
			throw std::runtime_error{"copy failed"};
		}
		ThrowOnCopyNoCall(ThrowOnCopyNoCall&&)                 = delete;
		ThrowOnCopyNoCall& operator=(const ThrowOnCopyNoCall&) = delete;
		ThrowOnCopyNoCall& operator=(ThrowOnCopyNoCall&&)      = delete;
		void               operator()() const
		{
			++(*call_counter);
		}
	};

	ThrowOnCopyNoCall f{&calls};
	try
	{
		alt::scope_success<ThrowOnCopyNoCall> guard{f};
		FAIL() << "Expected exception not thrown";
	}
	catch(const std::runtime_error&)
	{}

	EXPECT_EQ(calls, 0); // f() must NOT have been called
}

TEST(ScopeSuccess, MoveTransfersOwnership)
{
	int calls = 0;
	{
		alt::scope_success g1{[&] { ++calls; }};
		alt::scope_success g2{std::move(g1)};
	}
	EXPECT_EQ(calls, 1);
}

TEST(ScopeSuccess, DestructorNoexceptMatchesExitFunction)
{
	auto throwing     = []() -> void { throw std::runtime_error{""}; };
	auto non_throwing = []() noexcept {};

	using SS_throw   = alt::scope_success<decltype(throwing)>;
	using SS_nothrow = alt::scope_success<decltype(non_throwing)>;

	static_assert(!noexcept(std::declval<SS_throw>().~SS_throw()));
	static_assert(noexcept(std::declval<SS_nothrow>().~SS_nothrow()));
}

TEST(ScopeSuccess, NotConstructibleFromSelf)
{
	using SS = alt::scope_success<void (*)()>;
	static_assert(!std::is_copy_constructible_v<SS>);
	static_assert(!std::is_copy_assignable_v<SS>);
	static_assert(!std::is_move_assignable_v<SS>);
}

TEST(ScopeSuccess, DeductionGuide)
{
	auto f     = [] {};
	auto guard = alt::scope_success(f);
	static_assert(std::is_same_v<decltype(guard), alt::scope_success<decltype(f)>>);
	guard.release();
}

// ===========================================================================
// unique_resource
// ===========================================================================

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

	// Deleter that increments a reference counter. Not assignable (reference member).
	struct CallCounter
	{
		int& count;
		void operator()(int) const noexcept
		{
			++count;
		}
	};

	// Assignable variant — stores a pointer so copy/move assignment works.
	// Required for unique_resource move-assignment tests.
	struct AssignableCounter
	{
		int* count{};
		void operator()(int) const noexcept
		{
			if(count != nullptr)
			{
				++(*count);
			}
		}
	};

	// Assignable deleter that records which resource values were deleted.
	struct VecDeleter
	{
		std::vector<int>* deleted{};
		void              operator()(int v) const
		{
			if(deleted != nullptr)
			{
				deleted->push_back(v);
			}
		}
	};

} // namespace

// ---------------------------------------------------------------------------
// Compile-time constraints
// ---------------------------------------------------------------------------

TEST(UniqueResourceConstraints, NotCopyConstructibleOrCopyAssignable)
{
	using UR = alt::unique_resource<int, CallCounter>;
	static_assert(!std::is_copy_constructible_v<UR>);
	static_assert(!std::is_copy_assignable_v<UR>);
}

TEST(UniqueResourceConstraints, DeductionGuide)
{
	// Use a named lambda so decltype(d) refers to the same unique type in both places.
	int  n  = 0;
	auto d  = [&](int) { ++n; };
	auto ur = alt::unique_resource(42, d);
	static_assert(std::is_same_v<decltype(ur), alt::unique_resource<int, decltype(d)>>);
	ur.release();
}

// ---------------------------------------------------------------------------
// Default constructor
// ---------------------------------------------------------------------------

namespace
{
	struct NoopDeleter
	{
		void operator()(int) const noexcept
		{}
	};
} // namespace

TEST(UniqueResourceDefault, NoDeleteOnDestruction)
{
	// Default constructor requires R and D to be default-constructible.
	// execute_on_reset starts false, so the deleter is never called.
	alt::unique_resource<int, NoopDeleter> ur;
	static_assert(std::is_default_constructible_v<alt::unique_resource<int, NoopDeleter>>);
	(void)ur;
}

TEST(UniqueResourceDefault, GetReturnsZeroInitializedResource)
{
	alt::unique_resource<int, NoopDeleter> ur;
	EXPECT_EQ(ur.get(), 0);
}

// ---------------------------------------------------------------------------
// Two-arg constructor
// ---------------------------------------------------------------------------

TEST(UniqueResource, DeleterCalledOnDestruction)
{
	int calls = 0;
	{
		alt::unique_resource ur{42, CallCounter{calls}};
	}
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResource, DeleterNotCalledAfterRelease)
{
	int calls = 0;
	{
		alt::unique_resource ur{42, CallCounter{calls}};
		ur.release();
	}
	EXPECT_EQ(calls, 0);
}

TEST(UniqueResource, GetReturnsResource)
{
	int  calls = 0;
	auto ur    = alt::unique_resource(99, CallCounter{calls});
	EXPECT_EQ(ur.get(), 99);
	ur.release();
}

TEST(UniqueResource, GetDeleterReturnsDeleter)
{
	int  calls = 0;
	auto ur    = alt::unique_resource(1, CallCounter{calls});
	ur.get_deleter()(0); // call manually
	EXPECT_EQ(calls, 1);
	ur.release();
}

// ---------------------------------------------------------------------------
// Pointer resource: operator* and operator->
// ---------------------------------------------------------------------------

TEST(UniqueResourcePointer, DereferenceOperator)
{
	int  value = 42;
	int* p     = &value;
	int  calls = 0;
	{
		auto ur = alt::unique_resource(p, [&](int*) { ++calls; });
		EXPECT_EQ(*ur, 42);
	}
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResourcePointer, ArrowOperator)
{
	struct S
	{
		int x{7};
	};
	S    s;
	int  calls = 0;
	auto ur    = alt::unique_resource(&s, [&](S*) { ++calls; });
	EXPECT_EQ(ur->x, 7);
	ur.release();
}

TEST(UniqueResourcePointer, OperatorConstraints)
{
	// operator* and operator-> require is_pointer_v<R>; int is not a pointer.
	static_assert(!std::is_pointer_v<int>);
	// operator* additionally requires the pointed-to type is not void.
	static_assert(std::is_void_v<std::remove_pointer_t<void*>>);
}

// ---------------------------------------------------------------------------
// reset() no-arg
// ---------------------------------------------------------------------------

TEST(UniqueResourceReset, NoArgCallsDeleterImmediately)
{
	int  calls = 0;
	auto ur    = alt::unique_resource(1, CallCounter{calls});
	ur.reset();
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResourceReset, NoArgPreventsFurtherCallOnDestruction)
{
	int calls = 0;
	{
		auto ur = alt::unique_resource(1, CallCounter{calls});
		ur.reset();
		EXPECT_EQ(calls, 1);
	}
	EXPECT_EQ(calls, 1); // not called again
}

TEST(UniqueResourceReset, NoArgNoopWhenAlreadyReleased)
{
	int  calls = 0;
	auto ur    = alt::unique_resource(1, CallCounter{calls});
	ur.release();
	ur.reset();
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// reset(r) — replace resource
// ---------------------------------------------------------------------------

TEST(UniqueResourceResetR, ReleasesOldResourceAndOwnsNew)
{
	int              calls = 0;
	std::vector<int> deleted_values;
	auto             deleter = [&](int v) { ++calls; deleted_values.push_back(v); };

	auto ur = alt::unique_resource(1, deleter);
	ur.reset(2);
	// Old resource (1) should have been deleted
	EXPECT_EQ(calls, 1);
	ASSERT_EQ(deleted_values.size(), 1u);
	EXPECT_EQ(deleted_values[0], 1);

	// New resource (2) should be active
	EXPECT_EQ(ur.get(), 2);
}

TEST(UniqueResourceResetR, NewResourceDeletedOnDestruction)
{
	int calls = 0;
	{
		auto ur = alt::unique_resource(1, CallCounter{calls});
		ur.reset(2);
		EXPECT_EQ(calls, 1); // old resource deleted
	}
	EXPECT_EQ(calls, 2); // new resource deleted on destruction
}

// ---------------------------------------------------------------------------
// Move constructor
// ---------------------------------------------------------------------------

TEST(UniqueResourceMove, SourceNoLongerOwns)
{
	int calls = 0;
	{
		auto ur1 = alt::unique_resource(42, CallCounter{calls});
		auto ur2 = std::move(ur1);
		// ur1 should not call deleter
	}
	EXPECT_EQ(calls, 1); // only ur2 called it
}

TEST(UniqueResourceMove, MovedObjectCanBeReset)
{
	int  calls = 0;
	auto ur1   = alt::unique_resource(42, CallCounter{calls});
	auto ur2   = std::move(ur1);
	ur2.reset();
	EXPECT_EQ(calls, 1);
	ur1.reset(); // NOLINT(bugprone-use-after-move): intentional — tests that moved-from unique_resource is in released state
	EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// Move assignment
// ---------------------------------------------------------------------------

TEST(UniqueResourceMoveAssign, OldResourceReleasedNewOwned)
{
	// Use VecDeleter (assignable) so move-assignment compiles.
	std::vector<int> deleted;
	VecDeleter       del{&deleted};

	auto ua = alt::unique_resource(10, del);
	auto ub = alt::unique_resource(20, del);

	ua = std::move(ub);
	// ua's old resource (10) should be released before ub is adopted
	ASSERT_EQ(deleted.size(), 1u);
	EXPECT_EQ(deleted[0], 10);
	EXPECT_EQ(ua.get(), 20);
}

TEST(UniqueResourceMoveAssign, SourceNoLongerOwns)
{
	// Use AssignableCounter (assignable) so move-assignment compiles.
	int calls = 0;
	{
		auto ua = alt::unique_resource(1, AssignableCounter{&calls});
		auto ub = alt::unique_resource(2, AssignableCounter{&calls});
		ua      = std::move(ub);
		EXPECT_EQ(calls, 1); // ua's old resource (1) was deleted
	}
	EXPECT_EQ(calls, 2); // ua now holds 2, deleted on scope exit
}

// ---------------------------------------------------------------------------
// Reference resource type
// ---------------------------------------------------------------------------

TEST(UniqueResourceReference, ResourceIsReferenceType)
{
	int  value   = 42;
	int  calls   = 0;
	auto deleter = [&](int& v) { ++calls; v = 0; };

	{
		auto ur = alt::unique_resource<int&, decltype(deleter)>(value, deleter);
		EXPECT_EQ(ur.get(), 42);
		EXPECT_EQ(&ur.get(), &value); // same address
	}
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(value, 0); // deleter mutated value
}

// ---------------------------------------------------------------------------
// Exception safety: two-arg constructor
// ---------------------------------------------------------------------------

namespace
{

	struct ThrowOnCopyDeleter
	{
		int* call_count;
		int* copy_count;

		ThrowOnCopyDeleter(int* cc, int* cpc): call_count{cc}, copy_count{cpc}
		{}

		ThrowOnCopyDeleter(const ThrowOnCopyDeleter& o): call_count{o.call_count}, copy_count{o.copy_count}
		{
			++(*copy_count);
			throw std::runtime_error{"deleter copy failed"};
		}

		ThrowOnCopyDeleter(ThrowOnCopyDeleter&&)                 = default;
		ThrowOnCopyDeleter& operator=(const ThrowOnCopyDeleter&) = delete;
		ThrowOnCopyDeleter& operator=(ThrowOnCopyDeleter&&)      = delete;

		void operator()(int) const noexcept
		{
			++(*call_count);
		}
	};

} // namespace

TEST(UniqueResourceExceptionSafety, DeleterInitThrowsCallsDeleterOnResource)
{
	// When deleter init throws, d(RESOURCE) must be called to prevent resource leak.
	int deleter_calls = 0;
	int copy_count    = 0;

	ThrowOnCopyDeleter d{&deleter_calls, &copy_count};

	try
	{
		// Pass d as lvalue → triggers copy (which throws).
		alt::unique_resource<int, ThrowOnCopyDeleter> ur{99, d};
		FAIL() << "Expected exception not thrown";
	}
	catch(const std::runtime_error&)
	{}

	EXPECT_GE(copy_count, 1);
	EXPECT_EQ(deleter_calls, 1); // d(RESOURCE) was called
}

// ---------------------------------------------------------------------------
// make_unique_resource_checked
// ---------------------------------------------------------------------------

TEST(MakeUniqueResourceChecked, ValidResourceOwned)
{
	int calls = 0;
	{
		auto ur = alt::make_unique_resource_checked(42, -1, CallCounter{calls});
		EXPECT_EQ(ur.get(), 42);
	}
	EXPECT_EQ(calls, 1);
}

TEST(MakeUniqueResourceChecked, InvalidResourceNotOwned)
{
	int calls = 0;
	{
		auto ur = alt::make_unique_resource_checked(-1, -1, CallCounter{calls});
		EXPECT_EQ(ur.get(), -1);
	}
	EXPECT_EQ(calls, 0); // deleter must NOT be called
}

TEST(MakeUniqueResourceChecked, NullPointerInvalid)
{
	int  calls = 0;
	int  value = 7;
	auto del   = [&](int*) { ++calls; };

	{
		auto ur = alt::make_unique_resource_checked(static_cast<int*>(nullptr), nullptr, del);
	}
	EXPECT_EQ(calls, 0);

	{
		auto ur = alt::make_unique_resource_checked(&value, nullptr, del);
	}
	EXPECT_EQ(calls, 1);
}

TEST(MakeUniqueResourceChecked, CustomSentinelType)
{
	// S != decay_t<R>: pass explicit S
	int calls = 0;
	{
		// Use short as the resource and int as the sentinel type
		short res     = 5;
		int   invalid = -1;
		auto  ur      = alt::make_unique_resource_checked<short, CallCounter, int>(
      std::move(res), invalid, CallCounter{calls});
	}
	EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// Deleter called exactly once (no double-delete)
// ---------------------------------------------------------------------------

TEST(UniqueResource, DeleterCalledExactlyOnce_NormalPath)
{
	int calls = 0;
	{
		auto ur = alt::unique_resource(1, CallCounter{calls});
	}
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResource, DeleterCalledExactlyOnce_AfterReset)
{
	int calls = 0;
	{
		auto ur = alt::unique_resource(1, CallCounter{calls});
		ur.reset();
	}
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResource, DeleterCalledExactlyOnce_AfterMoveAndDestruction)
{
	int calls = 0;
	{
		auto ur1 = alt::unique_resource(1, CallCounter{calls});
		{
			auto ur2 = std::move(ur1);
		} // ur2 destroyed, deleter called once
	} // ur1 destroyed (released, no call)
	EXPECT_EQ(calls, 1);
}

TEST(UniqueResource, DeleterCalledExactlyOnce_AfterMoveAssign)
{
	int calls = 0;
	{
		auto ur1 = alt::unique_resource(1, AssignableCounter{&calls});
		auto ur2 = alt::unique_resource(2, AssignableCounter{&calls});
		ur1      = std::move(ur2); // deletes old resource in ur1 (value=1), then owns 2
		EXPECT_EQ(calls, 1);
	}
	EXPECT_EQ(calls, 2); // value=2 deleted
}

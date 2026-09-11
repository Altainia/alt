#include <gtest/gtest.h>

#include <alt/detail/cancellation.hpp>
#include <alt/task.hpp>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

namespace
{

	// Minimal eager root, used to drive tasks before sync_wait exists (Task 6).
	struct drive
	{
		struct promise_type
		{
			drive get_return_object() noexcept
			{
				return {};
			}
			std::suspend_never initial_suspend() noexcept
			{
				return {};
			}
			std::suspend_never final_suspend() noexcept
			{
				return {};
			}
			void return_void() noexcept
			{}
			void unhandled_exception()
			{
				std::terminate();
			}
		};
	};

	// Out-parameters are taken by pointer rather than reference: a coroutine
	// parameter of reference type is a lifetime hazard in general (the referent
	// might not outlive a suspension), and these drivers are coroutines
	// themselves. They happen to run eagerly to completion here, but pointers
	// keep the signature honest regardless.
	template<typename T>
	drive run_into(alt::task<T> t, std::optional<T>* out, std::exception_ptr* error)
	{
		try
		{
			*out = co_await std::move(t);
		}
		catch(...)
		{
			*error = std::current_exception();
		}
	}

	// Awaits through task's const lvalue operator co_await, so the caller's task survives
	// the await and its post-completion state can still be inspected.
	template<typename T>
	drive run_into_by_reference(const alt::task<T>* t, std::optional<T>* out, std::exception_ptr* error)
	{
		try
		{
			*out = co_await *t;
		}
		catch(...)
		{
			*error = std::current_exception();
		}
	}

	drive run_void(alt::task<void> t, bool* completed, std::exception_ptr* error)
	{
		try
		{
			co_await std::move(t);
			*completed = true;
		}
		catch(...)
		{
			*error = std::current_exception();
		}
	}

	alt::task<int> answer()
	{
		co_return 42;
	}
	alt::task<void> nothing()
	{
		co_return;
	}
	alt::task<std::string&> borrow(std::string* subject)
	{
		co_return *subject;
	}

	alt::task<std::unique_ptr<int>> owned()
	{
		co_return std::make_unique<int>(3);
	}

	alt::task<int> thrower()
	{
		throw std::runtime_error{"boom"};
		co_return 0; // unreachable; makes this a coroutine
	}

	alt::task<int> countdown(int n)
	{
		if(n == 0)
		{
			co_return 0;
		}
		co_return co_await countdown(n - 1);
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool           body_ran = false;
	alt::task<int> records_that_it_ran()
	{
		body_ran = true;
		co_return 1;
	}

	// The value is constructed into the promise as the coroutine returns, so a
	// throwing move must be reported through the same channel as a body throw.
	struct throws_on_move
	{
		throws_on_move()  = default;
		~throws_on_move() = default;

		// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor): the move constructor throws on purpose, which is the behavior RethrowsAThrowFromTheResultsMoveConstructor exercises; marking it noexcept would std::terminate instead of letting the exception reach the promise.
		throws_on_move(throws_on_move&& /*unused*/)
		{
			throw std::runtime_error{"move"};
		}

		throws_on_move(const throws_on_move&)            = delete;
		throws_on_move& operator=(throws_on_move&&)      = delete;
		throws_on_move& operator=(const throws_on_move&) = delete;
	};

	alt::task<throws_on_move> make_throwing_result()
	{
		co_return throws_on_move{};
	}

	drive run_throwing_result(alt::task<throws_on_move> t, std::exception_ptr* out)
	{
		try
		{
			co_await std::move(t);
		}
		catch(...)
		{
			*out = std::current_exception();
		}
	}

	// Every call site below only reaches this after ASSERT_TRUE(opt.has_value()) already
	// returned out of the test on failure, so the has_value() branch here always holds in
	// practice. It exists because clang-tidy's bugprone-unchecked-optional-access does not
	// trace gtest's ASSERT_TRUE macro as a has_value() check, so a bare *opt or opt.value()
	// straight after ASSERT_TRUE(opt.has_value()) still reads as unchecked to it; a real
	// `if` on the same has_value() call is a form it does recognize.
	template<typename T>
	[[nodiscard]] const T& value_of(const std::optional<T>& opt)
	{
		if(opt.has_value())
		{
			return *opt;
		}
		ADD_FAILURE() << "value_of() called on an optional with no value";
		std::abort();
	}

} // namespace

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
	run_into(answer(), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 42);
}

TEST(Task, SupportsAVoidResult)
{
	bool               completed = false;
	std::exception_ptr error;
	run_void(nothing(), &completed, &error);
	EXPECT_FALSE(error);
	EXPECT_TRUE(completed);
}

TEST(Task, SupportsAReferenceResult)
{
	std::string  subject{"hello"};
	std::string* observed = nullptr;

	[](alt::task<std::string&> t, std::string** out) -> drive {
		*out = std::addressof(co_await std::move(t));
	}(borrow(&subject), &observed);

	EXPECT_EQ(observed, &subject);
}

TEST(Task, MovesAMoveOnlyResultOut)
{
	std::optional<std::unique_ptr<int>> out;
	std::exception_ptr                  error;
	run_into(owned(), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	ASSERT_NE(value_of(out), nullptr);
	EXPECT_EQ(*value_of(out), 3);
}

TEST(Task, RethrowsAnExceptionFromTheBody)
{
	std::optional<int> out;
	std::exception_ptr error;
	run_into(thrower(), &out, &error);
	EXPECT_FALSE(out.has_value());
	ASSERT_TRUE(error);
	EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
}

TEST(Task, RethrowsAThrowFromTheResultsMoveConstructor)
{
	std::exception_ptr error;
	run_throwing_result(make_throwing_result(), &error);

	ASSERT_TRUE(error);
	EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
}

TEST(Task, MovedFromTaskIsInert)
{
	auto original = answer();
	auto moved    = std::move(original);
	// NOLINTNEXTLINE(bugprone-use-after-move): alt::task documents an empty, moved-from state as well-defined, and this line is exactly what asserts it.
	EXPECT_FALSE(original.is_ready());
	// original's destructor must not touch the frame moved is now holding.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(std::move(moved), &out, &error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 42);
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
	first      = answer(); // the first frame must be destroyed here
	EXPECT_FALSE(body_ran);

	std::optional<int> out;
	std::exception_ptr error;
	run_into(std::move(first), &out, &error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 42);
}

TEST(Task, DeepChainRunsInConstantStack)
{
	// Measured on GCC 15.2 and 16 for a 50000-frame chain: plain GCC elides the tail call
	// and passes at -O0, -O1, -O2 and -O3, and UBSan alone does not change that. Enabling
	// AddressSanitizer defeats GCC's sibling-call optimization at *every* optimization
	// level, so the chain then grows the stack linearly and overflows. That is the cause,
	// not larger frames: an optimized build would have no frames left to enlarge and so
	// could not overflow. Clang elides with or without the sanitizers at every level.
	//
	// __SANITIZE_ADDRESS__ covers the measured failure. The extra -O0 disjunct is deliberate
	// conservatism: GCC documents sibling-call elision as an optimization, so an unoptimized
	// build anywhere in the supported GCC 13+ range is not entitled to it even though the
	// versions measured here do elide.
#if defined(__GNUC__) && !defined(__clang__) && (defined(__SANITIZE_ADDRESS__) || !defined(__OPTIMIZE__))
	GTEST_SKIP() << "Symmetric transfer only avoids growing the stack where the compiler "
	                "tail-calls the handle returned from the final awaiter. Under GCC, "
	                "AddressSanitizer defeats that tail call at every optimization level, so "
	                "this chain overflows the stack; an unoptimized GCC build is also not "
	                "entitled to the elision across the supported GCC range. Clang elides "
	                "in every configuration, and so does GCC once AddressSanitizer is off.";
#endif
	// Where symmetric transfer holds (see the guard above for where it does not), this
	// chain runs in constant stack regardless of depth.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(countdown(50000), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 0);
}

namespace
{

	// An awaiter whose await_resume writes into itself, which is the only way to observe
	// whether the await ran on the caller's object or on a copy of it.
	struct writes_into_self
	{
		int observed = 0;

		bool await_ready() noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> /*awaiting*/) noexcept
		{}
		void await_resume() noexcept
		{
			observed = 99;
		}
	};

	alt::task<void> awaits_an_awaiter_lvalue(writes_into_self* subject)
	{
		co_await *subject;
	}

	// A move-only awaiter. Awaiting this as an lvalue used to be a hard error, because the
	// wrapper took its inner awaiter by value and so tried to copy it.
	struct move_only_awaiter
	{
		std::unique_ptr<int> payload = std::make_unique<int>(7);

		move_only_awaiter()                                    = default;
		~move_only_awaiter()                                   = default;
		move_only_awaiter(move_only_awaiter&&)                 = default;
		move_only_awaiter& operator=(move_only_awaiter&&)      = default;
		move_only_awaiter(const move_only_awaiter&)            = delete;
		move_only_awaiter& operator=(const move_only_awaiter&) = delete;

		bool await_ready() noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> /*awaiting*/) noexcept
		{}
		int await_resume() noexcept
		{
			return *payload;
		}
	};

	alt::task<int> awaits_a_move_only_awaiter_lvalue(move_only_awaiter* subject)
	{
		co_return co_await *subject;
	}

	alt::task<int> awaits_a_move_only_awaiter_rvalue()
	{
		co_return co_await move_only_awaiter{};
	}

} // namespace

TEST(Task, AwaitingAnAwaiterLvalueRunsOnTheCallersObject)
{
	// The cancellation wrapper sits between every co_await in an alt::task and its awaiter.
	// If it holds the awaiter by value, the caller's object is never touched and the await
	// silently reports a stale result instead of failing.
	writes_into_self subject;

	bool               completed = false;
	std::exception_ptr error;
	run_void(awaits_an_awaiter_lvalue(&subject), &completed, &error);

	EXPECT_FALSE(error);
	EXPECT_TRUE(completed);
	EXPECT_EQ(subject.observed, 99);
}

TEST(Task, AwaitingAMoveOnlyAwaiterLvalueCompiles)
{
	move_only_awaiter subject;

	std::optional<int> out;
	std::exception_ptr error;
	run_into(awaits_a_move_only_awaiter_lvalue(&subject), &out, &error);

	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 7);

	// The awaiter was referenced, not moved from, so its payload is still there.
	ASSERT_TRUE(subject.payload != nullptr);
	EXPECT_EQ(*subject.payload, 7);
}

TEST(Task, AwaitingAnAwaiterRvalueStillYieldsItsResult)
{
	// The rvalue case is the one the suite already covered. Keep it asserted next to the
	// lvalue case so a future change cannot fix one by breaking the other: a temporary
	// awaiter must still be owned by the wrapper rather than referenced.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(awaits_a_move_only_awaiter_rvalue(), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 7);
}

TEST(Task, NestedTaskAwaitRunsOnEveryToolchain)
{
	// The always-running counterpart to DeepChainRunsInConstantStack above: countdown is
	// the only coroutine in this file where one alt::task awaits another alt::task (every
	// other test awaits from the drive or relay_root promise instead), so without this test
	// that path — including the cancellation-context relay between two task promises — would
	// have no coverage at all on a toolchain where the deep chain is skipped.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(countdown(3), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 0);
}

TEST(Task, IsReadyOnlyAfterCompletion)
{
	auto               t = answer();
	std::optional<int> out;
	std::exception_ptr error;
	EXPECT_FALSE(t.is_ready());

	// Await through the lvalue overload so the task outlives the await and can still be
	// asked. The rvalue overload would leave t owning the frame too, but awaiting the
	// lvalue is the form a caller would use when it intends to inspect the task afterwards.
	run_into_by_reference(&t, &out, &error);

	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(value_of(out), 42);
	EXPECT_TRUE(t.is_ready());
}

namespace
{

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool child_ran_past_park = false;

	alt::task<void> parking_child()
	{
		const frame_tracker t{3};
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
			std::stop_source                  source;

			promise_type()
			{
				context.token = source.get_token();
			}

			relay_root get_return_object() noexcept
			{
				context.root = std::coroutine_handle<promise_type>::from_promise(*this);
				return relay_root{source};
			}

			std::suspend_never initial_suspend() noexcept
			{
				return {};
			}
			std::suspend_never final_suspend() noexcept
			{
				return {};
			}
			void return_void() noexcept
			{}
			void unhandled_exception()
			{
				std::terminate();
			}

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
		const frame_tracker t{1};
		co_await parking_child();
	}

} // namespace

TEST(TaskCancellationRelay, ChildObservesCancellationRequestedOnTheRoot)
{
	reset_coroutine_test_state();
	child_ran_past_park = false;

	auto root = relay_root_awaiting_child();
	EXPECT_EQ(live_frames, 2); // root frame and child frame

	root.source.request_stop();

	const auto handle = parked;
	parked            = {};
	handle.resume(); // child resumes, observes cancellation at its second await

	EXPECT_FALSE(child_ran_past_park);
	EXPECT_EQ(live_frames, 0); // the child destroyed the whole chain
}

namespace
{

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool leaf_ran_past_park = false;

	// The leaf of a three-frame chain: relay_root -> middle task -> this leaf task. Unlike
	// parking_child above, which relay_root awaits directly, this leaf is only ever reached
	// through an intermediate alt::task frame — that intermediate hop is the recursive step
	// under test, which the single-hop test above cannot exercise.
	alt::task<void> park_leaf()
	{
		const frame_tracker t{3};
		co_await park{};
		co_await park{};
		leaf_ran_past_park = true;
	}

	alt::task<void> await_leaf_through_middle()
	{
		const frame_tracker t{2};
		co_await park_leaf();
	}

	relay_root relay_root_awaiting_middle()
	{
		const frame_tracker t{1};
		co_await await_leaf_through_middle();
	}

} // namespace

TEST(TaskCancellationRelay, CascadeThroughAnIntermediateTaskFrameDestroysInnerToOuterOrder)
{
	reset_coroutine_test_state();
	leaf_ran_past_park = false;

	auto root = relay_root_awaiting_middle();
	EXPECT_EQ(live_frames, 3); // root, middle, and leaf frames all live

	root.source.request_stop();

	const auto handle = parked;
	parked            = {};
	handle.resume(); // leaf resumes, observes cancellation at its second await

	EXPECT_FALSE(leaf_ran_past_park);
	EXPECT_EQ(live_frames, 0); // the cascade destroyed all three frames

	// Destroying the root runs its await temporary's destructor, which owns the middle task,
	// which destroys the middle frame, which runs its own await temporary's destructor, which
	// owns the leaf. So the leaf's frame_tracker is destroyed before the middle's, and the
	// middle's before the root's: inner-to-outer order, which is the observable signature of
	// that recursive step actually happening rather than a two-frame chain repeated once.
	EXPECT_EQ(destruction_order, (std::vector<int>{3, 2, 1}));
}

#include <gtest/gtest.h>

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
	// This type's move constructor is deliberately throwing and deliberately not
	// noexcept: that is exactly the scenario RethrowsAThrowFromTheResultsMoveConstructor
	// exercises, so the general guidelines below do not apply to it.
	struct throws_on_move
	{
		throws_on_move()  = default;
		~throws_on_move() = default;

		// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor): intentionally throwing, see the comment above
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
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	EXPECT_EQ(out.value(), 42);
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
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	ASSERT_NE(out.value(), nullptr);
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	EXPECT_EQ(*out.value(), 3);
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
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	EXPECT_EQ(out.value(), 42);
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
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	EXPECT_EQ(out.value(), 42);
}

TEST(Task, DeepChainRunsInConstantStack)
{
	// Without symmetric transfer this overflows the stack.
	std::optional<int> out;
	std::exception_ptr error;
	run_into(countdown(50000), &out, &error);
	EXPECT_FALSE(error);
	ASSERT_TRUE(out.has_value());
	// NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT_TRUE above already guarantees out has a value; the checker cannot see through gtest's macro.
	EXPECT_EQ(out.value(), 0);
}

TEST(Task, IsReadyOnlyAfterCompletion)
{
	auto               t = answer();
	std::optional<int> out;
	std::exception_ptr error;
	EXPECT_FALSE(t.is_ready());
	run_into(std::move(t), &out, &error);
	EXPECT_TRUE(out.has_value());
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

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
		// NOLINTBEGIN(modernize-use-nodiscard, readability-convert-member-functions-to-static):
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<>) const noexcept
		{}
		int await_resume() const noexcept
		{
			return 42;
		}
		// NOLINTEND(modernize-use-nodiscard, readability-convert-member-functions-to-static)
	};

	// An awaitable that is not itself an awaiter: it has a member operator co_await.
	struct member_awaitable
	{
		ready_awaiter operator co_await() const noexcept
		{
			return {};
		}
	};

	// An awaitable reached through a free operator co_await.
	struct free_awaitable
	{
	};

	ready_awaiter operator co_await(free_awaitable) noexcept
	{
		return {};
	}

	// An awaiter whose await_resume throws, for as_expected.
	struct throwing_awaiter
	{
		// NOLINTBEGIN(modernize-use-nodiscard, readability-convert-member-functions-to-static):
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<>) const noexcept
		{}
		int await_resume() const
		{
			throw std::runtime_error{"boom"};
		}
		// NOLINTEND(modernize-use-nodiscard, readability-convert-member-functions-to-static)
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
	auto       adapted = alt::as_expected(throwing_awaiter{});
	auto       awaiter = std::move(adapted).operator co_await();
	const auto result  = awaiter.await_resume();
	ASSERT_FALSE(result.has_value());
	EXPECT_THROW(std::rethrow_exception(result.error()), std::runtime_error);
}

TEST(AsExpected, VoidResultBecomesExpectedVoid)
{
	struct void_awaiter
	{
		// NOLINTBEGIN(modernize-use-nodiscard, readability-convert-member-functions-to-static):
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<>) const noexcept
		{}
		void await_resume() const noexcept
		{}
		// NOLINTEND(modernize-use-nodiscard, readability-convert-member-functions-to-static)
	};

	auto       adapted = alt::as_expected(void_awaiter{});
	auto       awaiter = std::move(adapted).operator co_await();
	const auto result  = awaiter.await_resume();
	static_assert(std::is_same_v<decltype(result), const std::expected<void, std::exception_ptr>>);
	EXPECT_TRUE(result.has_value());
}

#include <gtest/gtest.h>

#include <alt/awaitable.hpp>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace
{
	// A bare awaiter: satisfies the three await members directly.
	struct ready_awaiter
	{
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> handle) const noexcept
		{
			(void)handle;
		}
		int await_resume() const noexcept
		{
			return 42;
		}
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

	ready_awaiter operator co_await(free_awaitable awaitable) noexcept
	{
		(void)awaitable;
		return {};
	}

	// An awaiter whose await_resume throws, for as_expected.
	struct throwing_awaiter
	{
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> handle) const noexcept
		{
			(void)handle;
		}
		int await_resume() const
		{
			throw std::runtime_error{"boom"};
		}
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
		// the co_await protocol always uses the return value and dispatches through
		// the object, so nodiscard is redundant and static would misrepresent the shape.
		bool await_ready() const noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> handle) const noexcept
		{
			(void)handle;
		}
		void await_resume() const noexcept
		{}
	};

	auto       adapted = alt::as_expected(void_awaiter{});
	auto       awaiter = std::move(adapted).operator co_await();
	const auto result  = awaiter.await_resume();
	static_assert(std::is_same_v<decltype(result), const std::expected<void, std::exception_ptr>>);
	EXPECT_TRUE(result.has_value());
}

TEST(AsExpected, AwaitingAnAwaiterLvalueRunsOnTheCallersObject)
{
	// as_expected wraps the awaiter the same way the cancellation check does. Holding it by
	// value would run await_resume on a copy, so the caller's object would never see the
	// write and the adapter would report a stale value rather than failing.
	struct writes_into_self
	{
		int observed = 0;

		bool await_ready() noexcept
		{
			return true;
		}
		void await_suspend(std::coroutine_handle<> handle) noexcept
		{
			(void)handle;
		}
		int await_resume() noexcept
		{
			observed = 99;
			return observed;
		}
	};

	writes_into_self subject;

	auto       adapted = alt::as_expected(subject);
	auto       awaiter = std::move(adapted).operator co_await();
	const auto result  = awaiter.await_resume();

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, 99);
	EXPECT_EQ(subject.observed, 99);
}

TEST(AsExpected, AwaitingAMoveOnlyAwaiterLvalueCompiles)
{
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
		void await_suspend(std::coroutine_handle<> handle) noexcept
		{
			(void)handle;
		}
		int await_resume() noexcept
		{
			return *payload;
		}
	};

	move_only_awaiter subject;

	auto       adapted = alt::as_expected(subject);
	auto       awaiter = std::move(adapted).operator co_await();
	const auto result  = awaiter.await_resume();

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, 7);

	// Referenced, not moved from.
	ASSERT_TRUE(subject.payload != nullptr);
	EXPECT_EQ(*subject.payload, 7);
}

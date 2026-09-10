#include <gtest/gtest.h>

#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <coroutine>
#include <exception>
#include <stop_token>
#include <utility>

#include "coroutine_test_support.hpp"

using namespace alt_test;

namespace
{

	// A minimal self-owning root, standing in for alt::cancellable_task.
	struct test_root
	{
		struct promise_type
		{
			alt::detail::cancellation_context context{};
			std::stop_source                  source;

			promise_type()
			{
				context.token = source.get_token();
			}

			test_root get_return_object() noexcept
			{
				context.root = std::coroutine_handle<promise_type>::from_promise(*this);
				return test_root{.source = source, .context = &context};
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

			template<typename A>
			auto await_transform(A&& a)
			{
				return alt::detail::make_cancellation_checked(std::forward<A>(a), &context);
			}
		};

		std::stop_source                   source;
		alt::detail::cancellation_context* context;
	};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool body_ran_past_cancellation = false;

	template<typename Park>
	test_root parking_root()
	{
		const frame_tracker t{1};
		co_await Park{};
		co_await Park{};
		body_ran_past_cancellation = true;
	}

	test_root parking_root_with_home()
	{
		const frame_tracker t{1};
		co_await park{};
		co_await park{};
		body_ran_past_cancellation = true;
	}

} // namespace

class CancellationTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
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
	EXPECT_EQ(live_frames, 1);

	resume_parked(); // past the first await, parks at the second
	resume_parked(); // past the second await, runs to completion

	EXPECT_TRUE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, CancelledCoroutineSelfDestructsAtItsNextSuspendPoint)
{
	auto root = parking_root<park>();
	EXPECT_EQ(live_frames, 1);

	root.source.request_stop();
	resume_parked(); // observes cancellation at the second await

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterUsesSymmetricTransfer)
{
	auto root = parking_root<park_by_transfer>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterReturnsBool)
{
	auto root = parking_root<park_by_bool>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, ATokenSwappedInWhileParkedIsObservedOnResume)
{
	// White-box: the context is the single source of truth for the signal, so
	// replacing its token mid-flight must change what the next suspend point sees.
	std::stop_source external;
	external.request_stop();

	auto root           = parking_root<park>();
	root.context->token = external.get_token();

	// The root already parked at its first await, so the swapped-in token must be
	// observed at the second one.
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, HomeSchedulerDefersDestructionUntilItIsPumped)
{
	alt::manual_scheduler home;
	auto                  root = parking_root_with_home();
	root.context->home         = alt::scheduler_ref{home};

	root.source.request_stop();
	resume_parked();

	// The destroy was posted, not performed: the frame is still alive.
	EXPECT_EQ(live_frames, 1);
	EXPECT_FALSE(home.empty());

	EXPECT_EQ(home.run_all(), 1U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(body_ran_past_cancellation);
}

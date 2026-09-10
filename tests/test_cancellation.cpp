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

	test_root parking_root_then_ready()
	{
		const frame_tracker t{1};
		co_await park{};
		co_await already_ready{};
		body_ran_past_cancellation = true;
	}

	test_root parking_root_then_typed_transfer()
	{
		const frame_tracker t{1};
		co_await park{};
		co_await park_by_typed_transfer{};
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

	// Ordering check: resume_parked() clears alt_test::parked before resuming, so
	// an empty handle here means the inner park's await_suspend never ran. If the
	// wrapper ever delegated to the inner awaiter before checking cancellation,
	// this would be the second parked handle instead of empty.
	EXPECT_FALSE(static_cast<bool>(alt_test::parked));
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterUsesSymmetricTransfer)
{
	auto root = parking_root<park_by_transfer>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);

	// See the ordering-check comment above: the inner awaiter must not have run.
	EXPECT_FALSE(static_cast<bool>(alt_test::parked));
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterReturnsBool)
{
	auto root = parking_root<park_by_bool>();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);

	// See the ordering-check comment above: the inner awaiter must not have run.
	EXPECT_FALSE(static_cast<bool>(alt_test::parked));
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

TEST_F(CancellationTest, CancelledCoroutineDoesNotRunPastAnAlreadyReadyAwait)
{
	// The inner awaiter here is already complete (await_ready() == true). Without
	// the wrapper's own cancellation check in await_ready, a cancelled coroutine
	// would sail straight through this await and run its body regardless.
	auto root = parking_root_then_ready();
	EXPECT_EQ(live_frames, 1);

	root.source.request_stop();
	resume_parked(); // past the first await; the second await is already ready

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationTest, SelfDestructsWhenTheInnerAwaiterReturnsATypedHandle)
{
	// The inner awaiter's await_suspend declares std::coroutine_handle<unrelated_promise>
	// rather than the erased handle. This only compiles because suspend_result_t
	// widens a handle-returning inner to the erased type; against the un-widened
	// alias, returning std::noop_coroutine() from the cancelled branch would be a
	// hard compile error here, since std::noop_coroutine_handle does not convert
	// to std::coroutine_handle<unrelated_promise>.
	auto root = parking_root_then_typed_transfer();
	root.source.request_stop();
	resume_parked();

	EXPECT_FALSE(body_ran_past_cancellation);
	EXPECT_EQ(live_frames, 0);
}

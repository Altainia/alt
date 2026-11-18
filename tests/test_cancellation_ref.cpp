#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/cancellation_ref.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <coroutine>
#include <exception>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

using namespace alt_test;

static_assert(std::is_trivially_copyable_v<alt::cancellation_ref>);
static_assert(sizeof(alt::cancellation_ref) == sizeof(void*));

namespace
{

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	alt::cancellation_ref recorded_view{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool view_was_recorded = false;

	// Parks its handle and records the view the accessor builds from the typed handle.
	struct view_recorder
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		void await_suspend(std::coroutine_handle<Promise> h) const noexcept
		{
			parked            = h;
			recorded_view     = alt::get_cancellation_ref(h);
			view_was_recorded = true;
		}

		void await_resume() const noexcept
		{}
	};

	// Takes the erased handle, which has no promise() to build a view from.
	struct erased_view_recorder
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked            = h;
			recorded_view     = alt::get_cancellation_ref(h);
			view_was_recorded = true;
		}

		void await_resume() const noexcept
		{}
	};

	// Posts its own resumption so alt::sync_wait's pump can drive it.
	struct posting_view_recorder
	{
		alt::manual_scheduler* scheduler;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		void await_suspend(std::coroutine_handle<Promise> h) const
		{
			recorded_view     = alt::get_cancellation_ref(h);
			view_was_recorded = true;
			scheduler->post(alt::work_item{.invoke = [](void* address) noexcept { std::coroutine_handle<>::from_address(address).resume(); }, .state = h.address()});
		}

		void await_resume() const noexcept
		{}
	};

	alt::cancellable_task cancellable_recording()
	{
		const frame_tracker t{1};
		co_await view_recorder{};
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool body_ran_after_await = false;

	alt::cancellable_task cancellable_recording_then_marking()
	{
		const frame_tracker t{1};
		co_await view_recorder{};
		body_ran_after_await = true;
	}

	// Starts a chain, records its view and abandons it with no stop requested. Never
	// returns in a build with asserts; lives here so the parent test process builds no
	// frame of its own, which the death test would otherwise leak.
	void abandon_a_chain_nobody_stopped()
	{
		const auto handle = alt::start(cancellable_recording());
		recorded_view.abandon();
	}

	alt::cancellable_task cancellable_recording_through_erased_handle()
	{
		const frame_tracker t{1};
		co_await erased_view_recorder{};
	}

	alt::task<void> plain_recording(alt::manual_scheduler* scheduler)
	{
		const frame_tracker t{1};
		co_await posting_view_recorder{scheduler};
	}

	// A minimal self-owning root with a hand-rolled context, as in test_cancellation.cpp.
	struct unstoppable_root
	{
		struct promise_type
		{
			alt::detail::cancellation_context context{};

			unstoppable_root get_return_object() noexcept
			{
				context.root = std::coroutine_handle<promise_type>::from_promise(*this);
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

			// The accessor the relays_cancellation concept looks for.
			alt::detail::cancellation_context* cancellation_context_pointer() noexcept
			{
				return &context;
			}
		};
	};

	unstoppable_root root_with_default_token()
	{
		const frame_tracker t{1};
		co_await view_recorder{};
	}

	using alt_test::asserts_are_compiled_in;
	using alt_test::scoped_death_test_style;
	using alt_test::thread_sanitizer_forks_death_tests;
	using alt_test::thread_sanitizer_is_active;

	/** Why a death test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "the abandon and accessor preconditions are asserts, and NDEBUG compiles them away, so this "
	  "build has no guard for a death test to observe.";

} // namespace

class CancellationRefTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
		recorded_view     = alt::cancellation_ref{};
		view_was_recorded = false;
	}
};

TEST_F(CancellationRefTest, ADefaultConstructedViewIsEmpty)
{
	EXPECT_FALSE(static_cast<bool>(alt::cancellation_ref{}));
	EXPECT_FALSE(alt::cancellation_ref{}.stop_requested());
	EXPECT_FALSE(alt::cancellation_ref{}.get_stop_token().stop_possible());
}

TEST_F(CancellationRefTest, AViewInsideACancellableTaskIsNotEmpty)
{
	auto handle = alt::start(cancellable_recording());

	ASSERT_TRUE(view_was_recorded);
	EXPECT_TRUE(static_cast<bool>(recorded_view));
	EXPECT_TRUE(recorded_view.get_stop_token().stop_possible());
	EXPECT_FALSE(recorded_view.stop_requested());

	parked.resume();
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AViewInsideAPlainTaskUnderSyncWaitIsEmpty)
{
	alt::manual_scheduler scheduler;

	alt::sync_wait(plain_recording(&scheduler), scheduler);

	ASSERT_TRUE(view_was_recorded);
	EXPECT_FALSE(static_cast<bool>(recorded_view));
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, TheViewCarriesTheChainsStopToken)
{
	auto handle = alt::start(cancellable_recording());
	ASSERT_TRUE(view_was_recorded);

	const std::stop_token token = recorded_view.get_stop_token();
	EXPECT_FALSE(recorded_view.stop_requested());
	EXPECT_FALSE(token.stop_requested());

	handle.request_stop();

	EXPECT_TRUE(recorded_view.stop_requested());
	EXPECT_TRUE(token.stop_requested());
	EXPECT_TRUE(recorded_view.get_stop_token().stop_requested());

	// Resuming completes the body; the chain is not abandoned by observation alone.
	parked.resume();
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AViewOverANonStoppableTokenIsNotEmptyButCannotStop)
{
	root_with_default_token();

	ASSERT_TRUE(view_was_recorded);
	EXPECT_TRUE(static_cast<bool>(recorded_view));
	EXPECT_FALSE(recorded_view.get_stop_token().stop_possible());
	EXPECT_FALSE(recorded_view.stop_requested());

	parked.resume();
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AnAwaiterTakingTheErasedHandleGetsAnEmptyView)
{
	// std::coroutine_handle<> has no promise() to ask, so the accessor can only
	// answer with the empty view, even inside a cancellable_task.
	auto handle = alt::start(cancellable_recording_through_erased_handle());

	ASSERT_TRUE(view_was_recorded);
	EXPECT_FALSE(static_cast<bool>(recorded_view));

	parked.resume();
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AbandonTearsTheChainDown)
{
	body_ran_after_await = false;

	auto handle = alt::start(cancellable_recording_then_marking());
	ASSERT_TRUE(view_was_recorded);
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	recorded_view.abandon();

	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(body_ran_after_await);
}

TEST_F(CancellationRefTest, AbandonPostsTheTeardownToHome)
{
	body_ran_after_await = false;
	alt::manual_scheduler pump;

	auto handle = alt::start(cancellable_recording_then_marking(), {.home = alt::scheduler_ref{pump}});
	ASSERT_TRUE(view_was_recorded);

	handle.request_stop();
	recorded_view.abandon();

	// Posted, not run: the frames stay suspended until the home scheduler gets to them.
	EXPECT_GT(live_frames, 0);

	pump.drain();

	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(body_ran_after_await);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body. Measured with the pinned clang-tidy-19 at threshold 0 with the suppression removed: each of the three death-test bodies in this file reports a cognitive complexity of 39. What a body actually contains is two skip guards and one expectation.
TEST(CancellationRefDeathTest, AbandoningAnEmptyViewAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const scoped_death_test_style style{"threadsafe"};

	EXPECT_DEATH(alt::cancellation_ref{}.abandon(), "abandoning an empty alt::cancellation_ref");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH, not this body; see the first death test in this file, whose suppression carries the measured 39.
TEST(CancellationRefDeathTest, AbandoningAChainWithNoStopRequestedAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const scoped_death_test_style style{"threadsafe"};

	EXPECT_DEATH(abandon_a_chain_nobody_stopped(), "abandoning an alt chain with no stop requested");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH, not this body; see the first death test in this file, whose suppression carries the measured 39.
TEST(CancellationRefDeathTest, GettingAViewFromAnEmptyHandleAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const scoped_death_test_style style{"threadsafe"};

	// Typed, with a promise that relays cancellation, so the accessor's own assert is
	// the one that fires rather than the erased-handle path.
	EXPECT_DEATH(static_cast<void>(alt::get_cancellation_ref(std::coroutine_handle<unstoppable_root::promise_type>{})), "getting a cancellation_ref from an empty coroutine handle");
}

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
#include <type_traits>
#include <utility>

#include "coroutine_test_support.hpp"

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

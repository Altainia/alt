#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/cancellation_ref.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/when_all.hpp>
#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <latch>
#include <optional>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
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

	// What an operation does when it completes. The two values are the only difference
	// between the mutation pair.
	enum class completion
	{
		resume_or_abandon,
		plain_resume,
	};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	int operation_starts = 0;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool completion_resumed = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	int awaited_result = 0;

	// Set on the line before the co_await, so a test can tell a body that reached its
	// suspension point from one that never ran.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool body_reached_await = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	completion pending_completion = completion::resume_or_abandon;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	alt::cancellation_ref pending_view{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	std::coroutine_handle<> pending_handle{};

	// Finishes the parked operation the way the awaiter was told to. Nothing here may
	// touch the awaiter afterwards: either branch can free the frame it lives in.
	void complete_pending_operation() noexcept
	{
		const alt::cancellation_ref   view = pending_view;
		const std::coroutine_handle<> h    = pending_handle;
		const completion              mode = pending_completion;

		if(mode == completion::resume_or_abandon)
		{
			completion_resumed = view.resume_or_abandon(h);
		}
		else
		{
			h.resume();
			completion_resumed = true;
		}
	}

	// Parks until the test (or a scheduler) calls complete_pending_operation.
	struct completing_awaiter
	{
		completion             on_completion;
		alt::manual_scheduler* scheduler = nullptr;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		void await_suspend(std::coroutine_handle<Promise> h) const
		{
			++operation_starts;
			pending_completion = on_completion;
			pending_view       = alt::get_cancellation_ref(h);
			pending_handle     = h;

			if(scheduler != nullptr)
			{
				scheduler->post(alt::work_item{.invoke = [](void*) noexcept { complete_pending_operation(); }, .state = nullptr});
			}
		}

		[[nodiscard]] int await_resume() const noexcept
		{
			return 42;
		}
	};

	alt::cancellable_task cancellable_completing(completion on_completion)
	{
		const frame_tracker t{1};
		body_reached_await   = true;
		awaited_result       = co_await completing_awaiter{.on_completion = on_completion};
		body_ran_after_await = true;
	}

	alt::task<void> plain_completing(alt::manual_scheduler* scheduler)
	{
		const frame_tracker t{1};
		awaited_result       = co_await completing_awaiter{.on_completion = completion::resume_or_abandon, .scheduler = scheduler};
		body_ran_after_await = true;
	}

	// Registers a std::stop_callback on the token the view hands it, whose body completes
	// the operation with resume_or_abandon: the adapter shape, where a stop request drives
	// the operation to its end synchronously on the requesting thread. A std::stop_callback
	// has no move constructor, so this awaiter is immovable and is awaited as a named lvalue.
	struct stop_callback_awaiter
	{
		std::optional<std::stop_callback<std::function<void()>>> callback;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		void await_suspend(std::coroutine_handle<Promise> h)
		{
			const alt::cancellation_ref view = alt::get_cancellation_ref(h);

			recorded_view     = view;
			view_was_recorded = true;
			parked            = h;

			callback.emplace(view.get_stop_token(), [view, h]() noexcept { static_cast<void>(view.resume_or_abandon(h)); });
		}

		void await_resume() const noexcept
		{}
	};

	static_assert(!std::is_move_constructible_v<stop_callback_awaiter>);

	alt::cancellable_task cancellable_with_a_stop_callback()
	{
		const frame_tracker t{1};
		body_reached_await = true;
		stop_callback_awaiter awaiter;
		co_await awaiter;
		body_ran_after_await = true;
	}

	// The same adapter shape, except that the abandoning std::stop_callback is NOT the
	// only registration on the chain's own token. Registration order is load-bearing:
	// libstdc++ walks the callback list in reverse registration order, so the no-op is
	// emplaced first to put the abandoning one at the head of the list. The abandoning
	// one then runs first and frees the chain, and the walk still has the no-op left to
	// reach -- which is where the chain's stop state gets touched after the teardown
	// unless something outside the destroyed frames owns it. Both callbacks live in the
	// awaiter, i.e. in a frame the teardown destroys.
	struct two_callback_awaiter
	{
		std::optional<std::stop_callback<std::function<void()>>> noop;
		std::optional<std::stop_callback<std::function<void()>>> abandoner;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		void await_suspend(std::coroutine_handle<Promise> h)
		{
			const alt::cancellation_ref view = alt::get_cancellation_ref(h);

			recorded_view     = view;
			view_was_recorded = true;
			parked            = h;

			noop.emplace(view.get_stop_token(), std::function<void()>{[] {}});
			abandoner.emplace(view.get_stop_token(), std::function<void()>{[view, h] { static_cast<void>(view.resume_or_abandon(h)); }});
		}

		void await_resume() const noexcept
		{}
	};

	alt::cancellable_task cancellable_with_two_stop_callbacks()
	{
		const frame_tracker t{1};
		body_reached_await = true;
		two_callback_awaiter awaiter;
		co_await awaiter;
		body_ran_after_await = true;
	}

	// What the when_all in combinator_root reported.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	bool combinator_reported = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state
	std::optional<std::error_code> combinator_error{};

	// The opted-in awaiter, inside a user awaitable that when_all runs as its own root.
	alt::task<int> combinator_child()
	{
		const frame_tracker t{1};
		const int           value = co_await completing_awaiter{.on_completion = completion::resume_or_abandon};
		co_return value;
	}

	alt::cancellable_task combinator_root()
	{
		const frame_tracker t{1};
		const auto          result = co_await alt::when_all(combinator_child());
		combinator_reported        = true;
		if(!result)
		{
			combinator_error = result.error();
		}
		co_await alt::cancellation_point{};
	}

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
		recorded_view        = alt::cancellation_ref{};
		view_was_recorded    = false;
		operation_starts     = 0;
		completion_resumed   = false;
		awaited_result       = 0;
		body_reached_await   = false;
		body_ran_after_await = false;
		pending_view         = alt::cancellation_ref{};
		pending_handle       = {};
		combinator_reported  = false;
		combinator_error.reset();
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

TEST_F(CancellationRefTest, ABodyDoesNotRunWhenTheAwaiterAbandons)
{
	auto handle = alt::start(cancellable_completing(completion::resume_or_abandon));
	ASSERT_EQ(operation_starts, 1);
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	complete_pending_operation();

	EXPECT_FALSE(completion_resumed);
	EXPECT_FALSE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
}

// The mutation of record for the test above: the same awaiter, with only its completion
// swapped for a plain resume. Without it the test above is consistent with the body never
// having been reachable at all.
TEST_F(CancellationRefTest, ABodyDoesRunWhenTheAwaiterResumesInstead)
{
	auto handle = alt::start(cancellable_completing(completion::plain_resume));
	ASSERT_EQ(operation_starts, 1);
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	complete_pending_operation();

	EXPECT_TRUE(completion_resumed);
	EXPECT_TRUE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, ResumeOrAbandonResumesWhenNoStopWasRequested)
{
	auto handle = alt::start(cancellable_completing(completion::resume_or_abandon));
	ASSERT_EQ(operation_starts, 1);

	complete_pending_operation();

	EXPECT_TRUE(completion_resumed);
	EXPECT_TRUE(body_ran_after_await);
	EXPECT_EQ(awaited_result, 42);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, ResumeOrAbandonResumesWhenTheViewIsEmpty)
{
	alt::manual_scheduler scheduler;

	alt::sync_wait(plain_completing(&scheduler), scheduler);

	EXPECT_FALSE(static_cast<bool>(pending_view));
	EXPECT_TRUE(completion_resumed);
	EXPECT_TRUE(body_ran_after_await);
	EXPECT_EQ(awaited_result, 42);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AStopBeforeTheAwaitStopsTheOperationStarting)
{
	// A token that is already stopped stops the chain before the body runs. The body
	// reaching its co_await while the awaiter is never started is what shows the chain
	// was torn down at the suspension point, by the check that was already there.
	std::stop_source source;
	source.request_stop();

	ASSERT_FALSE(body_reached_await);

	auto handle = alt::start(cancellable_completing(completion::resume_or_abandon), {.token = source.get_token()});

	EXPECT_TRUE(body_reached_await);
	EXPECT_EQ(operation_starts, 0);
	EXPECT_FALSE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH, not this body; see the first death test in this file, whose suppression carries the measured 39.
TEST(CancellationRefDeathTest, ResumeOrAbandonWithAnEmptyHandleAborts)
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

	EXPECT_DEATH(static_cast<void>(alt::cancellation_ref{}.resume_or_abandon(std::coroutine_handle<>{})), "resuming or abandoning an alt chain with an empty coroutine handle");
}

TEST_F(CancellationRefTest, ATeardownFromInsideRequestStopIsSafe)
{
	// The adapter's primary path: the stop callback's body tears the chain down while
	// request_stop() is still walking the stop state's callback list, destroying the root
	// frame and the leaf frame holding the callback that is executing.
	auto handle = alt::start(cancellable_with_a_stop_callback());
	ASSERT_TRUE(body_reached_await);
	ASSERT_TRUE(view_was_recorded);
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();

	EXPECT_FALSE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(handle.stop_requested());
}

TEST_F(CancellationRefTest, ATeardownFromInsideALinkedTokensRequestStopIsSafe)
{
	// The stop arrives through start_options::token with the chain's own handle
	// discarded, so the teardown destroys the external-link callback while it is itself
	// mid-invocation.
	std::stop_source source;

	static_cast<void>(alt::start(cancellable_with_a_stop_callback(), {.token = source.get_token()}));
	ASSERT_TRUE(body_reached_await);
	ASSERT_TRUE(view_was_recorded);
	ASSERT_EQ(live_frames, 1);

	EXPECT_TRUE(source.request_stop());

	EXPECT_FALSE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(source.stop_requested());
	EXPECT_TRUE(source.get_token().stop_requested());
	EXPECT_FALSE(source.request_stop());
}

TEST_F(CancellationRefTest, ATeardownWithASecondCallbackOnTheChainsTokenIsSafe)
{
	// The regression test for the counted local in detail::stop_forwarder::operator()
	// (include/alt/cancellable_task.hpp). It pins the one shape where every owner of the
	// chain's stop state lives inside the frames the teardown destroys: the stop arrives
	// through start_options::token, the chain's own cancellation_handle is discarded, so
	// the only owners are the promise's m_source, m_context.token and the stop_forwarder
	// inside m_external -- all in the root frame. The awaiter registers two callbacks on
	// the chain's token so that libstdc++'s walk has somewhere left to go after the
	// abandoning one has freed those frames; with only the abandoning callback
	// registered, _M_request_stop's "last callback" early return hides the freed state
	// and the test above passes either way.
	//
	// The arrangement depends on an invocation order the standard does not specify, so
	// whether it discriminates anything on MSVC was an open question (issue #47) until
	// both libraries were checked. Both walk in reverse registration order, so it
	// discriminates on both:
	//
	//   - libstdc++ registers at the head of _M_callbacks and _M_request_stop takes the
	//     head, so the abandoning callback -- emplaced second -- runs first.
	//   - MSVC's STL does the same: _Stop_callback_base::_Do_attach sets _Head = this,
	//     and _Stop_state::_Request_stop pops the head each iteration of its for(;;)
	//     (stl/inc/stop_token). Its loop then relocks the list to look for the next
	//     node, including after the last callback, which is the contact libstdc++'s
	//     "__last_cb" early return skips.
	//
	// This is clean ONLY because of that local, on both libraries, and not only under a
	// sanitizer. Measured by replacing the local with a direct target.request_stop():
	//
	//   - Linux, libstdc++ 15.2, Debug + ASan: heap-use-after-free, a READ of the freed
	//     stop state in _Stop_state_t::_M_lock() reached from stop_forwarder::operator();
	//     exactly 1 of the 24 CancellationRef cases fails, this one.
	//   - Linux, libstdc++ 15.2, plain Debug with no sanitizer: 40 runs of this case
	//     alone, 0 passed -- 26 segfaulted and 14 hung. A sanitizer makes the diagnosis
	//     legible; it is not what makes the case fail. An earlier version of this
	//     comment said the case belonged to the AddressSanitizer gate above all, which
	//     this measurement falsifies: every Linux gate catches it.
	//   - MSVC 14.51, debug-asan: the same shape, heap-use-after-free, READ of 8 bytes
	//     in _Locked_pointer<_Stop_callback_base>::_Lock_and_load() from
	//     _Stop_state::_Request_stop() (stop_token:94, the relock at the top of the
	//     loop) reached from stop_forwarder::operator(). 27 of 837 tests fail there, not
	//     1: the same missing keepalive also takes out ten when_all cases, nine when_any
	//     cases, ATeardownFromInsideALinkedTokensRequestStopIsSafe and a task_group
	//     case, because MSVC relocks after every callback and so needs the state alive
	//     in shapes where libstdc++'s early return does not. MSVC's release-tests
	//     preset, which has no sanitizer, passed the whole suite -- a use-after-free
	//     that reads mapped memory need not fail anything.
	//
	// The local is not this component's: main's f3f0a8b added it while porting to MSVC,
	// where request_stop() relocks the callback list after the LAST callback too and so
	// hangs without it, and this work rediscovered the same defect independently. That
	// commit pins the fix nowhere -- it touches cancellable_task.hpp,
	// combinator_state.hpp, projection.hpp and test_projection.cpp -- so this case is the
	// regression test for a fix that shipped without one, and the only one of the three
	// teardown shapes here that can fail on libstdc++ if the local is removed again.
	//
	// Re-measured against the merged tree, by replacing the local with a direct
	// target.request_stop(): exactly 1 of the 24 CancellationRef cases fails under
	// debug-asan, this one, with the report above at cancellable_task.hpp's
	// stop_forwarder::operator(). The other two teardown shapes pass either way.
	std::stop_source source;

	static_cast<void>(alt::start(cancellable_with_two_stop_callbacks(), {.token = source.get_token()}));
	ASSERT_TRUE(body_reached_await);
	ASSERT_TRUE(view_was_recorded);
	ASSERT_EQ(live_frames, 1);

	EXPECT_TRUE(source.request_stop());

	EXPECT_FALSE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(source.stop_requested());
	EXPECT_FALSE(source.request_stop());
}

TEST_F(CancellationRefTest, AnImmovableAwaiterWorksAsAnLvalue)
{
	// Issue #10's third limit. The awaiter is immovable (static_assert above), so a
	// prvalue co_await would not compile; a named lvalue is stored by reference.
	auto handle = alt::start(cancellable_with_a_stop_callback());
	ASSERT_TRUE(view_was_recorded);

	EXPECT_TRUE(static_cast<bool>(recorded_view));
	EXPECT_FALSE(body_ran_after_await);

	parked.resume();

	EXPECT_TRUE(body_ran_after_await);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, ACombinatorChildKeepsItsBlocksVerdict)
{
	// A satisfied verdict over an unfilled slot would abort in result_store, so reaching
	// the assertions below is part of the check.
	//
	// The discriminating assertion is EXPECT_FALSE(completion_resumed): the child's
	// awaiter abandoned rather than resumed. The error check is weaker than it looks,
	// because when_all reports a stop racing a completion as cancelled even with every
	// slot filled -- see the await_resume note in include/alt/when_all.hpp -- so
	// operation_canceled here does not by itself show the child abandoned.
	auto handle = alt::start(combinator_root());
	ASSERT_EQ(operation_starts, 1);

	handle.request_stop();
	complete_pending_operation();

	EXPECT_FALSE(completion_resumed);
	ASSERT_TRUE(combinator_reported);

	// The error is compared against the CONDITION rather than against a second
	// std::error_code built to match it, which include/alt/detail/async_wait.hpp asks
	// callers to do: equivalence is what the contract promises, and an exact
	// category-plus-value equality would pin generic_category as well.
	//
	// value_or rather than value() or operator*, and measured rather than guessed:
	// clang-tidy-19 does not accept the ASSERT_TRUE above as establishing the invariant
	// -- GoogleTest's ASSERT_ expansion is outside what its dataflow model follows -- so
	// both of those report bugprone-unchecked-optional-access and the pinned gate fails
	// on them. value_or is checked access, and the ASSERT_TRUE still carries the
	// empty-optional diagnostic, so nothing here rests on the default this never uses.
	ASSERT_TRUE(combinator_error.has_value());
	EXPECT_EQ(combinator_error.value_or(std::error_code{}), std::errc::operation_canceled);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AnOffThreadCompletionResumesSafely)
{
	// The resume branch, run off the test thread. The chain is never stopped, and the
	// join is what orders the thread's writes before the reads below.
	auto handle = alt::start(cancellable_completing(completion::resume_or_abandon));
	ASSERT_EQ(operation_starts, 1);

	std::thread::id completing_thread{};
	std::jthread    worker{[&completing_thread] {
    completing_thread = std::this_thread::get_id();
    complete_pending_operation();
  }};
	worker.join();

	EXPECT_NE(completing_thread, std::this_thread::get_id());
	EXPECT_TRUE(completion_resumed);
	EXPECT_TRUE(body_ran_after_await);
	EXPECT_EQ(awaited_result, 42);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(CancellationRefTest, AnOffThreadCompletionRacesAStopRequest)
{
	// The design's thread-crossing case, and the only test in this file where
	// resume_or_abandon's read of the stop state and abandon_chain's teardown actually
	// race a request_stop() on another thread. The test above orders everything with
	// join(), so it instruments the resume branch off-thread but no contention; this one
	// releases both sides together through a std::latch -- never a sleep, which would
	// only make the interleaving likely rather than contended.
	//
	// Which side wins is a genuine race, so the assertion is the disjunction rather than
	// a fixed outcome: the frames must be gone either way, and the body must have run if
	// and only if resume_or_abandon resumed rather than abandoned. Asserting a particular
	// winner here would be asserting a scheduling accident.
	//
	// This one matters most under ThreadSanitizer. The jthread's join at scope exit is
	// what orders the worker's writes before the reads below.
	//
	// Both branches tear the chain down on the WORKER thread, and that is load-bearing
	// for ThreadSanitizer cleanliness rather than incidental: completing_awaiter
	// registers no std::stop_callback, and alt::start is called with no
	// start_options::token, so the request_stop() below has no callback to run and only
	// sets the flag. Whichever branch resume_or_abandon then takes, it takes on the
	// worker, inside complete_pending_operation() -- so the frame_tracker destructors
	// that append to alt_test::destruction_order (a plain std::vector, see
	// tests/coroutine_test_support.hpp) all run on one thread, ordered against the reads
	// below by the join. An awaiter variant that DID register a stop callback on the
	// chain's token would tear down on the main thread instead, concurrently with the
	// worker, and would need that vector guarded before it could be written this way.
	//
	// Vacuity: because the assertion is a disjunction, a machine or toolchain whose
	// timing always lets one side win would still pass, and would do so silently. That
	// costs no branch coverage -- ABodyDoesNotRunWhenTheAwaiterAbandons and
	// ABodyDoesRunWhenTheAwaiterResumesInstead pin both branches deterministically, and
	// are the mutation pair for them. What a one-sided machine would lose is contention
	// on the abandon path, so the branch actually taken is recorded as a test property
	// below, making each run say which one it exercised instead of leaving it to be
	// inferred.
	//
	// Census of the COMPILED case, not of a standalone harness: see the figure recorded
	// at the RecordProperty call below.
	auto handle = alt::start(cancellable_completing(completion::resume_or_abandon));
	ASSERT_EQ(operation_starts, 1);

	std::latch both_ready{2};

	{
		const std::jthread worker{[&both_ready] {
			both_ready.arrive_and_wait();
			complete_pending_operation();
		}};

		both_ready.arrive_and_wait();
		handle.request_stop();
	}

	EXPECT_TRUE(handle.stop_requested());
	EXPECT_EQ(live_frames, 0);

	// Which side won, as a GoogleTest property, so the run is self-describing: it reaches
	// ctest's XML (ctest --output-junit, or the binary's own --gtest_output=xml) without
	// adding a line to the output of a passing run. It is also the census instrument --
	// run this case alone N times and count the values:
	//
	//   F=CancellationRefTest.AnOffThreadCompletionRacesAStopRequest
	//   for i in $(seq 1 2000); do
	//     alt_tests --gtest_filter=$F --gtest_output=xml:c.xml >/dev/null
	//     grep -o 'value="[a-z]*"' c.xml
	//   done | sort | uniq -c
	//
	// Both branches are reached in the compiled case, not merely in a standalone
	// harness: 2000 runs of the debug build gave 1988 resumed / 12 abandoned, and 500
	// runs of the debug-tsan build gave 424 resumed / 76 abandoned, with no failures in
	// either. The abandon path is rarer in the debug build (0.6%) than under
	// ThreadSanitizer (15.2%), which is the right way round -- TSan's instrumentation
	// widens the window this test is trying to hit, and TSan is the gate this case
	// exists for.
	RecordProperty("resume_or_abandon_branch", completion_resumed ? "resumed" : "abandoned");

	// The disjunction: resumed means the body ran, abandoned means it did not.
	EXPECT_EQ(body_ran_after_await, completion_resumed);
}

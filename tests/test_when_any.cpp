#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/when_any_result.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/task_completion_source.hpp>
#include <alt/when_any.hpp>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include "coroutine_test_support.hpp"

using namespace alt_test;

namespace
{
	/** The condition every released wait reports. See alt::wait_result. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/**
	 * Drives detail::when_any_awaiter directly, standing in for the awaitable the
	 * public alt::when_any overloads own.
	 *
	 * The awaiter is this task's deliverable; the two public overloads and their
	 * awaitables are a later task's, so there is nothing yet that owns the argument
	 * tuple for the awaiter to point into. This class is that owner and nothing more:
	 * it is the same three type aliases and the same rvalue-only operator co_await
	 * detail::when_all_awaitable has, with no overload resolution, no concept
	 * constraints and no range form. When the public overloads land, these tests keep
	 * working through this class and the new ones exercise that one; the point of
	 * keeping it is that it awaits the awaiter with nothing else in the way.
	 *
	 * Stored by value inside detail::cancellation_checked_awaitable, which lives for
	 * the whole full-expression, so the pointer the awaiter holds into the tuple is
	 * stable.
	 */
	template<typename... Awaitables>
	class [[nodiscard]] race
	{
	public:
		/** The result a satisfied race yields: the winner's position and its value. */
		using result_type = alt::when_any_result<alt::detail::when_any_value_t<Awaitables>...>;

		/** The slot each child writes into. */
		using slots_type = std::tuple<alt::detail::when_any_slot_t<Awaitables>...>;

		/** A value per rvalue argument, a reference per lvalue one. */
		using awaitables_type = std::tuple<Awaitables...>;

		explicit race(Awaitables&&... awaitables): m_awaitables(std::forward<Awaitables>(awaitables)...)
		{
		}

		auto operator co_await() && noexcept
		{
			return alt::detail::when_any_awaiter<result_type, slots_type, awaitables_type>{&m_awaitables};
		}

	private:
		awaitables_type m_awaitables;
	};

	/** Deduces a race over @p awaitables, keeping each lvalue a reference. */
	template<typename... Awaitables>
	[[nodiscard]] race<Awaitables...> make_race(Awaitables&&... awaitables)
	{
		return race<Awaitables...>{std::forward<Awaitables>(awaitables)...};
	}

	/** What one awaiting chain recorded about its race. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the await reported a satisfied wait. */
		bool satisfied = false;

		/** The error a released await reported. */
		std::error_code error;

		/** The thread the statement after the co_await ran on. */
		std::thread::id resumed_on;
	};

	/** Records how the await ended, whatever the result's value half holds. */
	template<typename Result>
	void record_outcome(wait_record& record, const Result& result)
	{
		++record.resumptions;
		record.satisfied  = result.has_value();
		record.resumed_on = std::this_thread::get_id();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/** A task that completes synchronously with @p value. */
	template<typename T>
	alt::task<T> make_value_task(T value)
	{
		co_return std::move(value);
	}

	/** A tracked task that parks on @p gate and hands back how its wait ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<alt::wait_result> tracked_wait(alt::async_event& gate, int tag)
	{
		const frame_tracker    tracker{tag};
		const alt::wait_result result = co_await gate;
		co_return result;
	}

	/** Races two int sources and records which position won. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task two_source_root(alt::task_completion_source<int>& first, alt::task_completion_source<int>& second, wait_record& record, std::size_t& winner, int& value)
	{
		const frame_tracker tracker{1};
		const auto          result = co_await make_race(first, second);
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();

			if(winner == 1)
			{
				value = result->template get<1>().value();
			}
		}

		// Somewhere for a cancelled chain to tear its frames down.
		co_await alt::cancellation_point{};
	}

	/** Races an int source against a move-only one and takes the winner's value out. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task move_only_root(alt::task_completion_source<int>& first, alt::task_completion_source<std::unique_ptr<int>>& second, wait_record& record, std::size_t& winner, std::unique_ptr<int>& taken)
	{
		const frame_tracker tracker{1};
		auto                result = co_await make_race(first, second);
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();

			if(winner == 1)
			{
				// The rvalue overload, and the std::move is what makes a move-only winner
				// reachable at all: the const-lvalue get<I>() yields a const T&.
				taken = std::move(*result).template get<1>().value();
			}
		}

		co_await alt::cancellation_point{};
	}

	/** Races two tracked children and reads live_frames at the instant the await returns. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task tracked_race_root(alt::async_event& first, alt::async_event& second, wait_record& record, std::size_t& winner, int& frames_at_return)
	{
		const auto result = co_await make_race(tracked_wait(first, 1), tracked_wait(second, 2));

		// Read before anything else, because this instant is the only place the
		// property is observable: by the time the requesting thread is back, every
		// child's body has finished either way.
		frames_at_return = live_frames.load(std::memory_order_acquire);
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
		}

		co_await alt::cancellation_point{};
	}

	/** Races two children that both complete inside the fan-out. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the record is owned by the calling test, which blocks in sync_wait until this completes.
	alt::task<void> ready_children_probe(wait_record& record, std::size_t& winner, int& value)
	{
		const auto result = co_await make_race(make_value_task(1), make_value_task(2));
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();

			if(winner == 0)
			{
				value = result->template get<0>();
			}
		}
	}
}

// What awaiting a race yields. The result travels in a std::expected, because
// cancellation is an observable result rather than a torn-down frame.
using two_int_sources = decltype(make_race(std::declval<alt::task_completion_source<int>&>(), std::declval<alt::task_completion_source<int>&>()));
using int_and_void    = decltype(make_race(std::declval<alt::task<int>&&>(), std::declval<alt::task<void>&&>()));

static_assert(alt::awaitable<two_int_sources>);
static_assert(std::same_as<alt::await_result_t<two_int_sources>,
                           std::expected<alt::when_any_result<alt::completion_result<int>, alt::completion_result<int>>, std::error_code>>);

// A void child occupies its position as std::monostate, the same mapping when_all uses.
static_assert(std::same_as<alt::await_result_t<int_and_void>, std::expected<alt::when_any_result<int, std::monostate>, std::error_code>>);

class WhenAnyTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(WhenAnyTest, TheFirstChildToCompleteIsTheWinner)
{
	alt::task_completion_source<int> first;
	alt::task_completion_source<int> second;
	wait_record                      record;
	std::size_t                      winner = 99;
	int                              value  = 0;

	auto handle = alt::start(two_source_root(first, second, record, winner, value));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1);

	// Position 1 is the only child that completes with a value, and it elects before
	// position 0 is resumed at all: setting the source resumes position 1, which stores
	// and wins, and only then does its request_children_stop() resume position 0
	// released. Position 0 does reach its own election and lose it. So the winner is
	// decided by completion order rather than by argument order, which is the whole
	// difference between this component and when_all, and the order is deterministic
	// here rather than raced for.
	second.set_value(2);

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U) << "the second argument completed first, so it is the winner";
	EXPECT_EQ(value, 2);
	EXPECT_EQ(live_frames, 0);

	// The loser was resumed released by the winner's request_children_stop(), ran its
	// body to the end and retired, which is why the chain was resumed at all: the
	// counter reaches zero only once every child has retired, winner or not.
	EXPECT_FALSE(handle.stop_requested()) << "the chain itself was never cancelled";
}

TEST_F(WhenAnyTest, TheWinnersValueAndIndexComeBack)
{
	alt::task_completion_source<int>                  first;
	alt::task_completion_source<std::unique_ptr<int>> second;
	wait_record                                       record;
	std::size_t                                       winner = 99;
	std::unique_ptr<int>                              taken;

	const auto handle = alt::start(move_only_root(first, second, record, winner, taken));
	ASSERT_EQ(record.resumptions, 0);

	second.set_value(std::make_unique<int>(7));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U);

	// A move-only winner came out of the result, which only the rvalue get<I>()
	// overload can do: alt::task<std::unique_ptr<T>> is a legal child.
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 7);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAnyTest, NoChildIsRunningWhenAWonAwaitReturns)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;
	std::size_t      winner           = 99;
	int              frames_at_return = -1;

	const auto handle = alt::start(tracked_race_root(first, second, record, winner, frames_at_return));
	ASSERT_EQ(live_frames, 2) << "both children are parked on their own event";
	ASSERT_EQ(record.resumptions, 0);

	second.set();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U);

	// The property this test exists for. Read at the instant the await returned, so
	// it says the loser's body had already run to its end by then -- not merely that
	// its frame was freed, which live_frames cannot see. An await that returned on the
	// winner's election, without waiting for the counter to drain, leaves this at 1.
	EXPECT_EQ(frames_at_return, 0) << "the await returned with a child's body still running";
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAnyTest, EveryChildAlreadyCompleteYieldsAWinnerWithoutSuspending)
{
	wait_record record;
	std::size_t winner = 99;
	int         value  = 0;

	// Both children are already-complete tasks, but the name's literal reading is not
	// reachable in a race and the test does not pretend otherwise: position 0 completes
	// inside its own start, elects, and its win stops the children, so position 1 is
	// started on an already-stopped token and dies at its first suspension point
	// without completing. What matters is the same either way -- both children retire
	// during the fan-out, so the count reaches zero as the starter retires and the join
	// refuses the link.
	// Nothing else in this test can resume the probe -- no second thread, no primitive
	// and no parked handle -- so sync_wait returning at all is the proof that
	// await_suspend returned false and handed the frame straight back.
	alt::sync_wait(ready_children_probe(record, winner, value));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 0U) << "the first child to complete wins, and here that is the first started";
	EXPECT_EQ(value, 1);
	EXPECT_FALSE(parked) << "no awaiter handed this frame away";
	EXPECT_EQ(record.resumed_on, std::this_thread::get_id());
}

TEST_F(WhenAnyTest, ACancelledRaceReportsOperationCanceled)
{
	alt::task_completion_source<int> first;
	alt::task_completion_source<int> second;
	wait_record                      record;
	std::size_t                      winner = 99;
	int                              value  = 0;

	auto handle = alt::start(two_source_root(first, second, record, winner, value));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1);

	// Both children are parked, so no child has completed and no position has been
	// elected when the stop lands. The children are then resumed released and each
	// one does reach its election, so a winner is elected on this path too -- with a
	// released value in its slot. The verdict must not be built from it.
	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(winner, 99U) << "a cancelled race reports no winner";
	EXPECT_EQ(value, 0);
	EXPECT_EQ(live_frames, 0);
}

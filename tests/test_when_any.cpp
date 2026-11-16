#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/when_any_result.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/task_completion_source.hpp>
#include <alt/when_any.hpp>
#include <array>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

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
	 * Most tests here drive the awaiter through this class rather than through
	 * alt::when_any, so that nothing but the awaiter is between the test and the
	 * behavior it pins; the public overloads' own tests are at the end of the file.
	 * It is the same three type aliases and the same rvalue-only operator co_await
	 * detail::when_any_awaitable has, with no overload resolution and no constraints.
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

	/**
	 * A tracked task that parks on @p gate and counts its own resumption past the wait.
	 *
	 * The counter is the observable for the claim when_any_child and
	 * when_any_awaiter::await_resume both rest on: a child already PARKED on an alt
	 * wait primitive is resumed *released* rather than torn down, so its body runs on
	 * past the co_await even on the cancelled path. The increment is the first
	 * statement after the wait, so it cannot run for a child that was torn down at its
	 * suspension point instead.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both objects are owned by the calling test, which holds them until live_frames is back to zero.
	alt::task<alt::wait_result> wait_then_count(alt::async_event& gate, std::atomic<int>& resumed, int tag)
	{
		const frame_tracker    tracker{tag};
		const alt::wait_result result = co_await gate;
		resumed.fetch_add(1, std::memory_order_acq_rel);
		co_return result;
	}

	/** Races two counting children and records how the wait ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task counting_race_root(alt::async_event& first, alt::async_event& second, std::atomic<int>& resumed, wait_record& record, std::size_t& winner)
	{
		const auto result = co_await make_race(wait_then_count(first, resumed, 1), wait_then_count(second, resumed, 2));
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
		}

		// Somewhere for a cancelled chain to tear its frames down.
		co_await alt::cancellation_point{};
	}

	/**
	 * A tracked task parked on a handle only the test can resume, deaf to every stop.
	 *
	 * alt_test::park registers no stop callback, and detail::cancellation_checked only
	 * checks on the way INTO a suspension point, so nothing anywhere in this chain can
	 * resume or tear down this frame once it is parked -- not the winner's
	 * request_children_stop(), not the chain's own cancellation. That is what lets a
	 * race sit with its election already settled and a child still outstanding, which
	 * is the shape used here to reach a cancellation that discards a winner. It is not
	 * the only shape that reaches it -- a stop landing mid-fan-out after an early
	 * election is another, and when_any_awaiter::await_resume lists both -- only the one
	 * a single-threaded test can hold still. The test must resume this frame by hand; it
	 * holds a reference to the shared block.
	 */
	alt::task<void> tracked_park(int tag)
	{
		const frame_tracker tracker{tag};
		co_await park{};
	}

	/** Awaits an already-set source, counts past the wait, and yields the value. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both objects are owned by the calling test, which holds them until live_frames is back to zero.
	alt::task<int> count_past_set_source(alt::task_completion_source<int>& source, std::atomic<int>& completed)
	{
		const auto result = co_await source;
		completed.fetch_add(1, std::memory_order_acq_rel);
		co_return result.value();
	}

	/** Races a child nothing can resume against one that completes and wins at once. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task elected_then_cancelled_root(alt::task_completion_source<int>& ready, std::atomic<int>& completed, wait_record& record, std::size_t& winner, int& frames_at_return)
	{
		const auto result = co_await make_race(tracked_park(1), count_past_set_source(ready, completed));

		frames_at_return = live_frames.load(std::memory_order_acquire);
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
		}

		co_await alt::cancellation_point{};
	}

	/**
	 * @brief Checks the state a discarded winner needs: elected, with a child still
	 *        outstanding.
	 *
	 * Split out of ACancellationDiscardsAWinnerAlreadyElected's body, with the two
	 * below, for the reason test_when_all.cpp's run_one_stop_race records: one function
	 * holding the whole scenario and all of its assertions is over clang-tidy's
	 * readability-function-cognitive-complexity threshold. Measured at 37 against a
	 * threshold of 25 before the split.
	 */
	void expect_a_winner_with_a_child_outstanding(const std::atomic<int>& completed, const wait_record& record)
	{
		EXPECT_EQ(completed.load(std::memory_order_acquire), 1) << "the winning child's body did not run to its end";
		EXPECT_EQ(record.resumptions, 0) << "the await returned before the race was over";
		EXPECT_EQ(live_frames, 1) << "the outstanding child's frame should still be parked";
	}

	/**
	 * @brief Checks that the cancelled verdict came back rather than the elected winner.
	 *
	 * @p frames_at_return is live_frames as read at the instant the await returned: a
	 * child was still outstanding then, which is what makes this a discard rather than
	 * a race that had already finished. Nothing asserts on the winner's position --
	 * the awaiting body reads it only on the satisfied branch, so it carries nothing
	 * the satisfied check does not already say.
	 */
	void expect_the_elected_winner_was_discarded(const wait_record& record, const int frames_at_return)
	{
		EXPECT_EQ(record.resumptions, 1);
		EXPECT_FALSE(record.satisfied) << "a cancelled race returned a winner it should have discarded";
		EXPECT_EQ(record.error, released_error);
		EXPECT_EQ(frames_at_return, 1);
	}

	/**
	 * @brief Resumes the one child nothing in the library can, so no failure leaks it.
	 *
	 * Guarded rather than asserted, and in every test that calls this the checks ahead
	 * of the call are EXPECTs for the same reason: an ASSERT returns from the test,
	 * which would skip this and leak both the parked frame and the shared block's last
	 * reference. That holds for all four callers -- the cancellation test below and the
	 * three tests over a child held past the election -- and is stated here rather than
	 * once per test. WhenAllTest.AStopDuringTheFanOutStopsTheChildrenStartedAfterIt
	 * guards the same cleanup the same way.
	 */
	void release_the_parked_child()
	{
		EXPECT_TRUE(parked) << "the outstanding child never reached its awaitable";

		if(parked)
		{
			resume_parked();
		}
	}

	/** A tracked task that parks on @p gate and then completes by throwing. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<int> wait_then_throw(alt::async_event& gate, int tag)
	{
		const frame_tracker tracker{tag};
		static_cast<void>(co_await gate);

		throw std::runtime_error{"winner body"};
	}

	/**
	 * @brief An awaitable parked on a handle only the test can resume, which then yields
	 *        @p value.
	 *
	 * alt_test::park with a result, and awaited by when_any_child DIRECTLY rather than
	 * through a nested alt::task. **That is load-bearing rather than a simplification.**
	 * alt::task's final_awaiter abandons the chain when a stop has been requested, so a
	 * child whose awaitable is a task never hands its value back to when_any_child once a
	 * winner has stopped the children: the chain is destroyed at the task's final suspend,
	 * ahead of the slot write and the election. A loser built that way has no result to
	 * discard and a test built on one witnesses nothing, which is measured -- see the two
	 * tests below and the mutation recorded with them. A direct awaitable has no such
	 * step: detail::cancellation_checked checks only on the way INTO a suspension point,
	 * so await_resume below runs on the stopped token and when_any_child's slot write and
	 * election follow it as straight-line code.
	 *
	 * Parked for tracked_park's reason, which its note gives: nothing in the library can
	 * resume this, so a race can sit with its election settled and this child still
	 * outstanding. @p resumed is raised in await_resume, which is the last step a test can
	 * observe before the slot write; the step from there to elect() is the straight-line
	 * code in when_any_child. The mutation recorded with the two tests is what turns that
	 * inference into a witness.
	 */
	struct park_then_yield_value
	{
		/** What awaiting this yields, and so what goes into the loser's slot. */
		int value;

		/** Raised by await_resume. Owned by the calling test. */
		std::atomic<int>* resumed;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
		}

		[[nodiscard]] int await_resume() const noexcept
		{
			resumed->fetch_add(1, std::memory_order_acq_rel);
			return value;
		}
	};

	/** park_then_yield_value's counterpart, which completes by throwing rather than with a value. */
	struct park_then_throw_value
	{
		/** Raised by await_resume before it throws. Owned by the calling test. */
		std::atomic<int>* resumed;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
		}

		/** Declared int so this position's slot holds an int, as the other loser's does. */
		[[nodiscard]] int await_resume() const
		{
			resumed->fetch_add(1, std::memory_order_acq_rel);

			throw std::runtime_error{"loser body"};
		}
	};

	/**
	 * @brief Races a throwing child against one held past the election, and records the
	 *        rethrow.
	 *
	 * Position 0 parks on @p gate and throws when it is set, so it stores an exception
	 * and wins its election with position 1 still outstanding; position 1 is parked on
	 * an awaitable no stop can resume, so only the test can retire it. @p returned is
	 * what says the await had not come back in between, and @p frames_at_throw is
	 * live_frames read at the instant the exception arrived -- the only place that
	 * instant is observable, and a body-scoped tracker, so zero there says every child
	 * BODY had run to its end before the rethrow.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task throwing_winner_root(alt::async_event& gate, std::string& caught, bool& returned, int& frames_at_throw)
	{
		try
		{
			[[maybe_unused]] const auto result = co_await make_race(wait_then_throw(gate, 1), tracked_park(2));

			frames_at_throw = live_frames.load(std::memory_order_acquire);
			ADD_FAILURE() << "the winning child's exception did not reach the await";
		}
		catch(const std::runtime_error& error)
		{
			frames_at_throw = live_frames.load(std::memory_order_acquire);
			caught          = error.what();
		}

		returned = true;
	}

	/** Races a child held past the election against one that completes and wins at once. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them until the race is over.
	alt::cancellable_task completed_loser_root(std::atomic<int>& resumed, wait_record& record, std::size_t& winner, int& value)
	{
		const auto result = co_await make_race(park_then_yield_value{.value = 10, .resumed = &resumed}, make_value_task(20));
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();

			if(winner == 1)
			{
				value = result->template get<1>();
			}
		}
	}

	/** completed_loser_root with a loser that throws rather than completing with a value. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them until the race is over.
	alt::cancellable_task throwing_loser_root(std::atomic<int>& resumed, wait_record& record, std::size_t& winner, int& value, std::string& caught)
	{
		try
		{
			const auto result = co_await make_race(park_then_throw_value{.resumed = &resumed}, make_value_task(20));
			record_outcome(record, result);

			if(result)
			{
				winner = result->index();

				if(winner == 1)
				{
					value = result->template get<1>();
				}
			}
		}
		catch(const std::runtime_error& error)
		{
			caught = error.what();
		}
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
	alt::async_event first;
	alt::async_event second;
	std::atomic<int> resumed{0};
	wait_record      record;
	std::size_t      winner = 99;

	auto handle = alt::start(counting_race_root(first, second, resumed, record, winner));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2) << "both children are parked on their own event";
	ASSERT_EQ(resumed.load(std::memory_order_acquire), 0);

	handle.request_stop();

	// No assertion on `winner`, for the reason
	// ACancellationDiscardsAWinnerAlreadyElected gives: the awaiting body writes it only
	// on the satisfied branch, so with EXPECT_FALSE(record.satisfied) above it cannot
	// fail independently.
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 0);

	// The one half of "a cancelled race still elects a winner" that a test can
	// observe, and the load-bearing half: both children were resumed RELEASED rather
	// than torn down, and both ran the statement after their co_await. From there to
	// elect() is straight-line code in when_any_child -- store, then elect, with no
	// suspension point between -- so both children reached their election.
	//
	// What this test CANNOT show, and does not claim to: that an elected winner is
	// discarded by await_resume's cancelled() test. On this path the elections happen
	// AFTER the await has returned. The chain's token is walked in reverse
	// registration order, so the joiner's own waiter_stop_release -- registered at the
	// park, after the block's m_link -- resumes the chain first, await_resume takes its
	// verdict from !waited, and only then does m_link fire cancel() and let these two
	// children run on. So winner() still reads no_winner where the flag is read, which
	// is exactly why the measurement in await_resume's note is a zero. See that note.
	EXPECT_EQ(resumed.load(std::memory_order_acquire), 2) << "a stopped child that was parked is resumed released and runs on";
}

TEST_F(WhenAnyTest, ACancellationDiscardsAWinnerAlreadyElected)
{
	alt::task_completion_source<int> ready;
	std::atomic<int>                 completed{0};
	wait_record                      record;
	std::size_t                      winner           = 99;
	int                              frames_at_return = -1;

	// Set before the race starts, so position 1 completes inside its own start.
	ready.set_value(4);

	auto handle = alt::start(elected_then_cancelled_root(ready, completed, record, winner, frames_at_return));

	// The state this test needs, and the reason it is reachable at all. Position 1 ran
	// past its wait and to the end of its body, so it wrote its slot and reached
	// elect(), which it won unopposed. Position 0 is parked on an awaitable that
	// registers no stop callback, so the winner's request_children_stop() cannot resume
	// it: it is still outstanding, the counter has not drained, and the chain is parked
	// with the election already settled.
	//
	// The election being settled is INFERRED, not read -- nothing exposes the winner
	// before the await returns. What is observed is the winning child's body running to
	// its end, and store-then-elect is straight-line code in when_any_child with no
	// suspension point between. What makes this test a witness rather than an
	// arrangement is the mutation recorded in await_resume's note: it is the only test
	// in the suite that fails when an elected winner is allowed to outrank the verdict.
	expect_a_winner_with_a_child_outstanding(completed, record);

	handle.request_stop();

	// Decision 2's promise, and the only place in the suite that holds it: a winner WAS
	// elected, and the cancelled await reports the cancellation rather than returning
	// it. ACancelledRaceReportsOperationCanceled cannot show this -- on its path every
	// election happens after the await has returned, so it cannot tell "no winner" from
	// "winner discarded".
	//
	// Which half of await_resume's verdict this pins: the FIRST one. The joiner's
	// waiter_stop_release is registered after the block's m_link and libstdc++ walks a
	// token's callbacks in reverse, so the joiner's release resumes the chain before
	// cancel() has run and await_resume reads the flag as false. The flag half has no
	// test at all; see that note for the two routes to it.
	expect_the_elected_winner_was_discarded(record, frames_at_return);

	// Nothing else can release position 0, and it holds the block's last reference.
	release_the_parked_child();

	// Its body ran on, lost its own election and retired, and its hook released that
	// last reference -- with no awaiter and no chain left, which is the half of the
	// lifetime argument only the sanitizers can check. Nothing is asserted here about a
	// second resumption of the chain: its frame was destroyed at the cancellation
	// point, so a second resumption would resume freed storage rather than increment a
	// counter, and this test could not report it either way.
	EXPECT_EQ(live_frames, 0);
}

// Measured: give when_any_child's catch clause a co_return, so a child that completed by
// throwing stores its exception and then skips its election. This is the only test in the
// 798-test suite that fails under that, and it fails in each of debug, AddressSanitizer,
// ThreadSanitizer and release-tests -- the exception never reaches the await, the parked
// sibling wins instead, and the caught message stays empty.
TEST_F(WhenAnyTest, AThrowingFirstChildWinsAndRethrowsAfterTheLosersRetire)
{
	alt::async_event gate;
	std::string      caught;
	bool             returned        = false;
	int              frames_at_throw = -1;

	// Every check ahead of release_the_parked_child is an EXPECT; see its note.
	const auto handle = alt::start(throwing_winner_root(gate, caught, returned, frames_at_throw));
	EXPECT_EQ(live_frames, 2) << "both children are parked on their own awaitable";
	EXPECT_FALSE(returned);

	// Position 0 resumes, throws, and when_any_child catches it into the slot -- a child
	// that completes by throwing still elects, so this wins. Its win stops the children,
	// which cannot reach position 1: that child is parked on an awaitable registering no
	// stop callback, so the counter still holds it.
	gate.set();

	// The half of decision 5 that only an outstanding loser can show. A winner is
	// elected and its exception is sitting in its slot, and the await has NOT come back
	// with it: the rethrow waits on the counter like a value would. An await that
	// rethrew on the election instead would have returned here, with a sibling body
	// still to run.
	EXPECT_FALSE(returned) << "the exception reached the await while a loser was still running";
	EXPECT_TRUE(caught.empty());
	EXPECT_EQ(live_frames, 1) << "the loser's frame should still be parked";

	release_the_parked_child();

	// The loser ran on and retired, which drained the counter and resumed the chain; only
	// then did assemble_when_any_result read the winning slot and result_store::value()
	// rethrow out of it. Nothing here claims the loser reached an ELECTION: it is a
	// nested alt::task, whose final_awaiter abandons the chain on a requested stop, so it
	// is destroyed ahead of its own slot write. See park_then_yield_value, where that
	// matters, and WhenAnyTest.ACompletedLosersResultIsDiscarded, which needs it not to
	// happen. All this test needs of position 1 is that it RETIRES.
	EXPECT_TRUE(returned) << "the exception never reached the await";
	EXPECT_EQ(caught, "winner body");

	// Read at the instant the exception arrived, and the tracker is a body-scoped local,
	// so zero says both child bodies had run to their end before the rethrow rather than
	// merely that their frames were freed -- the property that makes the rethrow and the
	// teardown ordered rather than coincidentally so.
	EXPECT_EQ(frames_at_throw, 0) << "a child body was still running when the winner's exception was rethrown";
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(handle.stop_requested()) << "the chain itself was never cancelled";
}

// The two modes below are measured together, by one mutation: an UNCONDITIONAL ELECTION,
// combinator_election::elect storing the position and returning true for every caller, so
// the LAST completer takes the race. Seven tests in the 798-test suite fail under it in
// each of debug, AddressSanitizer, ThreadSanitizer and release-tests, these two among
// them -- the first on the winner's index and value, the second with "a loser's exception
// reached the await: loser body", which is the mode it exists for.
//
// And the measurement that says these two witness a loser's slot write at all, rather
// than arranging one: with each loser built as a nested alt::task parked on alt_test::park
// instead of as the direct awaitable it is, BOTH TESTS PASS under that same mutation, in
// every preset. The task's final_awaiter abandons the chain once a stop has been
// requested, so such a loser is destroyed before it writes its slot and before it
// elects -- it has no result to discard. See park_then_yield_value.
TEST_F(WhenAnyTest, ACompletedLosersResultIsDiscarded)
{
	std::atomic<int> resumed{0};
	wait_record      record;
	std::size_t      winner = 99;
	int              value  = 0;

	// Every check ahead of release_the_parked_child is an EXPECT; see its note.
	const auto handle = alt::start(completed_loser_root(resumed, record, winner, value));

	// Position 1 completed inside its own start and won unopposed; position 0 is parked
	// on an awaitable registering no stop callback, so the winner's
	// request_children_stop() cannot reach it. It is outstanding, the counter has not
	// drained, and the chain is parked with the election already settled.
	EXPECT_EQ(record.resumptions, 0) << "the await returned before the race was over";
	EXPECT_EQ(resumed.load(std::memory_order_acquire), 0);

	release_the_parked_child();

	// Decision 6's first mode. The loser's awaitable yielded a value of its own AFTER the
	// election was settled, so when_any_child wrote that value into position 0's slot and
	// then lost the election with it -- the counter resumed the chain only once it had.
	// What comes back is the winner's position and the winner's value; nothing anywhere
	// exposes the loser's 10.
	EXPECT_EQ(resumed.load(std::memory_order_acquire), 1) << "the losing child's awaitable never yielded its value";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U) << "the first child to complete is the winner, whatever a loser completes with afterwards";
	EXPECT_EQ(value, 20);
	EXPECT_FALSE(handle.stop_requested()) << "the chain itself was never cancelled";
}

TEST_F(WhenAnyTest, ALosersExceptionIsDiscarded)
{
	std::atomic<int> resumed{0};
	wait_record      record;
	std::size_t      winner = 99;
	int              value  = 0;
	std::string      caught;

	// Every check ahead of release_the_parked_child is an EXPECT; see its note.
	const auto handle = alt::start(throwing_loser_root(resumed, record, winner, value, caught));

	EXPECT_EQ(record.resumptions, 0) << "the await returned before the race was over";
	EXPECT_EQ(resumed.load(std::memory_order_acquire), 0);

	release_the_parked_child();

	// Decision 6's second mode, and the asymmetry a caller coming from alt::when_all is
	// likeliest to be surprised by: a when_all rethrows ANY child's exception, and a
	// when_any rethrows only the WINNER's. This loser threw after the election was
	// settled, so when_any_child's catch put the exception in position 0's slot and then
	// lost the election with it. Nothing reads that slot, the awaiter reading the elected
	// position and nothing else, so the winner's value comes back and nothing is
	// rethrown.
	EXPECT_EQ(resumed.load(std::memory_order_acquire), 1) << "the losing child's awaitable never reached its throw";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U);
	EXPECT_EQ(value, 20);
	EXPECT_TRUE(caught.empty()) << "a loser's exception reached the await: " << caught;
	EXPECT_FALSE(handle.stop_requested()) << "the chain itself was never cancelled";
}

// The public overloads. Each test above this line drives the awaiter through the
// race class; these go through alt::when_any itself.

namespace
{
	/** How many times the concurrent-completion test repeats its race. */
	constexpr int concurrent_race_repeats = 100;

	/** The value each position's child yields in the two range roots below. One table, read by both and by the checks over them. */
	constexpr std::array<int, 2> race_child_values{10, 20};

	/** A move-only awaitable, ready at once, that records when its contents were moved away. */
	struct flagged_ready
	{
		explicit flagged_ready(const int v): value(v)
		{
		}

		flagged_ready(flagged_ready&& other) noexcept: value(other.value)
		{
			other.moved_from = true;
		}

		flagged_ready(const flagged_ready&)            = delete;
		flagged_ready& operator=(const flagged_ready&) = delete;
		flagged_ready& operator=(flagged_ready&&)      = delete;
		~flagged_ready()                               = default;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{
		}

		[[nodiscard]] int await_resume() const noexcept
		{
			return value;
		}

		int  value;
		bool moved_from = false;
	};

	/** A copyable awaitable that is ready at once with @p value. */
	struct copyable_ready
	{
		int value;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{
		}

		[[nodiscard]] int await_resume() const noexcept
		{
			return value;
		}
	};

	/**
	 * Both an awaitable and a sized range, and a range OF awaitables, so that the range
	 * overload's other constraints are all satisfied and only !awaitable<R> keeps it out.
	 * A range of ints would be excluded by its element type whatever that clause said.
	 */
	struct awaitable_range: copyable_ready
	{
		[[nodiscard]] const copyable_ready* begin() const noexcept
		{
			return nullptr;
		}

		[[nodiscard]] const copyable_ready* end() const noexcept
		{
			return nullptr;
		}

		[[nodiscard]] std::size_t size() const noexcept
		{
			return 0;
		}
	};

	/** A task that parks on @p gate and then yields @p value. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<int> wait_then_yield(alt::async_event& gate, int value)
	{
		const frame_tracker tracker{value};
		static_cast<void>(co_await gate);
		co_return value;
	}

	/** A task that parks on @p gate and then yields a pointer to @p value: a move-only winner. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<std::unique_ptr<int>> wait_then_yield_unique(alt::async_event& gate, int value)
	{
		const frame_tracker tracker{value};
		static_cast<void>(co_await gate);
		co_return std::make_unique<int>(value);
	}

	/** Parks in a when_any over a vector of two tasks and records the winner. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task range_race_root(alt::async_event& first, alt::async_event& second, wait_record& record, std::size_t& winner, int& value)
	{
		std::vector<alt::task<int>> children;
		children.push_back(wait_then_yield(first, race_child_values[0]));
		children.push_back(wait_then_yield(second, race_child_values[1]));

		const auto result = co_await alt::when_any(std::move(children));
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
			value  = result->value();
		}
	}

	/** What one repeat of the concurrent race counts, in place of the frame tracker. */
	struct concurrent_counts
	{
		/** Children that have reached their wait. Two is a race with both children parked. */
		std::atomic<int> started{0};

		/** Children whose body has run past its wait to its end. */
		std::atomic<int> completed{0};

		/** @c completed as read at the instant the await returned. Two is no child left running. */
		int completed_at_return = -1;
	};

	/**
	 * @brief A child of the concurrent race: counts itself in before its wait and out
	 *        after it.
	 *
	 * No frame_tracker, deliberately, and the reason is the harness's rather than this
	 * race's: ~frame_tracker pushes onto alt_test::destruction_order, a plain std::vector
	 * its own note reserves for single-threaded tests, and this is the only race in this
	 * file whose two children are resumed -- and so destroyed -- on two threads at once.
	 * The counts above say what live_frames would have said, and across threads, which
	 * live_frames alone could not: it is atomic, but the vector beside it is not.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both references are owned by the calling test, which joins both racing threads before leaving their scope.
	alt::task<int> count_around_wait(alt::async_event& gate, concurrent_counts& counts, int value)
	{
		counts.started.fetch_add(1, std::memory_order_acq_rel);
		static_cast<void>(co_await gate);
		counts.completed.fetch_add(1, std::memory_order_acq_rel);
		co_return value;
	}

	/** Races two children that two other threads satisfy at once, over the same table of values. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which joins both racing threads before leaving their scope.
	alt::cancellable_task concurrent_race_root(alt::async_event& first, alt::async_event& second, concurrent_counts& counts, wait_record& record, std::size_t& winner, int& value)
	{
		std::vector<alt::task<int>> children;
		children.push_back(count_around_wait(first, counts, race_child_values[0]));
		children.push_back(count_around_wait(second, counts, race_child_values[1]));

		const auto result = co_await alt::when_any(std::move(children));

		// Read before anything else, because this instant is the only place the property
		// is observable: by the time either racing thread is joined, both children's
		// bodies have finished either way.
		counts.completed_at_return = counts.completed.load(std::memory_order_acquire);
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
			value  = result->value();
		}
	}

	/**
	 * @brief Checks one repeat of the concurrent race and records which position won it.
	 *
	 * Split out of TwoChildrenCompletingConcurrentlyYieldExactlyOneWinner's loop body for
	 * the reason expect_a_winner_with_a_child_outstanding above is split out of its own
	 * test's: clang-tidy's readability-function-cognitive-complexity counts a loop's body
	 * against the function holding it, and a suppression is the wrong answer when a name
	 * will do.
	 *
	 * Nothing here says WHICH position won, and nothing anywhere in that test asserts a
	 * distribution over the two. Which one wins a contended race is a property of the
	 * build rather than of the library -- test_when_all.cpp's satisfier_handshake note
	 * measures how far that goes, the same loop scoring 199.6 of 200 one way in debug and
	 * 3.5 of 200 under AddressSanitizer -- so a lower bound on either position's share is
	 * the instrument that produced this project's one flaky test. What is asserted is an
	 * equality per repeat and a total over the run.
	 */
	void tally_the_one_winner(const int repeat, const wait_record& record, const concurrent_counts& counts, const std::size_t winner, const int value, std::array<int, 2>& wins)
	{
		EXPECT_EQ(record.resumptions, 1) << "repeat " << repeat << ": the chain was not resumed exactly once";
		EXPECT_TRUE(record.satisfied) << "repeat " << repeat << ": a race both of whose children completed yielded no winner";

		// The component's central guarantee, on the contended path: the await waits on the
		// counter rather than on the election, so both bodies had run to their end by the
		// instant it returned. An await that returned on the election would read one here.
		EXPECT_EQ(counts.completed_at_return, 2) << "repeat " << repeat << ": a child was still running when the won await returned";

		// Fatal, so a result whose index is no position of the race does not index the
		// tally below with it. The run's total then fails as well, which is the intended
		// pair of failures rather than a second fault.
		ASSERT_LT(winner, wins.size()) << "repeat " << repeat << ": the result's index is not a position of the race";

		++wins.at(winner);

		// The index and the value agree, which is what says ONE child's result came back
		// whole: a winner's position beside a loser's value would pass every check above
		// and fail this one.
		EXPECT_EQ(value, race_child_values.at(winner)) << "repeat " << repeat << ": the winner's index and value disagree";
	}

	/**
	 * @brief Runs one repeat of the concurrent race, from the fan-out to the tally.
	 *
	 * The whole repeat rather than only its checks, for the reason above: clang-tidy
	 * counts a loop's body against the function holding it, and with the setup and the
	 * two preconditions inline the test's own body measured 33 against a threshold of 25.
	 * test_when_all.cpp's run_one_stop_race is split out the same way for the same
	 * reading.
	 */
	void run_one_concurrent_race(const int repeat, std::array<int, 2>& wins)
	{
		reset_coroutine_test_state();

		// The record and the counts outlive the events deliberately, and the order is not
		// tidying: an ASSERT below returns from here with the children still parked, and
		// ~async_event then releases them, so both children and the chain run on after the
		// events are gone and must still have somewhere to write.
		wait_record              record;
		concurrent_counts        counts;
		std::size_t              winner = 99;
		int                      value  = 0;
		alt::async_event         first;
		alt::async_event         second;
		std::atomic<std::size_t> arrived{0};

		const auto handle = alt::start(concurrent_race_root(first, second, counts, record, winner, value));
		ASSERT_EQ(record.resumptions, 0) << "repeat " << repeat;
		ASSERT_EQ(counts.started.load(std::memory_order_acquire), 2) << "repeat " << repeat << ": both children should be parked on their own event";

		// Both events are set within nanoseconds of each other, on two threads neither of
		// which is this one, so neither child's completion is ordered behind the other's:
		// both are in flight with nothing between them. Whether they collide on a given
		// repeat is timing and no assertion here witnesses it, which is the same claim
		// test_when_all.cpp's free-running phase makes for itself.
		//
		// A child whose event is set after the winner has already stopped the children is
		// resumed RELEASED rather than torn down, so its body runs on and raises completed
		// before its co_return; whether it then reaches its own slot and election depends
		// on when the stop landed, alt::task's final_awaiter abandoning the chain once one
		// has been requested. Neither outcome disturbs what is checked: the winner writes
		// its own slot, so the index and the value agree however the race landed, and
		// completed reaches two either way because the increment precedes the co_return.
		{
			std::jthread first_completer{[&] {
				rendezvous(arrived, 2);
				first.set();
			}};

			std::jthread second_completer{[&] {
				rendezvous(arrived, 2);
				second.set();
			}};
		}

		tally_the_one_winner(repeat, record, counts, winner, value, wins);
	}

	/** Parks in an argument-list alt::when_any over a value task and a move-only one, and takes the winner. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task variadic_race_root(alt::async_event& first, alt::async_event& second, wait_record& record, std::size_t& winner, std::unique_ptr<int>& taken)
	{
		auto result = co_await alt::when_any(wait_then_yield(first, 10), wait_then_yield_unique(second, 20));
		record_outcome(record, result);

		if(result)
		{
			winner = result->index();
			taken  = std::move(*result).template get<1>();
		}
	}

	/** Races an empty vector of tasks, and records how the await ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the record is owned by the calling test, which holds it until live_frames is back to zero.
	alt::cancellable_task empty_range_root(wait_record& record)
	{
		const auto result = co_await alt::when_any(std::vector<alt::task<int>>{});
		record_outcome(record, result);
	}
}

// The compile-time rules, in the register test_when_all.cpp uses for its own.
template<typename... Args>
concept when_any_accepts = requires(Args&&... arguments) { alt::when_any(std::forward<Args>(arguments)...); };

// An argument list needs at least one awaitable. This is the constraint's doing and
// not the result machinery's: without it the call is well formed and the error only
// appears when something is awaited.
static_assert(!when_any_accepts<>);
static_assert(when_any_accepts<alt::task<int>>);
static_assert(when_any_accepts<alt::task<int>, alt::task<void>>);

// A const range of move-only elements cannot be moved from, and is rejected up front.
static_assert(!when_any_accepts<const std::vector<alt::task<int>>&>);
static_assert(when_any_accepts<std::vector<alt::task<int>>&>);
static_assert(when_any_accepts<std::vector<alt::task<int>>>);
static_assert(when_any_accepts<const std::vector<copyable_ready>&>);

// A range whose elements are not awaitable is not raced.
static_assert(!when_any_accepts<std::vector<int>>);

// A type that is both an awaitable and a sized range is one child of the argument-list form.
static_assert(std::same_as<alt::await_result_t<decltype(alt::when_any(std::declval<awaitable_range>()))>,
                           std::expected<alt::when_any_result<int>, std::error_code>>);

// The range form's own result class, with the one value type every element shares.
static_assert(std::same_as<alt::await_result_t<decltype(alt::when_any(std::declval<std::vector<alt::task<int>>>()))>,
                           std::expected<alt::when_any_range_result<int>, std::error_code>>);
static_assert(std::same_as<alt::await_result_t<decltype(alt::when_any(std::declval<std::vector<alt::task<void>>>()))>,
                           std::expected<alt::when_any_range_result<std::monostate>, std::error_code>>);

TEST_F(WhenAnyTest, TheRangeFormRacesEveryElementAndYieldsTheWinnersIndex)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;
	std::size_t      winner = 99;
	int              value  = 0;

	auto handle = alt::start(range_race_root(first, second, record, winner, value));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2);

	second.set();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U) << "the second element completed first, so its position is the winner";
	EXPECT_EQ(value, race_child_values[1]);
	EXPECT_EQ(live_frames, 0) << "the loser must have retired before a won await returns";
	EXPECT_FALSE(handle.stop_requested());
}

// WHAT THIS TEST CATCHES, measured rather than asserted, because the honest answer is a
// narrow one and a comment nobody can trust is worse than a number.
//
// It catches nothing that another test does not catch first. Three mutations were run
// against it, each in debug, under AddressSanitizer, under ThreadSanitizer and in the
// release-tests preset:
//
// - A NON-ATOMIC ELECTION -- combinator_election::elect reading m_winner and then storing
//   it, which is exactly the "a load followed by a store would let both see no_winner"
//   that class's note warns of -- is caught in all four presets by
//   CombinatorElectionTest.ConcurrentElectionsYieldExactlyOneWinner and by nothing else.
//   This test passes under it: one run in each of the four presets, plus 20 repeats of
//   the whole WhenAnyTest suite in each of the three debug presets, which is 2,000
//   contended races apiece. The reason is worth keeping:
//   every child writes its OWN slot, so a second winner changes only which position the
//   last store names, and that position's value still agrees with it. So "exactly one
//   winner" in the name is exactly one RESUMPTION with one coherent result, and the
//   election's own exactly-one-winner is pinned by that test and not by this one.
// - AN UNCONDITIONAL ELECTION (every caller wins, last completer takes it) is caught by
//   seven tests in all four presets, the two loser tests above among them. This one
//   passes under it, for the same reason.
// - SWAPPING complete() AND release() in on_when_any_child_complete aborts
//   ACancelledRaceReportsOperationCanceled on waiter_list's liveness assert in debug and
//   reports heap-use-after-free under both sanitizers, and passes the whole suite in
//   release-tests. Run alone under it, this test is green in all four presets over five
//   repeats.
//
// What it does assert, and what no other test in this file does: that two children
// completing at once yield one resumption, an index and a value that agree, and both
// bodies finished -- rather than a double resume, a torn result or an early return. It
// observes the contended outcome; it pins no ordering.
TEST_F(WhenAnyTest, TwoChildrenCompletingConcurrentlyYieldExactlyOneWinner)
{
	std::array<int, 2> wins{};

	for(int repeat = 0; repeat < concurrent_race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_concurrent_race(repeat, wins));
	}

	// The equality the whole run comes down to: every repeat elected exactly one winner,
	// and it was one of the two positions. A repeat that elected none leaves its position
	// untallied, and one that somehow reported a third leaves the ASSERT above to say so.
	EXPECT_EQ(wins[0] + wins[1], concurrent_race_repeats);
}

TEST_F(WhenAnyTest, TheArgumentListFormRacesItsArgumentsAndYieldsAMoveOnlyWinner)
{
	alt::async_event     first;
	alt::async_event     second;
	wait_record          record;
	std::size_t          winner = 99;
	std::unique_ptr<int> taken;

	auto handle = alt::start(variadic_race_root(first, second, record, winner, taken));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2);

	second.set();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(winner, 1U);
	ASSERT_NE(taken, nullptr) << "the move-only winner did not come out of the result";
	EXPECT_EQ(*taken, 20);
	EXPECT_EQ(live_frames, 0) << "the loser must have retired before a won await returns";
}

TEST_F(WhenAnyTest, AnEmptyRangeReportsInvalidArgument)
{
	wait_record record;

	const auto handle = alt::start(empty_range_root(record));

	// Resumed inside start(): nothing was parked, so nothing is left to wait for.
	EXPECT_EQ(record.resumptions, 1) << "an empty range must not suspend the chain";
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, std::make_error_code(std::errc::invalid_argument));

	const auto result = alt::sync_wait(alt::when_any(std::vector<alt::task<int>>{}));

	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error(), std::errc::invalid_argument);
}

TEST_F(WhenAnyTest, AnLvalueRangeIsBoundAndLeftMovedFrom)
{
	std::vector<flagged_ready> children;
	children.emplace_back(3);
	children.emplace_back(4);

	const auto result = alt::sync_wait(alt::when_any(children));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->index(), 0U) << "both are ready; the first started is the first elected";
	EXPECT_EQ(result->value(), 3);
	EXPECT_EQ(children.size(), 2U) << "the range itself is bound, not consumed";
	EXPECT_TRUE(children[0].moved_from);
	EXPECT_TRUE(children[1].moved_from);
}

TEST_F(WhenAnyTest, AConstRangeOfCopyableElementsIsCopied)
{
	const std::vector<copyable_ready> children{{.value = 7}, {.value = 8}};

	const auto result = alt::sync_wait(alt::when_any(children));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->index(), 0U);
	EXPECT_EQ(result->value(), 7);
}

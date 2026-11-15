#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/task_completion_source.hpp>
#include <alt/when_all.hpp>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// What awaiting a when_all yields. The result travels in a std::expected, because
// cancellation is an observable result rather than a torn-down frame; see the header.
using one_value_child = decltype(alt::when_all(std::declval<alt::task<int>&&>()));
using one_void_child  = decltype(alt::when_all(std::declval<alt::task<void>&&>()));
using no_children     = decltype(alt::when_all());

static_assert(alt::awaitable<one_value_child>);
static_assert(std::same_as<alt::await_result_t<one_value_child>, std::expected<std::tuple<int>, std::error_code>>);
static_assert(std::same_as<alt::await_result_t<one_void_child>, std::expected<std::tuple<std::monostate>, std::error_code>>);
static_assert(std::same_as<alt::await_result_t<no_children>, std::expected<std::tuple<>, std::error_code>>);

// An lvalue argument keeps its position's type as a reference, so the child awaits the
// caller's object rather than a copy of it. alt::task is not copyable, which is the point.
using lvalue_child = decltype(alt::when_all(std::declval<alt::async_event&>()));
static_assert(std::same_as<alt::await_result_t<lvalue_child>, std::expected<std::tuple<alt::wait_result>, std::error_code>>);

namespace
{
	/** How many times the contended test repeats its race. */
	constexpr int race_repeats = 100;

	/** The condition every released wait reports. See alt::wait_result. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/** How many times the contextless park test retries for a repeat that actually parked. */
	constexpr int contextless_park_attempts = 100;

	/** Spins until @p flag is set, yielding only as a backstop. See alt_test::rendezvous. */
	void spin_until(const std::atomic<bool>& flag)
	{
		for(int spins = 0; !flag.load(std::memory_order_acquire);)
		{
			if(spins < spin_limit)
			{
				++spins;
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	/** A task that completes synchronously with @p value. */
	template<typename T>
	alt::task<T> make_value_task(T value)
	{
		co_return std::move(value);
	}

	/** A task that completes synchronously with nothing. */
	alt::task<void> make_void_task()
	{
		co_return;
	}

	/** A task that completes synchronously with a reference to the caller's object. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the referenced object is owned by the calling test, which outlives the whole await.
	alt::task<int&> make_reference_task(int& target)
	{
		co_return target;
	}

	/** A task that publishes that its body ran and then completes with @p value. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the flag is owned by the calling test, which joins the thread watching it before leaving its scope.
	alt::task<int> make_signalling_value_task(int value, std::atomic<bool>& ran)
	{
		ran.store(true, std::memory_order_release);
		co_return value;
	}

	/** A task whose body throws instead of producing a value. */
	alt::task<int> make_throwing_task()
	{
		throw std::runtime_error{"child body"};
		co_return 0;
	}

	/** A task whose body raises @p thrown and then throws a runtime_error saying @p message. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the flag is owned by the calling test, which joins the thread watching it before leaving its scope.
	alt::task<int> make_signalling_throwing_task(std::string message, std::atomic<bool>& thrown)
	{
		thrown.store(true, std::memory_order_release);
		throw std::runtime_error{message};
		co_return 0;
	}

	/**
	 * A task that parks on @p source, then raises @p done and yields 7: a sibling that
	 * must outlive another's throw.
	 *
	 * Tracked, so that live_frames reports this frame rather than nothing: it is the
	 * frame whose liveness the sibling test is about, alive while the await is parked
	 * and gone once the await has returned.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both objects are owned by the calling test, which holds them until live_frames is back to zero.
	alt::task<int> make_finishing_task(alt::task_completion_source<int>& source, std::atomic<bool>& done)
	{
		const frame_tracker         tracker{1};
		[[maybe_unused]] const auto result = co_await source;
		done.store(true, std::memory_order_release);
		co_return 7;
	}

	/** A tracked task that parks on @p gate and hands back how its wait ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<alt::wait_result> tracked_wait(alt::async_event& gate, int tag)
	{
		const frame_tracker    tracker{tag};
		const alt::wait_result result = co_await gate;
		co_return result;
	}

	/** Sets an event as it is destroyed, so one child's teardown completes a sibling's wait. */
	struct event_setter
	{
		alt::async_event* target;

		~event_setter()
		{
			target->set();
		}
	};

	/** A tracked task that parks on @p gate and releases @p sibling as it unwinds. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both events are owned by the calling test, which holds them until live_frames is back to zero.
	alt::task<alt::wait_result> wait_then_release_sibling(alt::async_event& gate, alt::async_event& sibling, int tag)
	{
		const frame_tracker    tracker{tag};
		const event_setter     setter{&sibling};
		const alt::wait_result result = co_await gate;
		co_return result;
	}

	/**
	 * An awaitable whose await_suspend throws, standing in for an operation that
	 * fails to start. Without the child's try around the whole co_await this reaches
	 * cancellable_task_promise::unhandled_exception, which terminates the process.
	 */
	struct throwing_suspend
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const
		{
			throw std::runtime_error{"await_suspend"};
		}

		void await_resume() const noexcept
		{}
	};

	/** An awaitable that requests a stop from inside the fan-out and never suspends. */
	struct stop_requesting_ready
	{
		std::stop_source* source;

		[[nodiscard]] bool await_ready() const
		{
			source->request_stop();
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{}

		void await_resume() const noexcept
		{}
	};

	/** An awaitable that parks, counting how often it was reached at all. */
	struct counting_park
	{
		int* suspends;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h) const noexcept
		{
			++*suspends;
			parked = h;
		}

		void await_resume() const noexcept
		{}
	};

	/** What one awaiting chain recorded about its when_all. */
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

	/** Parks in a when_all over two sources and reports both values positionally. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the sources and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task order_root(alt::task_completion_source<int>& first, alt::task_completion_source<int>& second, wait_record& record, std::tuple<int, int>& values)
	{
		const frame_tracker tracker{1};
		const auto          result = co_await alt::when_all(first, second);
		record_outcome(record, result);

		if(result)
		{
			values = {std::get<0>(*result).value(), std::get<1>(*result).value()};
		}

		// Somewhere for a cancelled chain to tear its frames down.
		co_await alt::cancellation_point{};
	}

	/** Parks in a when_all over two sources and records only how the wait ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the sources and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task source_root(alt::task_completion_source<int>& first, alt::task_completion_source<int>& second, wait_record& record)
	{
		const frame_tracker tracker{1};
		const auto          result = co_await alt::when_all(first, second);
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Parks in a when_all whose children each park on an event of their own. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task event_root(alt::async_event& first, alt::async_event& second, wait_record& record)
	{
		const auto result = co_await alt::when_all(tracked_wait(first, 1), tracked_wait(second, 2));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Parks on @p gate and hands back how its wait ended. Untracked. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event is owned by the calling test, which holds it until live_frames is back to zero.
	alt::task<alt::wait_result> wait_on(alt::async_event& gate)
	{
		const alt::wait_result result = co_await gate;
		co_return result;
	}

	/**
	 * A task whose lifetime is observable through live_frames: it owns a tracker, and a
	 * when_all moves it into its child's frame, so the tracker dies exactly when that
	 * child frame does. The only way to see a child frame's teardown from outside, since
	 * the child's own body has long finished by the time the frame is destroyed.
	 */
	class homed_wait
	{
	public:
		homed_wait(alt::async_event& gate, int tag): m_task(wait_on(gate)), m_tracker(std::make_unique<frame_tracker>(tag))
		{
		}

		[[nodiscard]] auto operator co_await() && noexcept
		{
			return std::move(m_task).operator co_await();
		}

	private:
		alt::task<alt::wait_result>    m_task;
		std::unique_ptr<frame_tracker> m_tracker;
	};

	/** Parks in a when_all over two homed_wait children and records how the wait ended. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task home_root(alt::async_event& first, alt::async_event& second, wait_record& record)
	{
		const auto result = co_await alt::when_all(homed_wait{first, 1}, homed_wait{second, 2});
		record_outcome(record, result);
	}

	/** Parks in a when_all whose second child's teardown satisfies the first child. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task sibling_release_root(alt::async_event& first, alt::async_event& second, wait_record& record)
	{
		// Argument order is registration order, and the children's stop walks its
		// callbacks in reverse, so the second child is the one it reaches first.
		const auto result = co_await alt::when_all(tracked_wait(first, 1), wait_then_release_sibling(second, first, 2));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Enters the fan-out in step with a second thread that satisfies one child. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which joins the racing thread and waits for live_frames to reach zero before leaving their scope.
	alt::cancellable_task racing_root(alt::async_event& first, alt::async_event& second, std::atomic<std::size_t>& arrived, wait_record& record)
	{
		// Released together with the thread that sets the first event, so that
		// completion lands within nanoseconds of the fan-out that started it.
		rendezvous(arrived, 2);

		const auto result = co_await alt::when_all(tracked_wait(first, 1), tracked_wait(second, 2));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Requests a stop from inside the first child's start, then starts a second child. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which outlives the whole chain.
	alt::cancellable_task fan_out_stop_root(std::stop_source& external, int& suspends, wait_record& record)
	{
		const auto result = co_await alt::when_all(stop_requesting_ready{&external}, counting_park{&suspends});
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/**
	 * Awaits a throwing child beside one parked on @p source, and records how the await
	 * ended.
	 *
	 * @p frames_at_return takes live_frames as read at the instant the await returned,
	 * which is the only place that instant is observable. The test asserts it is zero,
	 * which says every child BODY ran to its end before the await returned -- the
	 * tracker is a body-scoped local, so it is gone once the body is past it. It does not
	 * say the child frames had been freed by then, and the sibling property this test is
	 * about is the body, not the frame.
	 */
	// The NOLINT below covers this function's whole signature only because
	// .clang-format's ColumnLimit is 0, which keeps the signature on one line. Wrapping
	// it by hand reopens cppcoreguidelines-avoid-reference-coroutine-parameters on every
	// line after the first, and the same trap applies to the dozen other long signatures
	// in this file.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which outlives the whole chain.
	alt::cancellable_task throwing_sibling_root(alt::task_completion_source<int>& source, std::atomic<bool>& finished, std::string& caught, bool& returned, int& frames_at_return)
	{
		try
		{
			[[maybe_unused]] const auto results = co_await alt::when_all(make_throwing_task(), make_finishing_task(source, finished));
			frames_at_return                    = live_frames.load(std::memory_order_acquire);
			ADD_FAILURE() << "the throwing child's exception did not reach the await";
		}
		catch(const std::runtime_error& error)
		{
			frames_at_return = live_frames.load(std::memory_order_acquire);
			caught           = error.what();
		}

		returned = true;
	}

	/**
	 * Awaits a when_all that parks, from a promise whose cancellation context is null.
	 *
	 * Position 0 parks on @p gate, which only the calling test's second thread can set;
	 * position 1 completes inside the fan-out and publishes @p fanned_out as it goes,
	 * which is what releases that thread. So the chain reaches its park with one child
	 * outstanding, and is resumed from the thread that sets the gate.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which blocks in sync_wait until this completes and joins its second thread after that.
	alt::task<void> contextless_probe(alt::task_completion_source<int>& gate, std::atomic<bool>& fanned_out, wait_record& record, std::tuple<int, int>& values)
	{
		const auto result = co_await alt::when_all(gate, make_signalling_value_task(2, fanned_out));
		record_outcome(record, result);

		if(result)
		{
			values = {std::get<0>(*result).value(), std::get<1>(*result)};
		}
	}

	/**
	 * @brief Runs one attempt at a contextless park, asserting what holds either way.
	 *
	 * Split out of the test's loop because one function holding the attempt and its
	 * assertions is over clang-tidy's readability-function-cognitive-complexity
	 * threshold, exactly as run_one_stop_race records.
	 *
	 * @param attempt The attempt index, so a failure names which one.
	 * @return Whether the await actually parked, read off the thread the body ran on.
	 */
	bool run_one_contextless_park(const int attempt)
	{
		reset_coroutine_test_state();

		alt::task_completion_source<int> gate;
		std::atomic<bool>                fanned_out{false};
		wait_record                      record;
		std::tuple<int, int>             values{0, 0};

		std::jthread completer{[&] {
			spin_until(fanned_out);
			gate.set_value(1);
		}};

		alt::sync_wait(contextless_probe(gate, fanned_out, record, values));
		completer.join();

		// True whichever way the race went.
		EXPECT_EQ(record.resumptions, 1) << "attempt " << attempt;
		EXPECT_TRUE(record.satisfied) << "attempt " << attempt;
		EXPECT_EQ(std::get<0>(values), 1) << "attempt " << attempt;
		EXPECT_EQ(std::get<1>(values), 2) << "attempt " << attempt;
		EXPECT_EQ(live_frames, 0) << "attempt " << attempt;

		return record.resumed_on != std::this_thread::get_id();
	}

	/** Awaits a when_all whose children all complete inside the fan-out. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the record is owned by the calling test, which blocks in sync_wait until this completes.
	alt::task<void> ready_children_probe(wait_record& record, std::tuple<int, int>& values)
	{
		const auto result = co_await alt::when_all(make_value_task(1), make_value_task(2));
		record_outcome(record, result);

		if(result)
		{
			values = {std::get<0>(*result), std::get<1>(*result)};
		}
	}

	/** Awaits a when_all with no children at all. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the record is owned by the calling test, which blocks in sync_wait until this completes.
	alt::task<void> no_children_probe(wait_record& record, std::size_t& positions)
	{
		const auto result = co_await alt::when_all();
		record_outcome(record, result);

		if(result)
		{
			positions = std::tuple_size_v<std::remove_cvref_t<decltype(*result)>>;
		}
	}
}

class WhenAllTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(WhenAllTest, TwoReadyChildrenYieldBothResults)
{
	const auto result = alt::sync_wait(alt::when_all(make_value_task(1), make_value_task(std::string{"two"})));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(std::get<0>(*result), 1);
	EXPECT_EQ(std::get<1>(*result), "two");
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, ResultsAreInArgumentOrderNotCompletionOrder)
{
	alt::task_completion_source<int> first;
	alt::task_completion_source<int> second;
	wait_record                      record;
	std::tuple<int, int>             values{0, 0};

	const auto handle = alt::start(order_root(first, second, record, values));
	ASSERT_EQ(record.resumptions, 0);

	// Completion order is the reverse of argument order.
	second.set_value(2);
	EXPECT_EQ(record.resumptions, 0) << "one child cannot satisfy the wait";

	first.set_value(1);

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(std::get<0>(values), 1) << "position zero is the first argument's value";
	EXPECT_EQ(std::get<1>(values), 2);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, AVoidChildOccupiesItsPositionAsMonostate)
{
	const auto result = alt::sync_wait(alt::when_all(make_void_task(), make_value_task(5)));

	ASSERT_TRUE(result.has_value());
	static_assert(std::same_as<std::tuple_element_t<0, std::remove_cvref_t<decltype(*result)>>, std::monostate>);
	EXPECT_EQ(std::get<1>(*result), 5);
}

TEST_F(WhenAllTest, AReferenceChildYieldsAWrapperAroundTheSameObject)
{
	int target = 7;

	const auto result = alt::sync_wait(alt::when_all(make_reference_task(target)));

	ASSERT_TRUE(result.has_value());
	static_assert(std::same_as<std::tuple_element_t<0, std::remove_cvref_t<decltype(*result)>>, std::reference_wrapper<int>>);
	EXPECT_EQ(&std::get<0>(*result).get(), &target) << "the wrapper refers to the caller's object";

	target = 9;
	EXPECT_EQ(std::get<0>(*result).get(), 9);
}

TEST_F(WhenAllTest, NoChildrenCompletesWithAnEmptyTuple)
{
	wait_record record;
	std::size_t positions = 1;

	// Nothing else in this test can resume the probe: there is no second thread, no
	// primitive and no parked handle. So sync_wait returning at all is the proof that
	// await_suspend returned false and handed the frame straight back. A suspension
	// point is still entered, await_ready being unconditionally false.
	alt::sync_wait(no_children_probe(record, positions));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(positions, 0U);
	EXPECT_FALSE(parked) << "no awaiter handed this frame away";
	EXPECT_EQ(record.resumed_on, std::this_thread::get_id());
}

TEST_F(WhenAllTest, EveryChildAlreadySatisfiedCompletesWithoutSuspending)
{
	wait_record          record;
	std::tuple<int, int> values{0, 0};

	// Both children run to completion inside the fan-out, so the count reaches zero as
	// the starter retires and the join refuses the link. Same argument as
	// NoChildrenCompletesWithAnEmptyTuple: nothing here could resume this frame.
	alt::sync_wait(ready_children_probe(record, values));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(std::get<0>(values), 1);
	EXPECT_EQ(std::get<1>(values), 2);
	EXPECT_FALSE(parked) << "no awaiter handed this frame away";
	EXPECT_EQ(record.resumed_on, std::this_thread::get_id());
}

TEST_F(WhenAllTest, AChildThatThrewRethrowsAtTheAwait)
{
	// The result is bound rather than discarded. A cast to void satisfies the compiler's
	// unused-result warning but not clang-tidy-19's bugprone-unused-return-value, which
	// was measured firing on the cast form here: that check's default
	// CheckedReturnTypes includes std::expected, which is what sync_wait returns for a
	// when_all, and its AllowCastToVoid defaults off. The statement throws, so nothing
	// is ever assigned; the binding exists to name the value the throw replaced.
	EXPECT_THROW({ [[maybe_unused]] const auto results = alt::sync_wait(alt::when_all(make_throwing_task(), make_value_task(5))); }, std::runtime_error);

	// The other child's result is discarded with the slots, and every frame is gone.
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, TwoThrowingChildrenRethrowTheLowestPositionNotTheFirstToThrow)
{
	alt::task_completion_source<int> first;
	std::atomic<bool>                second_threw{false};

	// Position 0 is parked on its source when position 1 is started, so position 1 throws
	// first. Only once it has thrown does this thread fail position 0, which therefore
	// completes last. The rethrown exception is position 0's, so the rule is position
	// order rather than completion order.
	std::jthread failer{[&] {
		while(!second_threw.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}

		first.set_exception(std::make_exception_ptr(std::runtime_error{"position 0"}));
	}};

	std::string caught;

	try
	{
		[[maybe_unused]] const auto results = alt::sync_wait(alt::when_all(first, make_signalling_throwing_task("position 1", second_threw)));
		ADD_FAILURE() << "neither child's exception reached the await";
	}
	catch(const std::runtime_error& error)
	{
		caught = error.what();
	}

	EXPECT_EQ(caught, "position 0");
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, ASuccessfulSiblingRunsToItsEndAfterAnotherChildThrows)
{
	alt::task_completion_source<int> source;
	std::atomic<bool>                sibling_finished{false};
	std::string                      caught;
	bool                             returned         = false;
	int                              frames_at_return = -1;

	// Single-threaded, so there is no race to hope for. Position 0 throws inside the
	// fan-out, and position 1 is parked on a source that only this thread can set, so
	// when start() returns the exception is already sitting in its slot and the sibling
	// has not finished. An await that parked only until the first failure -- or that
	// returned without parking -- would rethrow into the body right here, before the
	// sibling's completion.
	const auto handle = alt::start(throwing_sibling_root(source, sibling_finished, caught, returned, frames_at_return));

	EXPECT_FALSE(returned) << "the await returned while a child was still running";
	EXPECT_FALSE(sibling_finished.load(std::memory_order_acquire));

	// The sibling is the only tracked frame, and it is parked: the thrower's frame was
	// built and unwound inside the fan-out, so a count of 1 says the throw at position 0
	// did not keep position 1 from being started and reaching its suspension point.
	EXPECT_EQ(live_frames, 1) << "the sibling's frame should be alive and parked here";

	// Releasing the sibling resumes it on this thread; its completion retires the last
	// child, which resumes the chain, which rethrows.
	source.set_value(1);

	EXPECT_TRUE(returned);
	EXPECT_EQ(caught, "child body");
	EXPECT_TRUE(sibling_finished.load(std::memory_order_acquire));

	// Every child body ran to its end before the await returned -- not that the frames
	// were freed, which this cannot see: live_frames counts body-scoped trackers. Read at
	// the instant the await returned rather than here, because by the time this thread is
	// back the sibling has finished either way, so a live_frames read here cannot tell
	// the two apart. An await that returned while the sibling was still parked leaves
	// this at 1.
	EXPECT_EQ(frames_at_return, 0) << "the await returned with a child frame still alive";
}

TEST_F(WhenAllTest, TheChildrenInheritTheChainsHomeSchedulerForTheirTeardown)
{
	alt::manual_scheduler home;
	alt::async_event      first;
	alt::async_event      second;
	wait_record           record;

	auto handle = alt::start(home_root(first, second, record), {.home = alt::scheduler_ref{home}});
	ASSERT_EQ(live_frames, 2) << "both children are parked on their own event";

	handle.request_stop();

	// A child's frame is not destroyed by the stop that cancels it. Its teardown is
	// posted to the chain's home, the only way the child can have got that home, since
	// the test sets start_options::home on the root alone. The chain's own frame is not
	// posted: its body ran to its end.
	EXPECT_EQ(live_frames, 2) << "posted, not destroyed";
	EXPECT_EQ(home.drain(), 2U) << "one teardown per child";
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(record.error, released_error);
}

TEST_F(WhenAllTest, AnAwaitSuspendThatThrowsIsCaughtByTheChildRatherThanTerminating)
{
	// See the binding note in AChildThatThrewRethrowsAtTheAwait.
	EXPECT_THROW({ [[maybe_unused]] const auto results = alt::sync_wait(alt::when_all(throwing_suspend{}, make_value_task(5))); }, std::runtime_error);
	EXPECT_EQ(live_frames, 0);
}

// A when_all that PARKS in a promise with no cancellation context, which is the case
// the plan named and which no test covered: this one and
// EveryChildAlreadySatisfiedCompletesWithoutSuspending drove byte-identical probes until
// the final review noticed. An alt::task driven by sync_wait relays cancellation but has
// no root above it, so cancellation_context_pointer() is null -- no token to link, no
// home, and no stop callback for the joiner -- and the await must still park on the
// counter and be resumed by the last child.
//
// That it parked is confirmed after the fact rather than arranged, because nothing
// outside when_all_awaiter can observe the join's park: the thread that sets the gate is
// released from inside the fan-out, so it may win the race and complete the child before
// the chain parks, and then the chain is resumed on this thread instead. Which thread
// ran the body is the discriminator, and the loop is what makes the test deterministic
// despite the race. In practice the first attempt parks: the releasing thread has to
// wake from a spin and traverse task_completion_source::set_value while the chain has
// only to retire the starter and link.
TEST_F(WhenAllTest, AWhenAllThatParksInAPromiseWithNoCancellationContextStillCompletes)
{
	bool parked_once = false;

	for(int attempt = 0; attempt < contextless_park_attempts && !parked_once; ++attempt)
	{
		parked_once = run_one_contextless_park(attempt);
	}

	ASSERT_TRUE(parked_once) << "the await never parked in " << contextless_park_attempts << " attempts, so nothing here exercised a contextless park";
}

TEST_F(WhenAllTest, CancellingTheAwaitingChainReportsOperationCanceled)
{
	alt::task_completion_source<int> first;
	alt::task_completion_source<int> second;
	wait_record                      record;

	auto handle = alt::start(source_root(first, second, record));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

// Also the coverage of record for what the combinators brief planned as
// CancellingTheChainWhileEveryChildIsStoppedReportsCanceledNotSatisfied: the children's
// stop drives the counter to zero here, and the verdict below is the cancellation rather
// than a satisfied wait over unfilled slots. That test was not written separately because
// this one makes its assertions already. Its stated mechanism -- the cancelled() flag
// deciding the verdict -- is not the one at work; see the await_resume note in
// when_all.hpp and the note in combinator_state::cancel().
TEST_F(WhenAllTest, CancellingTheAwaitingChainRequestsEveryChildsStop)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;

	auto handle = alt::start(event_root(first, second, record));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2) << "both children are parked on their own event";

	handle.request_stop();

	EXPECT_EQ(live_frames, 0) << "every child's frame is gone";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
}

TEST_F(WhenAllTest, TheParentsBodyRunsOnAfterACancelledWait)
{
	alt::task_completion_source<int> first;
	alt::task_completion_source<int> second;
	wait_record                      record;

	auto handle = alt::start(source_root(first, second, record));
	handle.request_stop();

	// The result, not a torn-down frame, is what told the body it was cancelled: the
	// statement after the co_await ran, exactly once, and it ran HERE -- inline on the
	// thread that requested the stop, which is the half
	// CancellingTheAwaitingChainReportsOperationCanceled does not assert and the only
	// reason this test is not that one twice over. A wait_record::body_ran_on flag used
	// to stand in for the first of these; it was set by the same function that increments
	// resumptions, so it could never differ from resumptions > 0, and it is gone.
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.resumed_on, std::this_thread::get_id()) << "the cancelled chain's body is resumed by the stop, not posted";
}

TEST_F(WhenAllTest, TheChildrensStopOutlivesABlockTheLastChildFreed)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;

	auto handle = alt::start(sibling_release_root(first, second, record));
	ASSERT_EQ(live_frames, 2);

	// The chain is released before the children are stopped, so it drops the awaiter's
	// reference first and the block's LAST reference is then released by a child's
	// completion hook from inside the children's request_stop(). Whatever that stop is
	// still walking has to outlive the block it just freed.
	//
	// The second child's teardown satisfies the first child's wait, which puts the last
	// release inside the walk's *first* callback rather than its last one. That is the
	// configuration that matters: libstdc++ skips its final relock after the last
	// callback in the list, so a release during any earlier one is the case that reaches
	// the state again afterwards. See the DELICATE note on combinator_state::cancel().
	//
	// This test is implementation-order-dependent. It relies on libstdc++ invoking stop
	// callbacks in reverse registration order and skipping the relock after the last
	// one. On a standard library that invokes them forward, the release lands during the
	// last callback instead, the use-after-free is no longer reached, and the test still
	// passes: a green run on another toolchain is not evidence the counted copy works.
	handle.request_stop();

	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
}

TEST_F(WhenAllTest, AStopDuringTheFanOutStopsTheChildrenStartedAfterIt)
{
	std::stop_source external;
	int              suspends = 0;
	wait_record      record;

	alt::start_options options{};
	options.token = external.get_token();

	// The first child requests the stop from inside its own start, so the stop lands
	// between the two children being started.
	const auto handle = alt::start(fan_out_stop_root(external, suspends, record), options);

	EXPECT_EQ(suspends, 0) << "the second child died at its first suspension point";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	// A child that did reach its awaitable is parked on it; release it so the block and
	// its frame are not leaked when this assertion is the one that failed.
	if(parked)
	{
		resume_parked();
	}

	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, AChildCompletingOnAnotherThreadDuringTheFanOutDoesNotResumeEarly)
{
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_event         first;
		alt::async_event         second;
		std::atomic<std::size_t> arrived{0};
		wait_record              record;

		std::jthread completer{[&] {
			rendezvous(arrived, 2);
			first.set();
		}};

		const auto handle = alt::start(racing_root(first, second, arrived, record));

		completer.join();

		// One child cannot satisfy the wait however the race landed: the starter is
		// outstanding until the fan-out is over, and the second child is still parked.
		EXPECT_EQ(record.resumptions, 0) << "repeat " << repeat;

		second.set();

		EXPECT_EQ(record.resumptions, 1) << "repeat " << repeat;
		EXPECT_TRUE(record.satisfied) << "repeat " << repeat;
		EXPECT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

// The range form. Its element type is the range's value type, which a when_all owns and
// moves into the children, so a vector of tasks yields values rather than references.
using range_of_value_tasks = decltype(alt::when_all(std::declval<std::vector<alt::task<int>>>()));
using range_of_void_tasks  = decltype(alt::when_all(std::declval<std::vector<alt::task<void>>>()));

static_assert(alt::awaitable<range_of_value_tasks>);
static_assert(std::same_as<alt::await_result_t<range_of_value_tasks>, std::expected<std::vector<int>, std::error_code>>);
static_assert(std::same_as<alt::await_result_t<range_of_void_tasks>, std::expected<std::vector<std::monostate>, std::error_code>>);

namespace
{
	/** What a move_counted awaitable's moves are tallied into. */
	struct move_tally
	{
		int moves = 0;
	};

	/** An awaitable that is ready at once, move-only, and counts every move of itself. */
	class move_counted
	{
	public:
		explicit move_counted(move_tally* tally): m_tally(tally)
		{
		}

		move_counted(move_counted&& other) noexcept: m_tally(other.m_tally)
		{
			++m_tally->moves;
		}

		move_counted(const move_counted&)            = delete;
		move_counted& operator=(const move_counted&) = delete;
		move_counted& operator=(move_counted&&)      = delete;
		~move_counted()                              = default;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{
		}

		[[nodiscard]] int await_resume() const noexcept
		{
			return 1;
		}

	private:
		move_tally* m_tally;
	};

	/**
	 * A sized range that counts how often it is iterated.
	 *
	 * It counted size() calls too, and nothing ever read the count, so the "iterated
	 * once" claim rested on begins alone either way. The counter is gone rather than
	 * asserted because the real figure is two -- when_all's fan-out asks for the size
	 * once for the block and once more for a local that only the debug build's asserts
	 * read -- and that second call is exactly the thing a reviewer has already proposed
	 * guarding with NDEBUG. Pinning it here would make this test fail on a change that
	 * has nothing to do with what it is for.
	 */
	struct counting_range
	{
		std::vector<move_counted> items;
		int*                      begins;

		[[nodiscard]] auto begin()
		{
			++*begins;
			return items.begin();
		}

		[[nodiscard]] auto end()
		{
			return items.end();
		}

		[[nodiscard]] std::size_t size() const
		{
			return items.size();
		}
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

	/** An awaitable ready with a value that records when its contents were moved away. */
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

	/** Both an awaitable and a sized range: the argument-list overload must take it. */
	struct awaitable_range: copyable_ready
	{
		[[nodiscard]] int* begin() const noexcept
		{
			return nullptr;
		}

		[[nodiscard]] int* end() const noexcept
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

	/** A task whose body throws a runtime_error saying @p message. */
	alt::task<int> make_throwing_task_with(std::string message)
	{
		throw std::runtime_error{message};
		co_return 0;
	}

	/** Parks in a when_all over two tasks that yield distinct values, and stores them. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task range_value_root(alt::async_event& first, alt::async_event& second, wait_record& record, std::vector<int>& values)
	{
		std::vector<alt::task<int>> children;
		children.push_back(wait_then_yield(first, 10));
		children.push_back(wait_then_yield(second, 20));

		const auto result = co_await alt::when_all(std::move(children));
		record_outcome(record, result);

		if(result)
		{
			values = *result;
		}
	}

	/** Moves @p elements counting awaitables through a range-form when_all; returns the moves. */
	int moves_for_a_range_of(const int elements, int& begins)
	{
		move_tally tally;

		counting_range range{.items = {}, .begins = &begins};
		range.items.reserve(static_cast<std::size_t>(elements));

		for(int i = 0; i < elements; ++i)
		{
			range.items.emplace_back(&tally);
		}

		const auto result = alt::sync_wait(alt::when_all(std::move(range)));

		EXPECT_TRUE(result.has_value());
		EXPECT_EQ(result->size(), static_cast<std::size_t>(elements));
		return tally.moves;
	}

	/** Parks in a when_all over a vector of tasks that each park on an event of their own. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which holds them until live_frames is back to zero.
	alt::cancellable_task range_event_root(alt::async_event& first, alt::async_event& second, wait_record& record)
	{
		std::vector<alt::task<alt::wait_result>> children;
		children.push_back(tracked_wait(first, 1));
		children.push_back(tracked_wait(second, 2));

		const auto result = co_await alt::when_all(std::move(children));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}
}

TEST_F(WhenAllTest, ARangeOfTasksYieldsAVectorOfResults)
{
	std::vector<alt::task<int>> children;
	children.push_back(make_value_task(1));
	children.push_back(make_value_task(2));
	children.push_back(make_value_task(3));

	const auto result = alt::sync_wait(alt::when_all(std::move(children)));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, (std::vector<int>{1, 2, 3})) << "results are in range order";
}

TEST_F(WhenAllTest, ARangeOfVoidTasksYieldsAVectorOfMonostate)
{
	std::vector<alt::task<void>> children;
	children.push_back(make_void_task());
	children.push_back(make_void_task());

	const auto result = alt::sync_wait(alt::when_all(std::move(children)));

	ASSERT_TRUE(result.has_value());
	static_assert(std::same_as<std::remove_cvref_t<decltype(*result)>, std::vector<std::monostate>>);
	EXPECT_EQ(result->size(), 2U);
}

TEST_F(WhenAllTest, AnEmptyRangeYieldsAnEmptyVector)
{
	// The block is allocated for no children and released by the awaiter alone; ASan
	// is what reports it if that release is missing.
	const auto result = alt::sync_wait(alt::when_all(std::vector<alt::task<int>>{}));

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->empty());
}

TEST_F(WhenAllTest, ARangeChildThatThrewRethrowsAtTheAwait)
{
	std::vector<alt::task<int>> children;
	children.push_back(make_value_task(1));
	children.push_back(make_throwing_task());
	children.push_back(make_value_task(3));

	EXPECT_THROW(
	  {
		  [[maybe_unused]] const auto result = alt::sync_wait(alt::when_all(std::move(children)));
	  },
	  std::runtime_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, CancellingAChainParkedOnARangeFormStopsEveryChild)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;

	auto handle = alt::start(range_event_root(first, second, record));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2) << "both children are parked on their own event";

	handle.request_stop();

	EXPECT_EQ(live_frames, 0) << "every child's frame is gone";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
}

TEST_F(WhenAllTest, ARangeIsIteratedOnceAndItsElementsAreMovedIntoTheChildren)
{
	constexpr int small = 4;
	constexpr int large = 8;

	int small_begins = 0;
	int large_begins = 0;

	const int small_moves = moves_for_a_range_of(small, small_begins);
	const int large_moves = moves_for_a_range_of(large, large_begins);

	EXPECT_EQ(small_begins, 1) << "the range is iterated exactly once";
	EXPECT_EQ(large_begins, 1);

	// move_counted has no copy constructor, so this compiling is the no-copy guarantee:
	// an element can only reach its child by being moved. What a defect would change
	// is how many times, so the cost is held to a bound and to linearity in the count,
	// not to the exact figure the compiler's coroutine lowering happens to produce.
	EXPECT_EQ(small_moves % small, 0);
	EXPECT_EQ(large_moves % large, 0);
	EXPECT_EQ(small_moves / small, large_moves / large) << "moves per element do not depend on the range's size";
	EXPECT_GT(small_moves, 0) << "the elements reached their children by moving";
	EXPECT_LE(small_moves / small, 4) << "a handful of moves per element, none made per sibling";
}

TEST_F(WhenAllTest, ALvalueRangeIsMovedFromAndYieldsItsValues)
{
	std::vector<alt::task<int>> children;
	children.push_back(make_value_task(4));
	children.push_back(make_value_task(5));

	const auto result = alt::sync_wait(alt::when_all(children));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, (std::vector<int>{4, 5}));
	EXPECT_EQ(children.size(), 2U) << "the range itself is bound, not consumed";
}

TEST_F(WhenAllTest, ALvalueRangeIsLeftHoldingMovedFromElements)
{
	std::vector<flagged_ready> children;
	children.emplace_back(3);
	children.emplace_back(4);

	const auto result = alt::sync_wait(alt::when_all(children));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, (std::vector<int>{3, 4}));
	EXPECT_TRUE(children[0].moved_from);
	EXPECT_TRUE(children[1].moved_from);
}

TEST_F(WhenAllTest, AConstRangeOfCopyableAwaitablesIsCopiedIntoTheChildren)
{
	const std::vector<copyable_ready> children{{.value = 7}, {.value = 8}};

	const auto result = alt::sync_wait(alt::when_all(children));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, (std::vector<int>{7, 8}));

	// There was an EXPECT_EQ(children[0].value, 7) here. It could not fail: nothing in
	// the implementation could modify a const element without undefined behavior, so it
	// asserted a language rule rather than this overload's behavior. What actually pins
	// "copied, not moved" is that a const range compiles at all -- the static_assert
	// below -- since a const element cannot be moved from, and the values above, which
	// came out of the children.
}

// A const range of move-only elements cannot be moved from, and is rejected up front
// rather than failing inside the awaiter.
template<typename T>
concept when_all_accepts = requires(T&& argument) { alt::when_all(std::forward<T>(argument)); };

static_assert(!when_all_accepts<const std::vector<alt::task<int>>&>);
static_assert(when_all_accepts<std::vector<alt::task<int>>&>);
static_assert(when_all_accepts<const std::vector<copyable_ready>&>);

// A type that is both an awaitable and a sized range is one child of the argument-list form.
static_assert(std::same_as<alt::await_result_t<decltype(alt::when_all(std::declval<awaitable_range>()))>,
                           std::expected<std::tuple<int>, std::error_code>>);

TEST_F(WhenAllTest, ARangeYieldsResultsByPositionWhenTheChildrenCompleteInReverse)
{
	alt::async_event first;
	alt::async_event second;
	wait_record      record;
	std::vector<int> values;

	auto handle = alt::start(range_value_root(first, second, record, values));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 2);

	second.set();
	ASSERT_EQ(record.resumptions, 0) << "the first child is still parked";
	first.set();

	ASSERT_TRUE(record.satisfied);
	EXPECT_EQ(values, (std::vector<int>{10, 20})) << "position is range order, not completion order";
	EXPECT_EQ(live_frames, 0);
}

TEST_F(WhenAllTest, ARangeWithTwoThrowingChildrenRethrowsTheLowestPositions)
{
	std::vector<alt::task<int>> children;
	children.push_back(make_value_task(1));
	children.push_back(make_throwing_task_with("second position"));
	children.push_back(make_throwing_task_with("third position"));

	try
	{
		[[maybe_unused]] const auto result = alt::sync_wait(alt::when_all(std::move(children)));
		FAIL() << "the await did not throw";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_STREQ(error.what(), "second position");
	}
}

// The two paths that falsified the first revision of the combinators design, plus the
// spec's direct "no child left running" requirement. Revision 1 claimed a combinator
// could request a stop on each child and have every child gone before teardown
// returned. Both paths below break that claim on ordinary code, which is why the
// results, the counter and the children's stop source live in a refcounted block
// instead of in the awaiting frame.
namespace
{
	/** How many times the satisfier-against-stop race repeats. */
	constexpr int stop_race_repeats = 200;

	/** What the raced child recorded about its own wait, from inside the child. */
	struct child_record
	{
		/** Whether the child's body reached the statement after its co_await at all. */
		bool resumed = false;

		/**
		 * Whether waiter_list::claim handed the child's node to the satisfier rather
		 * than to the stop callback. True is revision 1's path (a): the stop released
		 * nothing and the child ran on.
		 */
		bool satisfier_won = false;

		/** The value the satisfier published, as the child observed it. */
		int value = 0;
	};

	/** A tracked task that parks on @p source and records which party resumed it. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both references are owned by the calling test, which joins the racing thread and waits for live_frames to reach zero before leaving its scope.
	alt::task<std::expected<int, std::error_code>> tracked_source_wait(alt::task_completion_source<int>& source, child_record& child, int tag)
	{
		const frame_tracker tracker{tag};
		auto                result = co_await source;

		child.resumed       = true;
		child.satisfier_won = result.has_value();

		if(result)
		{
			child.value = *result;
		}

		co_return result;
	}

	/**
	 * Parks in a when_all over a single child, the shape that lets a satisfier and a
	 * stop race for the same waiter node. @p values_seen counts the satisfied results
	 * whose slot held the value the satisfier published.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which joins the racing thread and waits for live_frames to reach zero before leaving their scope.
	alt::cancellable_task one_child_root(alt::task_completion_source<int>& source, child_record& child, wait_record& record, int& values_seen)
	{
		const auto result = co_await alt::when_all(tracked_source_wait(source, child, 1));
		record_outcome(record, result);

		if(result)
		{
			// Reading position 0 at all is half the assertion. A satisfied verdict over a
			// slot no child body filled does not return a wrong value here: result_store
			// asserts on "reading an alt coroutine result before it was produced", so the
			// read either sees the satisfier's value or aborts the process.
			const auto& slot = std::get<0>(*result);

			if(slot.has_value() && *slot == 1)
			{
				++values_seen;
			}
		}

		co_await alt::cancellation_point{};
	}

	/** Parks in a when_all over two homed children and then gives the stop somewhere to land. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the events and the record are owned by the calling test, which drains the home scheduler and waits for live_frames to reach zero before leaving their scope.
	alt::cancellable_task homed_cancel_root(alt::async_event& first, alt::async_event& second, wait_record& record)
	{
		const frame_tracker tracker{0};
		const auto          result = co_await alt::when_all(homed_wait{first, 1}, homed_wait{second, 2});
		record_outcome(record, result);

		// The suspension point the cancelled chain is torn down at. With a home
		// scheduler abandon_chain posts this frame's destruction rather than running
		// it, which is what defers the parent past its children.
		co_await alt::cancellation_point{};
	}

	/** A tracked task that parks on @p gate and counts every resumption of its own body. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event and the counter are owned by the calling test, which holds them until live_frames is back to zero.
	alt::task<alt::wait_result> wait_counting_resumptions(alt::async_event& gate, int tag, int& resumptions)
	{
		const frame_tracker    tracker{tag};
		const alt::wait_result result = co_await gate;
		++resumptions;
		co_return result;
	}

	/** Parks in a when_all over three children, each on an event of its own. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them all until live_frames is back to zero.
	alt::cancellable_task three_child_root(alt::async_event& first, alt::async_event& second, alt::async_event& third, int& resumptions, wait_record& record)
	{
		const auto result = co_await alt::when_all(wait_counting_resumptions(first, 1, resumptions),
		                                           wait_counting_resumptions(second, 2, resumptions),
		                                           wait_counting_resumptions(third, 3, resumptions));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Takes a satisfied when_all's results and then parks again, so the chain outlives the block. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them all until live_frames is back to zero.
	alt::cancellable_task satisfied_then_parked_root(alt::async_event& first, alt::async_event& second, alt::async_event& after, wait_record& record)
	{
		const frame_tracker tracker{0};
		const auto          result = co_await alt::when_all(tracked_wait(first, 1), tracked_wait(second, 2));
		record_outcome(record, result);

		// Parks the chain again, so its stop state is still live and still carries a
		// callback when the test requests the stop. The when_all's own link went with
		// the block, which the last child released above.
		[[maybe_unused]] const alt::wait_result parked_again = co_await after;
	}

	/** A tracked task that parks in a when_all of its own, counting a cancelled inner wait. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them all until live_frames is back to zero.
	alt::task<int> nested_pair(alt::async_event& first, alt::async_event& second, int tag, int& inner_cancelled)
	{
		const frame_tracker tracker{tag};
		const auto          result = co_await alt::when_all(tracked_wait(first, tag + 10), tracked_wait(second, tag + 20));

		if(!result)
		{
			++inner_cancelled;
		}

		co_return result ? 1 : 0;
	}

	/** Parks in a when_all whose two children each park in a when_all of their own. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which holds them all until live_frames is back to zero.
	alt::cancellable_task nested_root(alt::async_event& first, alt::async_event& second, alt::async_event& third, alt::async_event& fourth, int& inner_cancelled, wait_record& record)
	{
		const auto result = co_await alt::when_all(nested_pair(first, second, 1, inner_cancelled), nested_pair(third, fourth, 2, inner_cancelled));
		record_outcome(record, result);
		co_await alt::cancellation_point{};
	}

	/** Everything one repeat of the satisfier-against-stop race produced. */
	struct race_record
	{
		/** What the child saw: which party resumed it, and with what value. */
		child_record child;

		/** What the awaiting chain saw: its verdict and how often it resumed. */
		wait_record parent;

		/** Counts the chain's own reads of slot 0 that found the satisfier's value. */
		int values_seen = 0;
	};

	/**
	 * @brief Runs one repeat of revision 1's path (a). Asserts nothing about the outcome.
	 *
	 * Split from the two check_the_race_* functions below, and all three split out of
	 * the test's loop, because one function holding the race and its assertions is over
	 * clang-tidy's readability-function-cognitive-complexity threshold -- which every
	 * other file in this suite satisfies, so splitting is the fix rather than a
	 * suppression in tests/.clang-tidy.
	 *
	 * @param repeat The repeat index, so a precondition failure names which one.
	 * @param ran    Filled with what the child and the chain each recorded.
	 */
	void run_one_stop_race(const int repeat, race_record& ran)
	{
		reset_coroutine_test_state();

		alt::task_completion_source<int> source;
		std::atomic<std::size_t>         arrived{0};

		auto handle = alt::start(one_child_root(source, ran.child, ran.parent, ran.values_seen));
		ASSERT_EQ(ran.parent.resumptions, 0) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		// The child is parked on one waiter node, and waiter_list::claim elects between
		// the satisfier below and the waiter_stop_release that the children's stop
		// fires. detail::waiter_stop_release resumes nothing unless claim returns
		// detached, so when the satisfier wins the node the stop callback is a no-op:
		// request_stop returns having released nothing, and the child carries on -- on
		// the satisfier's thread -- to write its slot and fire its completion hook into
		// a block the awaiting chain has usually abandoned already.
		std::jthread satisfier{[&] {
			rendezvous(arrived, 2);
			source.set_value(1);
		}};

		rendezvous(arrived, 2);
		handle.request_stop();

		satisfier.join();
	}

	/**
	 * @brief Checks that exactly one party resolved the race and nothing was left behind.
	 *
	 * Either party may win and both are correct outcomes, so this says nothing about
	 * which did. What is not allowed is a second resumption, a child nobody resumed, or
	 * a frame left behind. Under AddressSanitizer and ThreadSanitizer the repeat is also
	 * the report of record for the block being touched from both threads at once: the
	 * chain releases its reference on one thread while the child retires into the same
	 * block on the other.
	 */
	void check_the_race_resolved_once(const int repeat, const race_record& ran)
	{
		EXPECT_EQ(ran.parent.resumptions, 1) << "repeat " << repeat;
		EXPECT_TRUE(ran.child.resumed) << "repeat " << repeat << ": exactly one party resumed the child";
		EXPECT_EQ(live_frames, 0) << "repeat " << repeat;
	}

	/**
	 * @brief Checks that what each party reported agrees with which of them won the node.
	 *
	 * Written as unconditional truths over derived predicates rather than as assertions
	 * inside ifs. Both forms say the same thing, and each of these holds on either side
	 * of the race; the branchless form is also what keeps this function under
	 * clang-tidy's cognitive-complexity threshold, which a GoogleTest assertion nested
	 * in an if is expensive enough to break.
	 */
	void check_the_race_reported_consistently(const int repeat, const race_record& ran)
	{
		const bool value_is_the_satisfiers  = !ran.child.satisfier_won || ran.child.value == 1;
		const bool slot_read_is_the_value   = !ran.parent.satisfied || ran.values_seen == 1;
		const bool verdict_carries_a_reason = ran.parent.satisfied || ran.parent.error == released_error;

		// Each message streams what was seen as well as what was expected, because a
		// bare EXPECT_TRUE over a derived predicate prints only "false" and a CI failure
		// in a 200-repeat loop should be diagnosable without rerunning it.
		EXPECT_TRUE(value_is_the_satisfiers) << "repeat " << repeat << ": a child the satisfier won read the published value; satisfier_won=" << ran.child.satisfier_won << " value=" << ran.child.value;
		EXPECT_TRUE(slot_read_is_the_value) << "repeat " << repeat << ": a satisfied verdict read the satisfier's value from slot 0; satisfied=" << ran.parent.satisfied << " values_seen=" << ran.values_seen;
		EXPECT_TRUE(verdict_carries_a_reason) << "repeat " << repeat << ": a released verdict reported operation_canceled; satisfied=" << ran.parent.satisfied << " error=" << ran.parent.error.message();
	}

	/** How many times the teardown-against-stop handshake repeats. */
	constexpr int teardown_race_repeats = 50;

	/** The value the tripwire child produces, read back by the chain out of its slot. */
	constexpr int tripwire_tag = 71;

	/**
	 * How many moves from the child's own result the copy inside the shared block is.
	 *
	 * Several objects carry one child's value, and only one of them lives in the block:
	 * when_all_child materializes the child's result as a temporary and
	 * result_store::set_value move-constructs the slot's copy from it, so the slot's is
	 * the first move-target. Everything further along -- what
	 * assemble_combinator_results builds, and what the std::expected around it holds --
	 * is a later one, which is why this counts moves rather than destructions: the
	 * number of moves downstream of the slot is the compiler's business and was in fact
	 * one higher than first assumed, while the step into the slot is a reference
	 * binding followed by one variant emplacement either way.
	 */
	constexpr int tripwire_block_hops = 1;

	/**
	 * @brief A child's result value whose destruction opens the window C1 lives in.
	 *
	 * A child's value is stored in a result slot inside the shared block.
	 * when_all_awaiter::await_resume moves it out into the result the chain reads and
	 * leaves the moved-from object in the slot, where ~when_all_state destroys it --
	 * and ~when_all_state runs from the release() inside ~when_all_awaiter. Destroying
	 * the slot's copy is therefore the last instant inside the block's teardown that a
	 * test can reach: after ~combinator_state has destroyed the completion counter and
	 * its waiter_list, and before the operator delete that frees the storage. So a stop
	 * released here reaches a waiter_list that has been destroyed, which is what the
	 * reset in ~when_all_awaiter prevents. The same window a few hundred nanoseconds
	 * later, with the storage freed rather than merely destroyed, is reachable only by
	 * timing -- see the note on the test -- and no rendezvous a test can write lands
	 * there, because nothing of ours runs between that free and ~m_stop.
	 *
	 * Only the copy in the block -- tripwire_block_hops moves from the child's own
	 * result -- releases the stop thread and holds the teardown open. The child's
	 * temporary is destroyed during the fan-out, and the chain's own copies are
	 * destroyed inside await_resume and in the chain's body, so a stop released by any
	 * of those would land somewhere the window does not exist.
	 *
	 * The hold is a handshake and not a delay: it waits for the stop thread's
	 * request_stop() to return, which is after every callback on the chain's token has
	 * run. The window is therefore opened by construction on every repeat rather than
	 * on the repeats whose timing happens to align, which matters because the window a
	 * delay would have to straddle is a few tens of nanoseconds wide.
	 */
	class teardown_tripwire
	{
	public:
		teardown_tripwire(int* opened, std::atomic<bool>* reached, std::atomic<bool>* stopped): m_opened(opened), m_reached(reached), m_stopped(stopped)
		{
		}

		teardown_tripwire(teardown_tripwire&& other) noexcept:
		  m_opened(other.m_opened),
		  m_reached(other.m_reached),
		  m_stopped(other.m_stopped),
		  m_tag(other.m_tag),
		  m_hops(other.m_hops + 1)
		{
		}

		teardown_tripwire(const teardown_tripwire&)            = delete;
		teardown_tripwire& operator=(const teardown_tripwire&) = delete;
		teardown_tripwire& operator=(teardown_tripwire&&)      = delete;

		~teardown_tripwire()
		{
			if(m_hops != tripwire_block_hops)
			{
				return;
			}

			++*m_opened;
			m_reached->store(true, std::memory_order_release);
			spin_until(*m_stopped);
		}

		/** The tag this value carries, which travels with it through both moves. */
		[[nodiscard]] int tag() const noexcept
		{
			return m_tag;
		}

	private:
		/** Counts the windows this repeat opened. Written only from the awaiting thread. */
		int* m_opened;

		/** Where the copy inside the block publishes that the teardown has reached it. */
		std::atomic<bool>* m_reached;

		/** Where the stop thread publishes that its request_stop() has returned. */
		std::atomic<bool>* m_stopped;

		/** Carried so that what the chain reads back is this object's own value. */
		int m_tag = tripwire_tag;

		/** How many moves away from the child's own result this copy is. */
		int m_hops = 0;
	};

	/** An awaitable that is ready at once with a teardown_tripwire. */
	struct tripwire_ready
	{
		int*               opened;
		std::atomic<bool>* reached;
		std::atomic<bool>* stopped;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{
		}

		[[nodiscard]] teardown_tripwire await_resume() const noexcept
		{
			return teardown_tripwire{opened, reached, stopped};
		}
	};

	/**
	 * Awaits a when_all whose only child is ready before the join can link, with the
	 * tripwire in its result slot. @p tag_seen takes what the chain read back.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which joins the stopping thread and waits for live_frames to reach zero before leaving their scope.
	alt::cancellable_task tripwire_root(int& opened, std::atomic<bool>& reached, std::atomic<bool>& stopped, wait_record& record, int& tag_seen)
	{
		const frame_tracker tracker{0};
		const auto          result = co_await alt::when_all(tripwire_ready{.opened = &opened, .reached = &reached, .stopped = &stopped});
		record_outcome(record, result);

		if(result)
		{
			tag_seen = std::get<0>(*result).tag();
		}

		// Somewhere for the stop this repeat requested to tear the chain down.
		co_await alt::cancellation_point{};
	}

	/**
	 * @brief Checks what one teardown-against-stop repeat produced.
	 *
	 * Split from the repeat itself, and both split out of the test's loop, for the
	 * reason run_one_stop_race records.
	 *
	 * Why each of the window's three conditions is needed, since none of it is visible
	 * in the assertions. The root is a cancellable_task, so the chain's cancellation
	 * context carries a token that can be stopped and wait_awaiter::await_suspend
	 * registers a callback at all -- a sync_wait-driven alt::task has a null context and
	 * registers nothing, which is why every other ready-children test here is safe. The
	 * one child is ready, so it retires before the join can link, the link is refused
	 * with that callback left engaged, and the awaiter's release() is the block's LAST
	 * reference. And the stop is requested from another thread, which is this library's
	 * documented shutdown idiom.
	 */
	void check_the_teardown_repeat(const int repeat, const wait_record& record, const int tag_seen, const int opened)
	{
		EXPECT_EQ(record.resumptions, 1) << "repeat " << repeat;
		EXPECT_TRUE(record.satisfied) << "repeat " << repeat << ": the stop lands after await_resume, so the verdict is the satisfied one";
		EXPECT_EQ(tag_seen, tripwire_tag) << "repeat " << repeat << ": the chain read its slot before the block was released";
		EXPECT_EQ(live_frames, 0) << "repeat " << repeat;

		// The window was opened exactly once. A repeat that never reached the block's
		// copy of the tripwire released its stop somewhere else entirely and guarded
		// nothing, which is a failure of the test rather than of the code.
		EXPECT_EQ(opened, 1) << "repeat " << repeat << ": the block's teardown reached the tripwire exactly once";
	}

	/**
	 * @brief Runs one repeat of the teardown-against-stop handshake. Asserts nothing.
	 *
	 * Split from its assertions, and both split out of the test's loop, for the reason
	 * run_one_stop_race records.
	 *
	 * @param record   Filled with what the awaiting chain saw.
	 * @param tag_seen Filled with the tag the chain read out of its slot.
	 * @return How many times the block's teardown reached the tripwire, which pins the
	 *         rendezvous: zero means the window was never opened and the repeat guarded
	 *         nothing.
	 */
	int run_one_teardown_race(wait_record& record, int& tag_seen)
	{
		reset_coroutine_test_state();

		std::stop_source         external;
		std::atomic<std::size_t> arrived{0};
		std::atomic<bool>        reached{false};
		std::atomic<bool>        stopped{false};
		int                      opened = 0;

		// The stop travels through an external token rather than the handle alt::start
		// returns, because every child here is ready: the whole chain runs to its end
		// inside start(), so the handle does not exist yet at the instant this repeat is
		// about. Neither party waits on the other's progress except through these two
		// flags: the chain waits for request_stop() to return, and request_stop() waits
		// for nothing the chain holds, so the handshake cannot deadlock.
		std::jthread stopper{[&] {
			rendezvous(arrived, 2);
			spin_until(reached);
			external.request_stop();
			stopped.store(true, std::memory_order_release);
		}};

		rendezvous(arrived, 2);

		[[maybe_unused]] const auto handle = alt::start(tripwire_root(opened, reached, stopped, record, tag_seen), {.token = external.get_token()});

		stopper.join();
		return opened;
	}
}

TEST_F(WhenAllTest, ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock)
{
	int satisfier_wins  = 0;
	int satisfied_waits = 0;

	for(int repeat = 0; repeat < stop_race_repeats; ++repeat)
	{
		race_record ran;

		// ASSERT_NO_FATAL_FAILURE, because an ASSERT_* inside run_one_stop_race returns
		// only from that helper: a fired precondition would otherwise leave the chain it
		// had already started parked forever and run the remaining repeats anyway.
		ASSERT_NO_FATAL_FAILURE(run_one_stop_race(repeat, ran));
		check_the_race_resolved_once(repeat, ran);
		check_the_race_reported_consistently(repeat, ran);

		satisfier_wins += ran.child.satisfier_won ? 1 : 0;
		satisfied_waits += ran.parent.satisfied ? 1 : 0;
	}

	// The path this test exists for, asserted rather than hoped for. A run in which the
	// stop won every single node never entered path (a) at all and proves nothing, so
	// that is a failure of the test rather than a pass of the code.
	//
	// The bound is loose on purpose, but the measurement is not: over six 200-repeat
	// runs in debug the satisfier won 199, 200, 200, 200, 199 and 200 nodes. It wins
	// almost every one because it reaches the node directly, while the stop first
	// travels the chain's token, the block's m_link, cancel(), the children's stop
	// source and the child's own stop_forwarder. The parent's verdict is a different
	// question and came out satisfied 0, 1, 0, 0, 0 and 0 times over those same runs:
	// winning the child's node is not winning the chain's, and an earlier draft of this
	// test asserted on the parent's verdict and so measured 0 or 1 out of 200, which
	// reads as "the window was never hit" and is not what it means.
	EXPECT_GT(satisfier_wins, 0) << "the satisfier never beat the stop; the race window was not hit";

	// Recorded rather than asserted, so that a reader of a failure in this loop knows
	// which mix of outcomes the failing run actually saw.
	RecordProperty("satisfier_wins", satisfier_wins);
	RecordProperty("satisfied_waits", satisfied_waits);
}

TEST_F(WhenAllTest, AHomeSchedulerDefersTheParentsDestructionPastItsChildrens)
{
	alt::manual_scheduler home;
	alt::async_event      first;
	alt::async_event      second;
	wait_record           record;

	auto handle = alt::start(homed_cancel_root(first, second, record), {.home = alt::scheduler_ref{home}});
	ASSERT_EQ(live_frames, 3) << "the chain's own frame and one per parked child";

	// Revision 1's path (b), single-threaded and deterministic: no second thread is
	// involved anywhere below. abandon_chain posts destroy_trampoline instead of
	// destroying in place whenever the context carries a home, so the chain's own
	// teardown is deferred -- and it is posted *first*, from the cancellation point the
	// resumed body reaches, before the children's stop has even been requested.
	//
	// That "first" is implementation-order-dependent, in the same way and for the same
	// reason as TheChildrensStopOutlivesABlockTheLastChildFreed above. Both of this
	// chain's stop callbacks are registered on its token in await_suspend -- the block's
	// m_link first, the joiner's waiter_stop_release second -- and this test's ordering
	// assertion holds because libstdc++ invokes them in reverse registration order, so
	// the joiner is claimed (resuming the chain, which then reaches its cancellation
	// point and posts) before m_link runs cancel() and the children are posted. On a
	// standard library that invokes callbacks forward, the children would be posted
	// first, destruction_order below would read {2, 1, 0}, and this test would fail with
	// the design still sound. The deferral itself -- three posts, nothing destroyed in
	// place -- does not depend on that order; only their sequence does.
	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 3) << "three posted teardowns, nothing destroyed yet";

	// Draining runs them in post order, so the chain's frame -- which in revision 1
	// owned the result slots and the completion counter -- is destroyed while both
	// children are still alive, and each child then calls complete() and release() on
	// its way out. Nothing here may touch the destroyed frame: the block is what the
	// children retire into, and it outlives every party by reference count.
	EXPECT_EQ(home.drain(), 3U) << "one teardown per child plus the chain's own";
	EXPECT_EQ(live_frames, 0);

	ASSERT_EQ(destruction_order.size(), 3U);
	EXPECT_EQ(destruction_order.front(), 0) << "the chain's frame was destroyed before either child's";

	// That the block itself was freed exactly once is not countable from here: the
	// erased destroy function is when_all_state's own private static, with no probe to
	// hand it. It is covered twice over elsewhere instead.
	// CombinatorStateTest.TheLastReleaseRunsTheErasedDeleterExactlyOnce counts the
	// calls directly, on a block driven by hand with a counting deleter, which is why
	// this test does not try to reproduce that count through a combinator. What is
	// specific to the drain above -- that the one release reaching zero is a child's,
	// after the chain's frame is already gone -- is covered by the sanitizers: a second
	// release reports a double free under AddressSanitizer, and a block nobody released
	// is reported by LeakSanitizer.
}

TEST_F(WhenAllTest, NoChildIsLeftRunningAfterTheChainIsCancelled)
{
	alt::async_event first;
	alt::async_event second;
	alt::async_event third;
	int              resumptions = 0;
	wait_record      record;

	auto handle = alt::start(three_child_root(first, second, third, resumptions, record));
	ASSERT_EQ(live_frames, 3) << "all three children are parked on their own event";
	ASSERT_EQ(resumptions, 0);

	handle.request_stop();

	// The spec's direct requirement. CancellingTheAwaitingChainRequestsEveryChildsStop
	// already shows that the frames are gone; what this adds is the other half of
	// "no child left running", which a frame count alone cannot show: each child's body
	// resumed exactly once, with a released wait, and nothing can resume it again.
	EXPECT_EQ(live_frames, 0) << "no child's frame is left";
	EXPECT_EQ(resumptions, 3) << "each child's body resumed exactly once, released";
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	const int after_the_stop = resumptions;

	first.set();
	second.set();
	third.set();

	EXPECT_EQ(resumptions, after_the_stop) << "setting the events resumed nothing: no child is parked on them";
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(record.resumptions, 1) << "and the chain was not resumed a second time";
}

TEST_F(WhenAllTest, AStopAfterTheWaitWasSatisfiedRunsOverABlockTheAwaiterHasLeft)
{
	alt::async_event first;
	alt::async_event second;
	alt::async_event after;
	wait_record      record;

	auto handle = alt::start(satisfied_then_parked_root(first, second, after, record));
	ASSERT_EQ(live_frames, 3);

	first.set();
	second.set();

	// The second child's hook drove the counter to zero, which resumed the chain; the
	// chain read its slots and dropped the awaiter's reference, and the same hook's
	// release then took the block's last one. So the block -- and the m_link stop
	// callback living inside it, registered on this chain's token -- is already freed,
	// while the chain itself is alive and parked on `after`.
	ASSERT_EQ(record.resumptions, 1);
	ASSERT_TRUE(record.satisfied);
	ASSERT_EQ(live_frames, 1) << "only the chain's own frame is left";

	// Review Focus 5. The stop walks this chain's token with the combinator's link long
	// gone. AddressSanitizer is the instrument: a link that outlived its block, or a
	// block whose last reference was the awaiter's rather than the last child's, reads
	// freed memory here.
	//
	// And "the instrument" is literal: this test is a sanitizer-only vehicle, measured
	// rather than assumed. Against the mutation it exists for -- ~when_all_awaiter
	// deleting the state instead of calling release(), so the block dies with the
	// awaiter rather than with its last reference -- it passes 200 out of 200 repeats in
	// a debug build and fails 200 out of 200 under AddressSanitizer, where the report is
	// heap-use-after-free in combinator_state::release() called from
	// on_combinator_child_complete, on the block this awaiter had already freed. A green
	// debug run of this test therefore says nothing at all; only the ASan preset does.
	handle.request_stop();

	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(record.resumptions, 1) << "the stop did not resume the chain a second time";
}

TEST_F(WhenAllTest, ANestedWhenAllInsideAChildIsItsOwnRoot)
{
	alt::async_event first;
	alt::async_event second;
	alt::async_event third;
	alt::async_event fourth;
	int              inner_cancelled = 0;
	wait_record      record;

	auto handle = alt::start(nested_root(first, second, third, fourth, inner_cancelled, record));
	ASSERT_EQ(live_frames, 6) << "two children, each parked in a when_all over two of its own";

	// Each child is its own cancellation root, so the outer stop reaches the inner
	// when_all only by being forwarded into the child's own stop source, and the inner
	// combinator links to the child's token exactly as the outer one links to the
	// chain's. Three blocks are in flight -- the outer one and one per child -- each
	// freed by its own last reference.
	//
	// The assertions below cannot tell that route apart from any other that reaches both
	// levels, so the test's name is earned by a mutation rather than by them: route the
	// children through the chain's token instead of the block's own stop source -- the
	// one line this test discriminates, see the DELICATE note on child_options.token in
	// when_all.hpp -- and this test fails 0 pass / 91 fail / 109 hang out of 200 repeats,
	// re-measured at 0 pass / 9 fail / 11 hang out of 20. It is the only test in the file
	// that catches it, and one level of nesting is why.
	//
	// "Every other test still passes" is how that used to be put here, and it was too
	// strong in one direction: a whole-suite run under the mutation prints 33 passes --
	// every test ahead of this one in the file -- and then dies in this one, so the test
	// after it never runs at all. Nothing else fails; one test is simply never reached.
	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(inner_cancelled, 2) << "both inner when_alls reported a cancelled wait, not a satisfied one";
	EXPECT_EQ(live_frames, 0) << "both levels are gone";
}

// The window ~when_all_awaiter's m_join.reset() closes, built on purpose rather than
// hoped for. Read that destructor's note first; this is its guard of record.
//
// READ THIS BEFORE TRUSTING A GREEN RUN. Its assertions below cannot fail on the
// mutation it exists for -- moving the reset below the release, which is the order this
// branch shipped with until the final review -- and neither sanitizer reports it either:
// measured 0 failures and 0 hangs in 20 repeats of this test alone in each of the debug,
// AddressSanitizer and ThreadSanitizer builds, and a full suite run of 763 passes in each
// of the two sanitizer presets with the mutation in place. That is not evidence the order
// does not matter, and the reason it is not is specific and worth knowing:
//
// On the mutated order the still-registered waiter_stop_release runs
// waiter_list::claim over the freed block, and claim touches the list through exactly
// one thing -- std::scoped_lock over the list's std::mutex, whose lock and unlock are
// glibc's pthread_mutex_lock and pthread_mutex_unlock. Neither sanitizer instruments
// glibc, so the write into the freed block is made by code they do not watch; and
// libstdc++'s std::mutex destructor is trivial, so no pthread_mutex_destroy is
// intercepted either. The rest of the callback's work is on the node, which lives in the
// chain's own frame and is still alive. So the bug is a write to freed heap memory with
// no instrumented access anywhere on its path.
//
// What was measured directly, by adding a timestamped print to
// waiter_stop_release::operator() and to when_all_state::destroy in a scratch build: on
// the mutated order the callback's claim is entered after the block's operator delete
// has returned, with the list pointer inside the freed block (list=0x...11f0 within
// block=0x...11c0) and AddressSanitizer reporting nothing. The window is real and this
// test reaches it; the instruments are blind to it.
//
// So what this test contributes is the window itself, deterministically: a
// cancellable_task root, every child retired before the join could link, and a
// concurrent request_stop() that is made to land inside the block's teardown by a
// handshake rather than by timing. No other test in the suite puts those three together.
// Keep it, and keep the mutation named here, because the next reader will otherwise find
// a green sanitizer run and conclude the reset is redundant.
TEST_F(WhenAllTest, AStopRacingTheEndOfAnAwaitWhoseJoinerNeverLinked)
{
	for(int repeat = 0; repeat < teardown_race_repeats; ++repeat)
	{
		wait_record record;
		int         tag_seen = 0;

		const int opened = run_one_teardown_race(record, tag_seen);

		check_the_teardown_repeat(repeat, record, tag_seen, opened);
	}
}

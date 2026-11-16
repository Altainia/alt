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
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
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

	/**
	 * Spins until @p flag is set or @p limit has elapsed, whichever comes first.
	 *
	 * For waits whose flag is published by a rendezvous that a future change could stop
	 * matching: an unbounded spin turns that into a hang, which CI reports as a timeout
	 * and not as a message. The clock is read only once the spin has fallen back to
	 * yielding, so the fast path is spin_until's own.
	 *
	 * @return true if the flag was set, false if the limit elapsed first.
	 */
	[[nodiscard]] bool spin_until_for(const std::atomic<bool>& flag, const std::chrono::steady_clock::duration limit)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;

		for(int spins = 0; !flag.load(std::memory_order_acquire);)
		{
			if(spins < spin_limit)
			{
				++spins;
			}
			else if(std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			else
			{
				std::this_thread::yield();
			}
		}

		return true;
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

		// No live_frames assertion: no coroutine on this path owns a frame_tracker, so
		// the counter reads 0 whatever the code does and the assertion could not fail.

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
	/** How many times each of the satisfier-against-stop phases repeats. */
	constexpr int stop_race_repeats = 200;

	/**
	 * How long either party of the satisfier handshake waits for the other.
	 *
	 * Bounded on both sides for the reason run_one_teardown_race's waits are: a change
	 * that stopped the satisfier from resuming the child would turn an unbounded spin
	 * into a hang, which CI reports as a timeout rather than as the message that says
	 * what broke.
	 */
	constexpr std::chrono::seconds satisfier_handshake_limit{5};

	/**
	 * @brief The two-party handshake that settles the raced node's election up front.
	 *
	 * Why it exists, measured rather than argued. Left to race, which party wins the
	 * child's waiter node is a property of the build and not of the library. The reason
	 * to expect the satisfier is that it reaches the node directly, while the stop first
	 * travels the chain's token, the block's m_link, cancel(), the children's stop source
	 * and the child's own stop_forwarder -- and in a debug build that is what happens:
	 * over 200 runs the satisfier won a mean of 199.6 of the 200 nodes and never fewer
	 * than 196. It is not what happens under instrumentation. Over 500 AddressSanitizer
	 * runs of the same loop the satisfier won a mean of 3.5, and
	 * 0 of 200 in 2 of those runs -- each of which failed this test's old lower-bound
	 * self-check over a window that was simply never hit. Nor is raising the repeat
	 * count or retrying the block a fix for that: instrumented into five consecutive
	 * 200-repeat blocks per process, the satisfier's wins under AddressSanitizer are
	 * concentrated in the FIRST repeat the process runs, which it took in 282 of 300
	 * runs, against a steady-state rate near 0.5% per repeat that leaves a later block
	 * scoring zero 54% to 64% of the time. Both of those options make the failure rarer
	 * and neither removes it.
	 *
	 * So the satisfier's win is arranged instead of hoped for. The awaiting thread does
	 * not request its stop until the child's own body reports that it has been resumed,
	 * and while no stop has been requested the only party that can have resumed it is
	 * the satisfier -- so the election is over, and the satisfier won it, before the
	 * stop exists. The child then waits to be released, which the awaiting thread does
	 * immediately before requesting that stop, so the child's drain into the shared
	 * block and the stop's walk of the chain are both in flight: this pins which party
	 * won the node, it does not serialize the teardown that follows. That much is
	 * arranged by the order of the two statements; that the two are still overlapping
	 * when the stop walks is timing, and no assertion witnesses it. The test's note on
	 * pinned_satisfied_waits says so rather than implying otherwise.
	 *
	 * It pins only that. Which party wins the chain's OWN joiner node, and so whether
	 * the chain's verdict is satisfied or released, is not arranged here at all. It is
	 * measured, though, and it does not change: the pinned phase reaches a released
	 * verdict on effectively every repeat, pinned_satisfied_waits coming out a mean of
	 * 0.008 of 200 over 2,500 AddressSanitizer runs and 0.09 in debug, which is the
	 * free-running phase's own rate. So the pinned phase is not a weaker sample of the
	 * window noted on ~when_all_awaiter. That window needs three things at once: a
	 * released joiner, a satisfier-won child node, and the awaiter's release being the
	 * block's LAST. Two of the three are observable from here, and the pinned phase has
	 * both on every repeat where the free-running phase has them about 2.8 times in 200
	 * under AddressSanitizer. Whether the third holds as often is not observable from a
	 * test, so what this buys that window is an expectation and not a measured count.
	 *
	 * THE SAME STRUCT PINS THE OTHER SIDE OF THE ELECTION, with the other pair of
	 * parties. On race_election::stop_pinned the awaiting thread requests its stop while
	 * the child is still parked and publishes `stopped` only once request_stop() has
	 * returned; the party held back is the SATISFIER, waiting at the rendezvous for that
	 * flag, and the child is handed no handshake at all, so `resumed` and `released` go
	 * unused there. No value exists for the child's node to be claimed with until the
	 * stop has already claimed it, so the stop wins every node by construction. That is
	 * the witness that the stop has any effect on the child at all, and it is an equality
	 * rather than a rate; the test's own notes say what it catches and what it does not.
	 *
	 * What the free-running phase is still here for, with both sides of the election now
	 * arranged, is the CONTENTION -- and that word is used here as carefully as "arranged
	 * but unwitnessed" is used below. It is the only one of the three phases where
	 * NEITHER party's claim is ordered behind the other's: the rendezvous releases the
	 * two threads within nanoseconds of each other and nothing after it serializes them,
	 * so both claims are in flight with no ordering between them. Whether they ever
	 * actually collide on a given repeat is timing, and no assertion here witnesses that
	 * either. Both pinned elections remove the possibility instead: the loser arrives at
	 * a node the winner has already taken off the list, so it loses the compare_exchange
	 * in waiter_list::try_claim having never contended for it, and a broken election
	 * could not resume the child twice on either of them -- on a satisfier-pinned repeat
	 * the stop's claim reads a node already @c claimed, and on a stop-pinned one the
	 * satisfier's detach_one walks a list the node has already left. The raced phase is
	 * where check_the_race_resolved_once's "exactly one party resumed the child" and its
	 * resumption count are claims about an unordered pair of claimers rather than about a
	 * sequence. That is its justification; the released joiner is not, and neither is the
	 * stop's side of the election any more.
	 */
	struct satisfier_handshake
	{
		/** The repeat this handshake belongs to, so either half's message names it. */
		int repeat = 0;

		/** Set by the child's body once some party has resumed it. */
		std::atomic<bool> resumed{false};

		/** Set by the awaiting thread immediately before it requests the stop. */
		std::atomic<bool> released{false};

		/**
		 * Set by the awaiting thread once request_stop() has returned, which is what lets
		 * the satisfier go on a stop-pinned repeat. Touched by nothing on the other two
		 * elections, whose repeats run through a different function entirely; see
		 * run_one_stop_pinned_race for why there are two.
		 */
		std::atomic<bool> stopped{false};
	};

	/** Whether a repeat leaves the node's election to the build or pins it, and for whom. */
	enum class race_election : std::uint8_t
	{
		/** Left to the build's timing: either party may win the child's node. */
		raced,

		/** Arranged by satisfier_handshake: the satisfier wins every node. */
		pinned,

		/** Arranged by satisfier_handshake inverted: the stop wins every node. */
		stop_pinned
	};

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

	/**
	 * A tracked task that parks on @p source and records which party resumed it.
	 *
	 * @p handshake is this body's half of satisfier_handshake, or null on a repeat that
	 * leaves the election to the build and on a stop-pinned one, whose handshake is
	 * between the awaiting thread and the satisfier instead. See satisfier_handshake.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): both references are owned by the calling test, which joins the racing thread and waits for live_frames to reach zero before leaving its scope.
	alt::task<std::expected<int, std::error_code>> tracked_source_wait(alt::task_completion_source<int>& source, child_record& child, int tag, satisfier_handshake* handshake)
	{
		const frame_tracker tracker{tag};
		auto                result = co_await source;

		child.resumed       = true;
		child.satisfier_won = result.has_value();

		if(result)
		{
			child.value = *result;
		}

		if(handshake != nullptr)
		{
			// Both halves sit after the co_await and before the co_return, which is what
			// makes them say what they say. After the co_await, so a resumption has
			// already happened and publishing it cannot influence which party achieved
			// it. Before the co_return, so this body is still short of the slot write and
			// the completion hook when the awaiting thread's stop starts travelling the
			// chain.
			handshake->resumed.store(true, std::memory_order_release);

			if(!spin_until_for(handshake->released, satisfier_handshake_limit))
			{
				ADD_FAILURE() << "repeat " << handshake->repeat << ": the awaiting thread never released the child, so this repeat's drain into the block did not overlap its stop";
			}
		}

		co_return result;
	}

	/**
	 * Parks in a when_all over a single child, the shape that lets a satisfier and a
	 * stop race for the same waiter node. @p values_seen counts the satisfied results
	 * whose slot held the value the satisfier published, and @p handshake is handed
	 * straight to the child.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): every reference is owned by the calling test, which joins the racing thread and waits for live_frames to reach zero before leaving their scope.
	alt::cancellable_task one_child_root(alt::task_completion_source<int>& source, child_record& child, wait_record& record, int& values_seen, satisfier_handshake* handshake)
	{
		const auto result = co_await alt::when_all(tracked_source_wait(source, child, 1, handshake));
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
	 * @brief Waits for the child to report its resumption, then releases it.
	 *
	 * The awaiting thread's half of satisfier_handshake, called by the repeat below
	 * immediately before that thread requests its stop.
	 *
	 * @param repeat    The repeat index, so a failure here names which one.
	 * @param handshake The handshake this repeat's child was handed.
	 */
	void pin_the_satisfiers_win(const int repeat, satisfier_handshake& handshake)
	{
		const bool resumed = spin_until_for(handshake.resumed, satisfier_handshake_limit);

		// Stored whether that wait succeeded or not, so a child that reports its
		// resumption just after the wait expired is never left spinning out its own
		// limit on a flag nobody would ever publish. Its wait is bounded too, so
		// neither half of this handshake can strand the other.
		//
		// Stored BEFORE the caller's request_stop() rather than after it, and that order
		// is the whole of what makes the two overlap: a release published after
		// request_stop() returned would hold the child until every callback on the
		// chain's token had run, which is the serialized shape teardown_tripwire wants
		// and the opposite of what this one does. It is not a deadlock that the order
		// avoids -- resume_claimed runs the child's body with no lock of this source
		// held, and the stop's own claim on an already-claimed node returns lost ahead
		// of any mutex -- and both waits are bounded anyway, so the cost of the wrong
		// order would be a quiet loss of the overlap rather than a hang. Quiet is the
		// word: nothing in this file would notice, pinned_satisfied_waits least of all,
		// since the serialized order drives that count AWAY from its bound rather than
		// toward it. The test's note on that bound says so, and says what witnessing the
		// overlap would actually take.
		handshake.released.store(true, std::memory_order_release);

		// Fatal rather than ADD_FAILURE, so the first broken repeat ends the phase. The
		// wait above is five seconds and the phase is stop_race_repeats repeats long,
		// so a non-fatal failure here would accumulate those bounds past this test's
		// TIMEOUT in tests/CMakeLists.txt and report as a killed test rather than as
		// this message. The caller runs each repeat under ASSERT_NO_FATAL_FAILURE, which
		// is what carries this out of the loop.
		ASSERT_TRUE(resumed) << "repeat " << repeat << ": the satisfier never resumed the child, so nothing pinned which party won its node";
	}

	/**
	 * @brief Runs one repeat of revision 1's path (a). Asserts nothing about the outcome.
	 *
	 * Split from the two check_the_race_* functions below, and all three split out of
	 * the test's loop, because one function holding the race and its assertions is over
	 * clang-tidy's readability-function-cognitive-complexity threshold -- which every
	 * other file in this suite satisfies, so splitting is the fix rather than a
	 * suppression in tests/.clang-tidy.
	 *
	 * @param repeat    The repeat index, so a precondition failure names which one.
	 * @param ran       Filled with what the child and the chain each recorded.
	 * @param handshake The handshake that pins the satisfier's win, or null to leave the
	 *                  election to the build's timing. See satisfier_handshake.
	 */
	void run_one_stop_race(const int repeat, race_record& ran, satisfier_handshake* handshake)
	{
		reset_coroutine_test_state();

		alt::task_completion_source<int> source;
		std::atomic<std::size_t>         arrived{0};

		auto handle = alt::start(one_child_root(source, ran.child, ran.parent, ran.values_seen, handshake));
		ASSERT_EQ(ran.parent.resumptions, 0) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		// The child is parked on one waiter node, and waiter_list::claim elects between
		// the satisfier below and the waiter_stop_release that the children's stop
		// fires. detail::waiter_stop_release resumes nothing unless claim returns
		// detached, so when the satisfier wins the node the stop callback is a no-op:
		// request_stop returns having released nothing, and the child carries on -- on
		// the satisfier's thread -- to write its slot and fire its completion hook into
		// a block the awaiting chain has usually abandoned already.
		//
		// Which party wins is the build's business on a null handshake and the
		// handshake's on any other, and the rendezvous below is what makes the null case
		// a contended race rather than a sequence either way.
		std::jthread satisfier{[&] {
			// DO NOT ADD A RUNTIME TEST BETWEEN THESE TWO STATEMENTS. This lambda is where
			// a merge of this function with run_one_stop_pinned_race would put the null
			// test that picks between the two, and the raced phase's window is a few
			// instructions wide: measured, that one test moves its AddressSanitizer wins
			// from a mean near 3 of 200 to 1.01, with 368 runs in 1,000 scoring zero
			// instead of 1 to 3. The figures and what a merge would have to show are on
			// run_one_stop_pinned_race.
			rendezvous(arrived, 2);
			source.set_value(1);
		}};

		rendezvous(arrived, 2);

		if(handshake != nullptr)
		{
			pin_the_satisfiers_win(repeat, *handshake);
		}

		handle.request_stop();

		satisfier.join();
	}

	/**
	 * @brief Runs one repeat with the election pinned to the STOP. Asserts nothing about
	 *        the outcome either.
	 *
	 * The inverted arrangement satisfier_handshake describes: the satisfier is held at
	 * the rendezvous until request_stop() has returned, so the stop walks a chain whose
	 * child is still parked and whose source holds no value, and the children's stop
	 * claims the child's node on every repeat.
	 *
	 * A FUNCTION OF ITS OWN rather than a third branch inside run_one_stop_race, and the
	 * reason is measured. Folding the two together needs one null-pointer test in the
	 * satisfier's lambda, between the rendezvous and the set_value, and the raced phase
	 * cannot afford it. Measured under AddressSanitizer, where that phase's wins are rare
	 * enough to count: kept apart, the raced phase's satisfier wins are a mean of 2.78 to
	 * 3.28 of 200 across four builds of this form, with 1 to 3 runs per 1,000 (500 on one
	 * of them) scoring zero, against 2.90 of 200 with 1 run in 1,000 scoring zero on the
	 * pre-change binary. Folded into one body, with that test
	 * in the lambda, they collapse to a mean of 1.01 with 368 runs in 1,000 scoring zero.
	 * The window that phase samples is a few instructions wide, so a branch the raced
	 * repeat does not need costs it two thirds of its wins.
	 *
	 * WHAT THAT MEASUREMENT PRICES IS THE RUNTIME BRANCH, and nothing more. It does not
	 * establish that the duplication below is necessary, only that this particular merge
	 * is not free. A form that decides the election at COMPILE time -- a runner templated
	 * on the election, with if constexpr around the spin, the post-join ASSERT_TRUE and
	 * the pin_the_satisfiers_win call -- would emit no test in the raced instantiation's
	 * lambda at all, and might well cost nothing; it was not attempted here because it
	 * moves the layout of both runners and would need the whole re-measurement this note
	 * asks for. Whoever attempts either form owes two things: the raced phase's
	 * AddressSanitizer distribution re-measured over at least 1,000 runs against the
	 * figures above, and run_the_stop_race_phase's cognitive-complexity score re-checked
	 * against the threshold run_one_repeat_of records.
	 *
	 * @param repeat    The repeat index, so a precondition failure names which one.
	 * @param ran       Filled with what the child and the chain each recorded.
	 * @param handshake The handshake whose `stopped` flag holds the satisfier back. Its
	 *                  other two flags go unused here: the child is handed nothing,
	 *                  because the party that must be held back is the one that would
	 *                  otherwise win the node, and here that is the satisfier.
	 */
	void run_one_stop_pinned_race(const int repeat, race_record& ran, satisfier_handshake& handshake)
	{
		reset_coroutine_test_state();

		alt::task_completion_source<int> source;
		std::atomic<std::size_t>         arrived{0};

		// Written by the satisfier and read below, after the join that orders the two.
		// Plain rather than atomic for that reason, and for the same reason it is a bool
		// the awaiting thread asserts on rather than a failure the satisfier reports: a
		// fatal assertion on that thread would end its lambda and nothing else, and a
		// non-fatal one would let every remaining repeat spend the same five seconds over
		// again -- 200 of them, past this test's TIMEOUT in tests/CMakeLists.txt, which
		// reports as a killed test rather than as the message that says what broke.
		bool stop_was_reported = false;

		auto handle = alt::start(one_child_root(source, ran.child, ran.parent, ran.values_seen, nullptr));
		ASSERT_EQ(ran.parent.resumptions, 0) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		std::jthread satisfier{[&] {
			rendezvous(arrived, 2);

			// Bounded on this side for the reason satisfier_handshake_limit records: a
			// change that stopped the awaiting thread from publishing its stop would turn
			// an unbounded spin into a hang. Reaching the limit is not itself a broken
			// stop, and the equality in the test body is what reports that: a stop that
			// never reached the child leaves it parked, the set_value below resumes it, and
			// that repeat counts as the satisfier's win, which is exactly what it is.
			stop_was_reported = spin_until_for(handshake.stopped, satisfier_handshake_limit);

			// A source nobody is parked on by now, the stop having taken the child's node:
			// detach_one finds an empty list, publish() fills the store, and nothing is
			// resumed. That is a set with no waiter and not a second set, so neither of
			// task_completion_source's two preconditions -- setting twice, awaiting twice --
			// is approached.
			source.set_value(1);
		}};

		rendezvous(arrived, 2);
		handle.request_stop();

		// After request_stop() has RETURNED, which is what leaves the child parked for the
		// stop to claim: the satisfier cannot reach the node until every callback on the
		// chain's token has run. That is the serialized order pin_the_satisfiers_win
		// exists to avoid, wanted here for the opposite reason -- it pins the election
		// rather than an overlap, and there is no overlap on this election to lose.
		//
		// What sits between the thread's construction and this store is request_stop()
		// itself, the whole walk under test, so this is not a statement nothing can
		// prevent being reached. What it cannot be prevented by is anything a bound in a
		// test could help with: a walk that deadlocks never reaches this store and hangs
		// whatever the assertion below does, and ctest's TIMEOUT is the only instrument
		// for that. What the assertion below buys is the OTHER failure -- a change that
		// stops this store from happening while the walk still returns -- which is the
		// mutation it was measured against.
		handshake.stopped.store(true, std::memory_order_release);

		satisfier.join();

		// Fatal, and after the join, so the first broken repeat ends the phase instead of
		// the phase accumulating the bound above -- 200 repeats of five seconds, which is
		// past this test's TIMEOUT in tests/CMakeLists.txt. The caller runs each repeat
		// under ASSERT_NO_FATAL_FAILURE, which is what carries this out of the loop.
		// Measured at the full repeat count, by deleting the store above: this test then
		// ends in 5.01 s with exit 1, ctest reporting Failed, where with this assertion
		// downgraded to EXPECT_TRUE the same breakage runs to ***Timeout 60.01 sec. What
		// it does not and cannot buy is a walk that never returns; see the store above.
		ASSERT_TRUE(stop_was_reported) << "repeat " << repeat << ": the awaiting thread never reported its stop, so nothing left the child parked for the stop to claim";
	}

	/**
	 * @brief Runs whichever of the two repeats @p election calls for.
	 *
	 * A dispatcher rather than a branch in either body, the two runners being kept apart
	 * for the reason run_one_stop_pinned_race records, and rather than a branch in the
	 * phase loop below, which cannot carry one: with these two calls inlined there, each
	 * under its own ASSERT_NO_FATAL_FAILURE, clang-tidy-19 reports
	 * run_the_stop_race_phase at a cognitive complexity of 54 against a threshold of 25.
	 * Measured, like the rest of this file's splits.
	 *
	 * A fatal failure inside either runner returns from it, then from here, and the
	 * loop's own ASSERT_NO_FATAL_FAILURE is what ends the phase.
	 */
	void run_one_repeat_of(const race_election election, const int repeat, race_record& ran, satisfier_handshake& handshake)
	{
		if(election == race_election::stop_pinned)
		{
			run_one_stop_pinned_race(repeat, ran, handshake);
			return;
		}

		run_one_stop_race(repeat, ran, election == race_election::pinned ? &handshake : nullptr);
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

	/** What one phase of the satisfier-against-stop repeats produced. */
	struct phase_tally
	{
		/** How many repeats the child reported as the satisfier's win. */
		int satisfier_wins = 0;

		/** How many repeats the awaiting chain reached a satisfied verdict on. */
		int satisfied_waits = 0;
	};

	/**
	 * @brief Runs stop_race_repeats repeats of one election and checks each of them.
	 *
	 * Split out of the test's loop for the reason run_one_stop_race records, and the
	 * tally travels in an out parameter rather than a return value because a
	 * GoogleTest ASSERT_* may only appear in a function returning void.
	 *
	 * @param election Which party each repeat's node is pinned to, if either.
	 * @param tally    Filled with what the repeats of this phase reported.
	 */
	void run_the_stop_race_phase(const race_election election, phase_tally& tally)
	{
		for(int repeat = 0; repeat < stop_race_repeats; ++repeat)
		{
			race_record         ran;
			satisfier_handshake handshake{.repeat = repeat};

			// Constructed on every repeat and handed to a different party by each election:
			// to the child on a satisfier-pinned repeat, to the satisfier on a stop-pinned
			// one, and to neither on a raced one, which costs it three atomic
			// initializations of flags no party of it reads and leaves it the repeat it was
			// before either pinned phase existed. Measured rather than assumed, twice: the
			// raced phase's distribution was unchanged by the pinned phase, a mean of 3.46
			// wins over 1,000 AddressSanitizer runs against 3.50 over the 500 measured
			// before it, and unchanged again by the stop-pinned one, a mean of 2.78 to 3.28
			// AddressSanitizer wins across four builds of this form against 2.90 on the
			// pre-change binary the same afternoon. What keeping it unchanged cost is
			// recorded on run_one_stop_pinned_race: folding these two calls into one body
			// moves that mean to 1.01, so there are two bodies rather than one branch.
			//
			// ASSERT_NO_FATAL_FAILURE, because an ASSERT_* inside either runner returns only
			// from that helper: a fired precondition would otherwise leave the chain it had
			// already started parked forever and run the remaining repeats anyway.
			ASSERT_NO_FATAL_FAILURE(run_one_repeat_of(election, repeat, ran, handshake));
			check_the_race_resolved_once(repeat, ran);
			check_the_race_reported_consistently(repeat, ran);

			tally.satisfier_wins += ran.child.satisfier_won ? 1 : 0;
			tally.satisfied_waits += ran.parent.satisfied ? 1 : 0;
		}
	}

	/** How many times the teardown-against-stop handshake repeats. */
	constexpr int teardown_race_repeats = 50;

	/** How long the stop thread waits for the tripwire before calling the window unopened. */
	constexpr std::chrono::seconds teardown_rendezvous_limit{5};

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
	 * released here reaches a waiter_list whose lifetime has ended -- if anything of this
	 * chain's is still registered to be invoked by it. In the shipped tree nothing is:
	 * detail::wait_awaiter::await_suspend deregisters the joiner's callback in its
	 * refused-link tail, and ~when_all_awaiter's reset ahead of the release would
	 * deregister it too. The window is what this tripwire opens; which of those two
	 * statements is keeping it empty is what the mutations named on
	 * AStopRacingTheEndOfAnAwaitWhoseJoinerNeverLinked separate. The same window a few
	 * hundred nanoseconds later, with the storage freed rather than merely destroyed, is
	 * reachable only by timing and no rendezvous a test can write lands there, because
	 * nothing of ours runs between that free and ~m_stop. That note says what this leaves
	 * the test able to claim, and agrees with this one.
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

			// Bounded for the same reason the stop thread's wait on `reached` is: that
			// thread gives up after teardown_rendezvous_limit, and if it gave up just
			// before this store nothing will ever publish `stopped`, so an unbounded
			// wait here would hang the suite in a narrower window than the one the stop
			// thread's bound closes. ADD_FAILURE is safe in a destructor: it records
			// the failure and returns, and throws only if the run was started with
			// --gtest_throw_on_failure, which this suite does not use. Each side's wait
			// is bounded on its own, so neither can strand the other.
			if(!spin_until_for(*m_stopped, teardown_rendezvous_limit))
			{
				ADD_FAILURE() << "the stop thread gave up or never ran, so the block's teardown was not held open and this repeat guarded nothing";
			}
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
	 * one child is ready, so it retires before the join can link, the link is refused,
	 * and the awaiter's release() is the block's LAST reference -- the three conditions
	 * that put a registered callback and a dead waiter_list in the same instant, in a
	 * tree whose refused-link tail does not deregister that callback. And the stop is
	 * requested from another thread, which is this library's documented shutdown idiom.
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

			// Bounded, because `reached` is published only by the tripwire whose hop
			// count equals tripwire_block_hops. If a change moves the slot's copy off
			// that count nothing publishes it, and an unbounded wait would hang the
			// suite instead of failing it. Giving up here strands nobody: the tripwire
			// returns early on a hop mismatch and never waits, and if it published
			// `reached` just after this wait expired, its own wait on `stopped` is
			// bounded too and fails the repeat instead of hanging.
			if(!spin_until_for(reached, teardown_rendezvous_limit))
			{
				ADD_FAILURE() << "the teardown tripwire never matched tripwire_block_hops, so the window was never opened and this repeat guarded nothing; "
				                 "the slot's copy of the child's result is no longer that many moves from the child's own";
				return;
			}

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
	phase_tally raced;
	phase_tally pinned;
	phase_tally stop_pinned;

	// The free-running race first, and first on purpose. What it is here for is the
	// CONTENTION, which neither pinned phase below has any of: it is the only phase in
	// which neither party's claim is ordered behind the other's, so it is the only one
	// where "exactly one party resumed the child" is a claim about two unordered claimers
	// rather than about a sequence. That they ever collide on a given repeat is timing and
	// is no more witnessed here than the pinned phase's overlap is; what is arranged is
	// that nothing serializes them. See satisfier_handshake.
	//
	// It is NOT here for either side of the election any more. The satisfier's side is
	// arranged by the pinned phase and the stop's by the stop-pinned phase, each on every
	// repeat; this phase reaches the stop's side on 0.13% of its repeats in debug -- a
	// mean of 199.73 satisfier wins of 200 over 1,000 runs -- which was its stated
	// justification until that third phase landed and is no longer worth anything on its
	// own. Nor is it here for the released chain verdict -- the pinned phase
	// supplies that at the same rate, measured, so crediting this phase with it would be
	// wrong. Running it ahead of both pinned phases keeps the mix it samples the mix it
	// sampled before either of them existed, which matters because the process's very
	// first repeat is measurably the satisfier's best one under AddressSanitizer. See
	// satisfier_handshake.
	ASSERT_NO_FATAL_FAILURE(run_the_stop_race_phase(race_election::raced, raced));

	// Then the same repeat with the election pinned. This is the phase that carries the
	// path the test exists for, entered on every repeat by construction instead of on
	// the repeats whose timing happens to favor the satisfier. Which is plausibly a
	// coverage gain and not only a stabilization: of the three conditions the window
	// noted on ~when_all_awaiter needs, this phase has the two a test can see -- a
	// released joiner over a satisfier-won child node -- on all 200 repeats, where the
	// free-running phase above has them about 2.8 times under AddressSanitizer. The
	// third, the awaiter's release being the block's last, is not observable from here,
	// so that is where the gain stops being measured. See satisfier_handshake.
	ASSERT_NO_FATAL_FAILURE(run_the_stop_race_phase(race_election::pinned, pinned));

	// And the same repeat with the election pinned the other way, which is where the
	// STOP's effect is asserted instead of sampled. The satisfier is held at the
	// rendezvous until request_stop() has returned, so the stop walks a chain whose child
	// is still parked and whose source holds no value: the children's stop claims the
	// child's node on every repeat, and the satisfier then sets a source nobody is parked
	// on, which is a set with no waiter rather than a second set -- detach_one finds an
	// empty list and the double-set assert in task_completion_source::publish is not
	// approached. Last of the three because the two phases above sample timing and this
	// one samples nothing. See satisfier_handshake.
	//
	// What it buys beyond the witness, and the reason it is worth 200 repeats rather than
	// one: it is the only repeat in this test where cancel() reaches a child that is still
	// PARKED. The other two phases hand cancel() a child the satisfier has already
	// resumed, so their stop callback claims nothing and the child retires on the
	// satisfier's thread; here the children's stop claims a live node, runs the child's
	// body, retires its frame and drops the block's LAST reference, all on the requesting
	// thread inside request_stop(). That is the ordinary cancellation path
	// combinator_state::cancel()'s counted copy of the children's stop source exists for,
	// and this phase now walks it 200 times per run. It is not a second guard for that
	// copy: TheChildrensStopOutlivesABlockTheLastChildFreed is, and it is built so the
	// release lands during an EARLIER callback of the children's source than the last,
	// which is where libstdc++ relocks the state the copy keeps alive. This phase has one
	// child, so that source carries one callback, the release lands during the last one,
	// and the relock is skipped. The path is walked here; the hazard on it is that test's.
	ASSERT_NO_FATAL_FAILURE(run_the_stop_race_phase(race_election::stop_pinned, stop_pinned));

	// The self-check, and an equality rather than a lower bound. The handshake makes
	// the satisfier the only party that can have resumed the child, so every pinned
	// repeat entering path (a) is a structural consequence and not a sampling result:
	// one repeat short of the full count means a child was resumed by something the
	// handshake rules out, or was not resumed at all, and either way the path this test
	// is about was not taken on it.
	//
	// What this replaced was EXPECT_GT over the raced phase's wins, which is the same
	// claim measured instead of arranged, and in this build it is not measurable: the
	// satisfier takes a mean of 3.5 of those 200 nodes under AddressSanitizer and took
	// none at all in 2 of 500 runs, so the lower bound failed about 1 run in 250 over a
	// window the code had not stopped reaching. Nothing is asserted about the raced
	// phase's mix for that reason; it is recorded below instead.
	EXPECT_EQ(pinned.satisfier_wins, stop_race_repeats) << "a pinned repeat's child was not resumed by its satisfier, so path (a) was not entered on every one of them";

	// A degenerate bound, and what it is a bound on is the stop still having an effect on
	// THIS phase's repeats. Reaching stop_race_repeats means every single repeat's chain
	// ended satisfied, which against a measured rate near zero means the stop stopped
	// winning the chain's joiner on any repeat at all: a request_stop() that became a
	// no-op, a stop path that no longer reaches the joiner, or a claim on it that always
	// loses. The stop's effect in general is asserted two assertions below, structurally
	// and per repeat, which is what this bound was the only check on until the stop-pinned
	// phase existed.
	//
	// It is NOT a witness for the overlap, which an earlier draft of this comment claimed
	// and had backwards. Serializing the release -- publishing it only once
	// request_stop() had returned -- holds the child BEHIND the stop, so the joiner is
	// still parked when the stop reaches it, the verdict is the released one, and this
	// count moves toward zero rather than toward the bound. The shipped order is the one
	// that can produce a satisfied verdict at all, because it gives the child a head
	// start of a few instructions. See the note below for what that leaves unwitnessed.
	//
	// The bound is at the full count and not at a rate for the reason the deleted
	// EXPECT_GT is a cautionary tale: measured over 4,300 runs, this count is a mean of
	// 0.008 of 200 under AddressSanitizer, 0.09 in debug and 0.12 under ThreadSanitizer,
	// the highest single run anywhere being 3. 200 is not a number a timing shift can
	// reach from there.
	EXPECT_LT(pinned.satisfied_waits, stop_race_repeats) << "every pinned repeat ended satisfied, so no repeat's stop won the chain's joiner and the stop is no longer having any effect here";

	// The witness that the stop had an effect, which is the hole the note below used to
	// record as open. Zero rather than a lower bound on the stop's wins, and an equality
	// for the same reason the pinned phase's is: on this election the satisfier cannot
	// reach the child's node until request_stop() has returned, so one win is one repeat
	// on which the stop did not claim the child's node, structurally and not as a
	// sampling shortfall. It says nothing about WHERE that stop stopped: a break in the
	// chain's token, in the block's m_link, in cancel(), in the children's stop source or
	// in the child's own stop_forwarder all read the same from here.
	//
	// Structural UP TO satisfier_handshake_limit, and that clause belongs in writing. The
	// satisfier is held by a bounded spin, so a request_stop() that took longer than five
	// seconds to return would release it mid-walk and let it win a node after all. That
	// is not a flake channel against a walk that takes microseconds, and it is the same
	// dependency pin_the_satisfiers_win already ships with, but it is a dependency: the
	// repeat's own ASSERT_TRUE fires first and names it, so a run that hits it reports the
	// expired handshake rather than this equality.
	//
	// What it catches that the two assertions above do not: a stop that stops reaching
	// the CHILDREN while still releasing the chain. Deleting the link(context->token)
	// line in when_all_awaiter::await_suspend is that mutation, and it takes this count
	// from 0 to 200 of 200 in the debug, AddressSanitizer, ThreadSanitizer and optimized
	// presets alike, 10 to 20 runs of each, while leaving every other assertion in this
	// test green: the joiner's own callback still releases the chain, so the bound above
	// stays satisfied, and the satisfier still wins every pinned child. Eight other tests
	// in the suite catch that mutation too, so what this buys is this test failing for
	// the right reason rather than the suite noticing at all.
	//
	// What it does NOT catch, and what does: a stop that reaches the child's node and
	// then resumes nothing leaves the child parked rather than satisfier-won, so this
	// count stays 0 and it is check_the_race_resolved_once's resumption count and
	// live_frames that report it. Measured twice, by dropping the resumption from
	// detail::waiter_stop_release::operator() and by treating its won claim as a lost one,
	// which are the same mutation: 20 runs in each of the four presets leave this count
	// at 0 and fire those three instead. That mutation is not a sharper instrument than
	// the one above, either -- waiter_stop_release is the only stop release every
	// wait_awaiter in the library uses, the chain's own joiner included, so it breaks
	// chain-level cancellation rather than isolating the child's election: 18 tests fail
	// ahead of this one -- across the four async primitives, the completion counter and
	// task_completion_source -- and the suite then aborts on task_group's own destructor
	// assert, never reaching this file.
	//
	// Neither does this phase say anything about the stop under CONTENTION, having none:
	// a stop that only lost a contended claim it used to win would show up, if anywhere,
	// in the raced phase's recorded mix.
	EXPECT_EQ(stop_pinned.satisfier_wins, 0) << "a stop-pinned repeat's child was resumed by its satisfier, so on that repeat the stop did not claim the child's node";

	// What no phase checks, recorded here because it is where the next reader will look.
	//
	// THE CHAIN'S VERDICT ON A STOP-PINNED REPEAT IS NOT ASSERTED, and the reason is that
	// no assertion over it could fail on its own. It is recorded, as
	// stop_pinned_satisfied_waits, and measured at 0 of 200 on every one of 3,800 runs,
	// but an EXPECT_EQ(stop_pinned.satisfied_waits, 0) is dominated by the equality above
	// in every state of the library. Enumerating when_all_awaiter::await_resume's
	// satisfied condition, waited && !cancelled(): a child the stop resumed makes
	// cancelled() true, so a satisfied verdict needs a child the SATISFIER resumed, which
	// is a win the equality above already counts; and a child nobody resumed is what
	// check_the_race_resolved_once reports. The one state left is a request_stop() that
	// does nothing whatever, and there the equality above reads 200. So the verdict could
	// only ever fail alongside something else, which is what this file means by an
	// assertion watching the wrong variable.
	//
	// The operative mechanism is also not the one an earlier draft of this note credited.
	// It is NOT combinator_state::cancel() storing its cancelled flag ahead of the
	// children's stop: m_link is registered in when_all_awaiter::await_suspend before the
	// join parks, the joiner's own release during it, and libstdc++ walks a token's
	// callbacks in reverse registration order, so the joiner is claimed and released
	// first, await_resume short-circuits on !waited, and the flag is never read.
	// combinator_state's own note says as much -- swapping cancel()'s two statements fails
	// no when_all test IN A DEBUG BUILD for exactly this reason. Two riders belong with
	// that, and both are stated once, in cancel()'s note: under AddressSanitizer the same
	// swap is a heap-use-after-free, which aborts
	// WhenAllTest.CancellingTheAwaitingChainReportsOperationCanceled, so the order is
	// load-bearing here as well and not only elsewhere; and the flag decides a VERDICT for
	// a combinator that reaches await_resume with the wait satisfied and the flag set,
	// which is when_any's business and which
	// WhenAnyTest.ASatisfiedWaitWithTheCancellationFlagSetDiscardsItsWinner holds.
	//
	// The overlap the satisfier-pinned phase is built around is ARRANGED BUT UNWITNESSED,
	// and the stop-pinned phase deliberately has no overlap to witness: it holds the
	// satisfier until request_stop() has returned, which is the serialized order
	// pin_the_satisfiers_win exists to avoid, chosen there because what it pins is the
	// election and not an overlap.
	//
	// The ordering itself is program order on one thread: pin_the_satisfiers_win stores
	// released and only then does the caller enter request_stop(), so the child is
	// certainly let go before the stop starts travelling the chain. Whether the child's
	// teardown is still IN FLIGHT while that stop walks is timing, and no assertion in
	// this file observes it. The handful of satisfied verdicts per thousand runs is not
	// that witness either: it says the two are close enough in time for their order to
	// vary, which overlap needs but does not follow from, a child that ran to completion
	// before the stop was entered producing the same verdict. Witnessing it means
	// instrumenting the library side -- ~when_all_awaiter observing the block's count
	// above one, or a counter of the two threads' contact with it -- which is out of this
	// test's scope. An honest "arranged but unwitnessed" is worth more here than an
	// assertion watching the wrong variable, which is what the bound above was first
	// written as.
	//
	// Recorded rather than asserted, so that a reader of a failure in any of the three
	// loops knows which mix of outcomes the failing run actually saw. The phases are kept
	// apart because they are three different samples: the raced phase's wins are the
	// build's timing, the other two are each handshake's doing, and summing them would
	// hide the one that moves. Only the raced phase's two numbers are a sample at all:
	// of the four below them, two are the asserted equalities, one is the degenerate
	// bound, and the last is the verdict the paragraph above says why nothing asserts.
	// All four are recorded so that a reader of a failure sees what the run actually
	// reported rather than only which equality it missed.
	//
	// No satisfied count is asserted on as a RATE, and no handshake arranges one: the
	// pinned phase's bound is a degenerate one over a count that is near zero by timing,
	// and the stop-pinned phase's is not asserted at all. Winning the child's node is not
	// winning the chain's, and an earlier draft of this test asserted on the chain's
	// verdict and measured 0 or 1 satisfied out of 200, which reads as "the window was
	// never hit" and is not what it means.
	RecordProperty("satisfier_wins", raced.satisfier_wins);
	RecordProperty("satisfied_waits", raced.satisfied_waits);
	RecordProperty("pinned_satisfier_wins", pinned.satisfier_wins);
	RecordProperty("pinned_satisfied_waits", pinned.satisfied_waits);
	RecordProperty("stop_pinned_satisfier_wins", stop_pinned.satisfier_wins);
	RecordProperty("stop_pinned_satisfied_waits", stop_pinned.satisfied_waits);
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

// The window a refused link used to leave open, built on purpose rather than hoped for.
// Read detail::wait_awaiter::await_suspend's refused-link tail and ~when_all_awaiter's
// destructor note before this one. Between them they say what this test is the guard of
// and what, as of this branch, it is no longer the guard of.
//
// WHAT IT GUARDS, measured rather than as first written. The hazard needs two things
// true at once: a stop callback still registered for a joiner whose link was refused,
// and the awaiter's release() freeing the block -- and with it the completion counter's
// waiter_list -- while that callback is registered. Two separate statements each falsify
// the first half on their own, so this test fails only when both are gone:
//
// - await_suspend's m_stop.reset() in the refused-link tail. Remove it alone and this
//   test still passes 20 of 20 in each of debug, AddressSanitizer and ThreadSanitizer.
// - ~when_all_awaiter's m_join.reset() ahead of the release. Move it under the release
//   alone and this test still passes 20 of 20 in all three, and so does the whole suite
//   over 20 whole-suite runs per preset.
// - Remove both and this test fails 20 of 20 in all three, every failure an abort on
//   waiter_list's liveness assert, "claiming an alt waiter on a destroyed list", exit
//   134.
//
// So this test guards the conjunction and neither statement separately. It was cited in
// ~when_all_awaiter as the guard of that destructor's ordering, which was true of the
// tree it was written against and is not true now: await_suspend's reset satisfies the
// conjunction by itself, and nothing in the suite fails when the destructor's order is
// swapped. That destructor's note carries the argument for keeping the order anyway, and
// it is an argument, not a measurement.
//
// READ THIS BEFORE TRUSTING A GREEN RUN. This test's own EXPECT assertions cannot fail
// on either mutation and cannot be made to; what reports the fault is the abort above,
// which takes the whole binary with it. Before the sentinel existed, the
// both-statements-gone mutation produced 0 failures and 0 hangs in 20 repeats of this
// test in each of the debug, AddressSanitizer and ThreadSanitizer builds, and a full
// suite run of 763 passes -- the suite's size at the time of that measurement -- in each
// of the two sanitizer presets. Those zeros are what the sentinel was added for. They
// are not a statement about this tree, where the same mutation aborts every time.
//
// What this test's handshake reaches. when_all_state declares m_slots and then m_shared,
// so ~combinator_state -- and with it the completion counter and its waiter_list -- runs
// BEFORE the slots are destroyed, and the operator delete that frees the storage runs
// after both. The tripwire lives in a result slot (see teardown_tripwire, which says the
// same), so the handshake lands the concurrent request_stop() after the waiter_list's
// lifetime has ended and BEFORE its storage is freed. In the shipped tree nothing is
// registered to arrive there at all -- that is what await_suspend's reset accomplishes,
// and the block's own m_link is deregistered by ~combinator_state earlier in the same
// teardown -- so the stop finds no callback of this chain's to invoke. Under the
// both-statements-gone mutation the joiner's callback is what arrives, and it claims a
// list whose lifetime has ended, which is where the sentinel's abort comes from.
//
// What the handshake does NOT reach, in any configuration. The same window a few hundred
// nanoseconds later, with the storage freed rather than merely destroyed: no rendezvous a
// test can write lands there, because nothing of ours runs between the free and ~m_stop.
// It was reached once, by a different instrument than this test's handshake: a timestamped
// print in waiter_stop_release::operator() and in when_all_state::destroy, plus a
// library-side hold, in a scratch build. The trace showed the claim entered with the list
// pointer inside the freed block (list=0x...11f0 within block=0x...11c0) and
// AddressSanitizer reporting nothing. This test's runs are not that evidence and do not
// reproduce it.
//
// What a sanitizer can and cannot say about either window, since the first thing a reader
// reaches for is a green ASan run. A claim's contact with the list is now two things, not
// one: waiter_list::alive()'s four-byte load of the list's own storage, which is an
// ordinary instrumented access, and then std::scoped_lock over its std::mutex, which is
// glibc's pthread_mutex_lock and pthread_mutex_unlock. Neither sanitizer instruments
// glibc, the trivial std::mutex destructor means no pthread_mutex_destroy is intercepted,
// and ThreadSanitizer drops its synchronization variables on free -- so the LOCK is
// invisible, and in an NDEBUG build, where the assert and with it the load are gone, that
// is the whole of the contact and the hazard has no instrumented access anywhere on its
// path. With asserts compiled in, a genuinely FREED list is reported by AddressSanitizer
// at that load, and a merely DESTROYED one -- this test's window -- is caught by the
// sentinel instead, there being no memory error in it for any tool to find. waiter_list's
// class notes carry that partition; do not read either half of it without the other.
//
// So what this test contributes is the window it does build, deterministically: a
// cancellable_task root, every child retired before the join could link, and a concurrent
// request_stop() made to land inside the block's teardown by a handshake rather than by
// timing, 50 repeats per preset. No other test in the suite puts those together. Keep it,
// and keep both mutations named here, because a reader who finds a green run and only one
// of them will conclude that whichever statement they looked at is redundant.
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

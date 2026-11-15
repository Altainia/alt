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

		/** Set by the statement after the co_await, the point of decision 5. */
		bool body_ran_on = false;

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
		record.satisfied   = result.has_value();
		record.body_ran_on = true;
		record.resumed_on  = std::this_thread::get_id();

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

	/** Awaits a when_all from a promise whose cancellation context is null. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the record is owned by the calling test, which blocks in sync_wait until this completes.
	alt::task<void> contextless_probe(wait_record& record, std::tuple<int, int>& values)
	{
		const auto result = co_await alt::when_all(make_value_task(1), make_value_task(2));
		record_outcome(record, result);

		if(result)
		{
			values = {std::get<0>(*result), std::get<1>(*result)};
		}
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

TEST_F(WhenAllTest, WhenAllInAPromiseWithNoCancellationContextStillCompletes)
{
	wait_record          record;
	std::tuple<int, int> values{0, 0};

	// An alt::task driven by sync_wait relays cancellation but has no root above it, so
	// cancellation_context_pointer() is null: no token to link and no home scheduler.
	alt::sync_wait(contextless_probe(record, values));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(std::get<0>(values), 1);
	EXPECT_EQ(std::get<1>(values), 2);
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
	// statement after the co_await ran, exactly once.
	EXPECT_TRUE(record.body_ran_on);
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
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

	/** A sized range that counts how often it is iterated and sized. */
	struct counting_range
	{
		std::vector<move_counted> items;
		int*                      begins;
		int*                      sizes;

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
			++*sizes;
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
		int        sizes = 0;

		counting_range range{.items = {}, .begins = &begins, .sizes = &sizes};
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
	EXPECT_EQ(children[0].value, 7);
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

#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <stop_token>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// The concept describes context-free awaitables, so this is the check that the
// awaiter still accepts an erased std::coroutine_handle<> and therefore works in
// a coroutine that relays no cancellation context at all.
static_assert(alt::awaitable<alt::async_event&>);
static_assert(std::is_same_v<alt::await_result_t<alt::async_event&>, std::expected<void, std::error_code>>);

// An event is referred to by the awaiters inside suspended coroutine frames, so
// it can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<alt::async_event>);
static_assert(!std::is_move_constructible_v<alt::async_event>);

namespace
{
	/** How many times the two contended tests repeat their race. */
	constexpr int race_repeats = 100;

	/** The error every released wait reports. */
	const std::error_code released_error = std::make_error_code(std::errc::operation_canceled);

	/** What one waiter coroutine recorded about its wait, for the test to inspect. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the wait reported success. */
		bool satisfied = false;

		/** The error a released wait reported. */
		std::error_code error;

		/** Whether the body got past its suspension point after the wait. */
		bool reached_end = false;
	};

	/** Records one wait's outcome. Shared by every waiter coroutine below. */
	void record_outcome(wait_record& record, const std::expected<void, std::error_code>& result)
	{
		++record.resumptions;
		record.satisfied = result.has_value();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/** A cancellable waiter that records how its wait ended and then finishes. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task waiting_root(alt::async_event& event, wait_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		record_outcome(record, co_await event);
	}

	/**
	 * As above, but with a suspension point after the wait, so a chain that was
	 * released by a cancellation has somewhere to tear itself down.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::task<void> waiting_leaf(alt::async_event& event, wait_record& record)
	{
		const frame_tracker tracker{3};
		record_outcome(record, co_await event);
		co_await alt::cancellation_point{};
		record.reached_end = true;
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::task<void> waiting_middle(alt::async_event& event, wait_record& record)
	{
		const frame_tracker tracker{2};
		co_await waiting_leaf(event, record);
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::cancellable_task waiting_chain_root(alt::async_event& event, wait_record& record)
	{
		const frame_tracker tracker{1};
		co_await waiting_middle(event, record);
	}

	/**
	 * Starts a second waiter from the continuation of its own wait, which runs
	 * inside set(): after set() has taken the waiting queue and before it returns.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::cancellable_task starts_another_waiter(alt::async_event& event, wait_record& first, wait_record& second)
	{
		const frame_tracker tracker{1};
		record_outcome(first, co_await event);
		const auto handle = alt::start(waiting_root(event, second, 2));
	}

	/** Awaits the event a second time from the continuation of its first wait. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::cancellable_task twice_waiting_root(alt::async_event& event, wait_record& first, wait_record& second)
	{
		const frame_tracker tracker{1};
		record_outcome(first, co_await event);
		record_outcome(second, co_await event);
	}

	/** A plain task, which relays a null cancellation context rather than none. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	alt::task<void> plain_waiter(alt::async_event& event, wait_record& record)
	{
		const frame_tracker tracker{1};
		record_outcome(record, co_await event);
	}

	/**
	 * A waiter that rendezvouses immediately before its co_await, so that another
	 * thread's set() lands in the window between this waiter's own ready check and
	 * its link.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the event, the record and the counter are owned by the calling test, which joins both threads before leaving the scope that holds them.
	alt::cancellable_task racing_waiter(alt::async_event& event, wait_record& record, std::atomic<std::size_t>& arrived)
	{
		const frame_tracker tracker{1};
		rendezvous(arrived, 2);
		record_outcome(record, co_await event);
	}

	/**
	 * A coroutine that relays an alt cancellation context without the
	 * await_transform alt's own task types carry.
	 *
	 * This is the only way to drive one specific window from a test. In an
	 * alt::task or an alt::cancellable_task, detail::cancellation_checked observes
	 * an already-requested stop on the way into the suspension point and abandons
	 * the whole chain before the event's awaiter is ever reached. A stop landing
	 * between that check and the awaiter's own registration still has to be caught,
	 * and this promise reaches the awaiter with the stop already requested, which is
	 * that window made deterministic.
	 */
	struct relaying_coroutine
	{
		struct promise_type
		{
			/** Installed by the test before the body runs. */
			alt::detail::cancellation_context* context = nullptr;

			relaying_coroutine get_return_object() noexcept
			{
				return relaying_coroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
			}

			std::suspend_always initial_suspend() noexcept
			{
				return {};
			}

			// Self-destroying, so the frame is gone once the body finishes and
			// live_frames is a usable check.
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

			[[nodiscard]] alt::detail::cancellation_context* cancellation_context_pointer() const noexcept
			{
				return context;
			}
		};

		std::coroutine_handle<promise_type> handle;
	};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as waiting_root above.
	relaying_coroutine relaying_waiter(alt::async_event& event, wait_record& record)
	{
		const frame_tracker tracker{1};
		record_outcome(record, co_await event);
	}

	/** Sets an event from a scheduler's queue. */
	void set_the_event(void* state) noexcept
	{
		static_cast<alt::async_event*>(state)->set();
	}
}

class AsyncEventTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(AsyncEventTest, AwaitingASetEventIsReadyWithoutSuspending)
{
	alt::async_event event{true};
	ASSERT_TRUE(event.is_set());

	// Taken through the awaiter directly, because never suspending is a property of
	// await_ready rather than of anything observable after the fact.
	auto awaiter = alt::get_awaiter(event);
	EXPECT_TRUE(awaiter.await_ready());
	EXPECT_TRUE(awaiter.await_resume().has_value());

	// And end to end: nothing resumes this waiter, so it can only have run through.
	wait_record record;
	const auto  handle = alt::start(waiting_root(event, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, AwaitingAnUnsetEventSuspendsUntilSet)
{
	alt::async_event event;
	wait_record      record;

	const auto handle = alt::start(waiting_root(event, record, 1));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1); // parked, with its frame alive

	event.set();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, SetReleasesEveryWaiter)
{
	alt::async_event event;
	wait_record      first;
	wait_record      second;
	wait_record      third;

	const auto first_handle  = alt::start(waiting_root(event, first, 1));
	const auto second_handle = alt::start(waiting_root(event, second, 2));
	const auto third_handle  = alt::start(waiting_root(event, third, 3));
	ASSERT_EQ(live_frames, 3);

	event.set();

	// One set releases all three, not just the head of the list.
	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_EQ(third.resumptions, 1);
	EXPECT_TRUE(first.satisfied);
	EXPECT_TRUE(second.satisfied);
	EXPECT_TRUE(third.satisfied);
	EXPECT_EQ(live_frames, 0);

	// Link order is resume order, so the oldest waiter ran first.
	EXPECT_EQ(destruction_order, (std::vector<int>{1, 2, 3}));
}

TEST_F(AsyncEventTest, SetLeavesTheEventSetSoALaterAwaitIsReady)
{
	alt::async_event event;
	event.set();
	ASSERT_TRUE(event.is_set());

	// An event is sticky: a waiter arriving after the set does not wait at all.
	wait_record record;
	const auto  handle = alt::start(waiting_root(event, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, ResetClearsIt)
{
	alt::async_event event{true};
	ASSERT_TRUE(event.is_set());

	event.reset();
	EXPECT_FALSE(event.is_set());

	// And it is a real reset rather than a flag nothing reads: the next waiter parks.
	wait_record record;
	const auto  handle = alt::start(waiting_root(event, record, 1));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(live_frames, 1);

	event.set();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
}

TEST_F(AsyncEventTest, ASatisfiedWaitReportsSuccess)
{
	alt::async_event event;
	wait_record      record;

	const auto handle = alt::start(waiting_root(event, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	event.set();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(record.error, std::error_code{});
}

TEST_F(AsyncEventTest, CancellingAParkedWaiterReleasesItWithOperationCanceled)
{
	// Parked is the state this component exists to be cancelled in, and the stop
	// callback is the only thing that can reach a waiter there: nothing in the
	// coroutine machinery runs again until someone resumes the frame. Without the
	// registration this waiter stays parked and its frame is never freed.
	alt::async_event event;
	wait_record      record;

	auto handle = alt::start(waiting_root(event, record, 1));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	// Released rather than satisfied: the thing it was waiting for never happened.
	EXPECT_FALSE(event.is_set());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, ACancelledWaiterTearsDownTheWholeChain)
{
	// The released wait reports to its own frame, which then reaches its next
	// suspension point and abandons the chain from there. Nothing about the
	// release short-circuits that, which is what the destruction order proves.
	alt::async_event event;
	wait_record      record;

	auto handle = alt::start(waiting_chain_root(event, record));
	ASSERT_EQ(live_frames, 3); // root, middle, leaf

	handle.request_stop();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_EQ(record.error, released_error);
	EXPECT_FALSE(record.reached_end);
	EXPECT_EQ(live_frames, 0);

	// Root frame destruction reaches the leaf through the owning task destructors,
	// so leaf locals are destroyed before their parents'. A teardown that only
	// destroyed the leaf would leave this {3} alone.
	EXPECT_EQ(destruction_order, (std::vector<int>{3, 2, 1}));
}

TEST_F(AsyncEventTest, ACancelledWaitersTeardownGoesToItsHomeScheduler)
{
	// The release runs on whichever thread observed the stop, and a home scheduler
	// is how a caller keeps the frames from being destroyed there. Exactly one
	// teardown is posted, which is what the drain count pins.
	alt::manual_scheduler home;
	alt::async_event      event;
	wait_record           record;

	auto handle = alt::start(waiting_chain_root(event, record), {.home = alt::scheduler_ref{home}});
	ASSERT_EQ(live_frames, 3);

	handle.request_stop();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_EQ(live_frames, 3); // posted, not destroyed
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_EQ(live_frames, 0);
	EXPECT_FALSE(record.reached_end);
}

TEST_F(AsyncEventTest, AStopAndASetDoNotBothResume)
{
	// End to end, the property the claim election exists for: a satisfier and a
	// cancellation reaching for the same parked waiter must not both resume it.
	// Twice would run a frame the first resumption already destroyed, and never
	// would leave the coroutine suspended forever.
	//
	// The rendezvous is what makes this contended rather than sequential: both
	// threads block on the same counter and the second arrival releases both, so
	// set()'s detach of the node and the stop callback's claim of it reach the same
	// state word within nanoseconds. Starting one thread after the other, or
	// sleeping, hands the race to whichever side ran first and the window never
	// opens.
	//
	// Measured rather than argued. With the stop callback made to resume whatever it
	// wins, rather than only what it actually unlinked off the list, this test failed
	// in 20 of 20 runs of itself: 18 by segfault, 2 by hanging on a frame that had
	// already been destroyed. That is one machine, one compiler, one sample, and a
	// detector built on a race is never certain whatever a sample shows. The standing
	// guards are the exactly-once resumption count and live_frames, which hold
	// whatever the interleaving turns out to be, plus ThreadSanitizer running all of
	// it.
	//
	// Both sides really do win, which is the other thing worth measuring, because a
	// race only one party ever wins exercises half of what it claims to. Counting the
	// winner over three runs of the 100 repeats below gave 15, 41 and 13 sets against
	// 85, 59 and 87 stops.
	//
	// reset_coroutine_test_state() is called per repeat because this loop asserts
	// live_frames: the fixture zeroes it once in SetUp, so without the reset every
	// repeat after the first would be reading the sum of the ones before it.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_event         event;
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		auto handle = alt::start(waiting_root(event, record, 1));
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		{
			std::jthread setter{[&] {
				rendezvous(arrived, 2);
				event.set();
			}};

			std::jthread stopper{[&handle, &arrived] {
				rendezvous(arrived, 2);
				handle.request_stop();
			}};
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncEventTest, AWaiterArrivingFromInsideSetDoesNotPark)
{
	// Why set() stores its flag before it walks the queue rather than after. The
	// first waiter's continuation runs from inside set(), which is the one moment a
	// waiter can arrive after the walk is over and before set() has returned; the
	// flag is the only thing that can tell it the event is already set. This is the
	// same window two threads race in AConcurrentSetIsNeverMissedByAnArrivingWaiter
	// below, reached deterministically instead.
	alt::async_event event;
	wait_record      first;
	wait_record      second;

	const auto handle = alt::start(starts_another_waiter(event, first, second));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(first.resumptions, 0);

	event.set();

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_TRUE(first.satisfied);

	// Nothing will ever walk the queue again, so a second waiter that parked here
	// would wait forever.
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, AnEventSetBetweenTheReadyCheckAndTheLinkDoesNotPark)
{
	// The same window as the contended test below, made deterministic by driving
	// the awaiter by hand: its ready check is asked first, the event is set second,
	// and only then does it try to park. Linking there would suspend a waiter on an
	// event that is already set, with the set that would have released it long past.
	alt::async_event event;

	auto awaiter = alt::get_awaiter(event);
	ASSERT_FALSE(awaiter.await_ready());

	event.set();

	// False is the awaiter's only way to say the wait is over, and it must report
	// that wait as satisfied rather than released: nothing cancelled it.
	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));
	EXPECT_TRUE(awaiter.await_resume().has_value());
}

TEST_F(AsyncEventTest, AConcurrentSetIsNeverMissedByAnArrivingWaiter)
{
	// The other side of the same window: a waiter that checks the event, finds it
	// unset, and links. A set() landing between those two steps has already walked
	// the list and found nothing, so unless the link itself reconsiders the event's
	// state the waiter parks forever on an event that is already set. Nothing would
	// report that: the wait simply never ends.
	//
	// The rendezvous sits inside the waiter's own body, immediately before the
	// co_await, so the two threads reach the check-then-link and the
	// set-then-detach together. The event is deliberately inspected before it goes
	// out of scope, because its destructor would release a lost waiter and hide the
	// defect.
	//
	// This is the weakest detector in the file, and knowing that is the point of
	// having measured it: with the link made unconditional, so that it ignores the
	// event's state entirely, it failed in only 1 of 20 runs of itself. The two tests
	// above it catch that same defect every time, by reaching the window
	// deterministically instead, and they are the guards. What this one adds is
	// ThreadSanitizer over a set() that really is concurrent with a waiter arriving,
	// which no deterministic test can give.
	//
	// reset_coroutine_test_state() is called per repeat for the reason the test above
	// gives: this loop asserts live_frames, which SetUp zeroes only once.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_event         event;
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		{
			std::jthread waiter{[&] {
				const auto handle = alt::start(racing_waiter(event, record, arrived));
			}};

			rendezvous(arrived, 2);
			event.set();
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_TRUE(record.satisfied) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncEventTest, DestroyingTheEventReleasesParkedWaiters)
{
	// A waiter nobody resumes stays suspended forever and its frame is never freed,
	// so releasing them beats leaking them even though the thing they waited for
	// never happened.
	wait_record first;
	wait_record second;

	{
		alt::async_event event;
		const auto       first_handle  = alt::start(waiting_root(event, first, 1));
		const auto       second_handle = alt::start(waiting_root(event, second, 2));
		ASSERT_EQ(live_frames, 2);
		ASSERT_EQ(first.resumptions, 0);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_EQ(first.error, released_error);
	EXPECT_EQ(second.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, AWaiterThatReAwaitsWhileTheEventIsBeingDestroyedIsStillReleased)
{
	// Why the event releases its waiters itself instead of leaving it to the
	// waiter_list member's own destructor, which does the same thing one step later.
	// A released waiter's continuation may reach for this event again, and the event
	// still exists while its destructor body runs: that re-await parks in a list that
	// is still whole, and the member's destructor then releases it. Leave the release
	// to that member instead and the re-await lands in a list whose walk is already
	// finished, parking the waiter on an event that will never release anything again.
	//
	// What this pins is that defensive behavior, not a promise about it. The
	// documentation says a waiter released by the destructor must not await the event
	// again -- unsupported on all four primitives -- so do not delete this test on the
	// strength of that sentence. It guards the explicit release, which is what keeps
	// the unsupported case from leaking rather than something a caller may rely on.
	wait_record first;
	wait_record second;

	{
		alt::async_event event;
		const auto       handle = alt::start(twice_waiting_root(event, first, second));
		ASSERT_EQ(live_frames, 1);
		ASSERT_EQ(first.resumptions, 0);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, AStopRequestedBeforeTheAwaitStillReleasesTheWaiter)
{
	// The window the registration order closes. The awaiter registers its stop
	// callback before it links, and std::stop_callback invokes its callback
	// immediately when the token has already been stopped, so the callback runs
	// here with the node on no list. It must therefore resume nothing: this frame
	// is inside its own await_suspend. What it does instead is win the node, which
	// makes the link refuse, and the frame resumes itself with the release.
	std::stop_source source;
	source.request_stop();

	alt::async_event event;
	wait_record      record;

	const auto coroutine = relaying_waiter(event, record);
	ASSERT_EQ(live_frames, 0); // lazy: the body has not started

	alt::detail::cancellation_context context{.root = coroutine.handle, .token = source.get_token(), .home = {}};
	coroutine.handle.promise().context = &context;
	coroutine.handle.resume();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_FALSE(event.is_set());

	// Never parked, so nothing is left in the event to release, and the frame is
	// gone because the body ran to its end rather than suspending.
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncEventTest, AwaitingFromACoroutineWithNoCancellationContextStillWorks)
{
	// A plain alt::task relays a null context, and alt::sync_wait's own root
	// coroutine relays none at all, so the awaiter's stop-callback branch is skipped
	// on both counts. The wait then has exactly one way to end, which is the event.
	alt::async_event      event;
	alt::manual_scheduler scheduler;
	wait_record           record;

	// Posted rather than set up front, so the wait really does park and the
	// pumping thread is what releases it.
	scheduler.post(alt::work_item{.invoke = &set_the_event, .state = &event});
	alt::sync_wait(plain_waiter(event, record), scheduler);

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_TRUE(event.is_set());
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(scheduler.drain(), 0U); // the set was the only item, and it ran
}

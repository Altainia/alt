#include <gtest/gtest.h>

#include <alt/async_semaphore.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <coroutine>
#include <cstddef>
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
static_assert(alt::awaitable<alt::async_semaphore&>);
static_assert(std::is_same_v<alt::await_result_t<alt::async_semaphore&>, std::expected<void, std::error_code>>);

// A semaphore is referred to by the awaiters inside suspended coroutine frames, so
// it can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<alt::async_semaphore>);
static_assert(!std::is_move_constructible_v<alt::async_semaphore>);

namespace
{
	/** How many times the contended tests repeat their race. */
	constexpr int race_repeats = 100;

	/** The error every released wait reports. */
	const std::error_code released_error = std::make_error_code(std::errc::operation_canceled);

	/** What one waiter coroutine recorded about its wait, for the test to inspect. */
	struct wait_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the wait reported success, which for a semaphore means a permit. */
		bool satisfied = false;

		/** The error a released wait reported. */
		std::error_code error;
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

	/** A cancellable acquirer that records how its wait ended and then finishes. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the semaphore and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task acquiring_root(alt::async_semaphore& semaphore, wait_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		record_outcome(record, co_await semaphore);
	}

	/** Acquires twice in a row, the second time from the continuation of the first wait. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as acquiring_root above.
	alt::cancellable_task twice_acquiring_root(alt::async_semaphore& semaphore, wait_record& first, wait_record& second)
	{
		const frame_tracker tracker{1};
		record_outcome(first, co_await semaphore);
		record_outcome(second, co_await semaphore);
	}

	/** Parks on something else first, so a stop can be requested before it ever reaches the semaphore. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as acquiring_root above.
	alt::cancellable_task parks_then_acquires(alt::async_semaphore& semaphore, wait_record& record)
	{
		const frame_tracker tracker{1};
		co_await park{};
		record_outcome(record, co_await semaphore);
	}

	/**
	 * A waiter that rendezvouses immediately before its co_await, so that another
	 * thread's release lands in the window between this waiter's own ready check and
	 * its link.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the semaphore, the record and the counter are owned by the calling test, which joins both threads before leaving the scope that holds them.
	alt::cancellable_task racing_acquirer(alt::async_semaphore& semaphore, wait_record& record, std::atomic<std::size_t>& arrived)
	{
		const frame_tracker tracker{1};
		rendezvous(arrived, 2);
		record_outcome(record, co_await semaphore);
	}

	/**
	 * An acquirer that rendezvouses immediately before its co_await and records into
	 * atomics, for the one test whose waiters are resumed on several threads at once.
	 * frame_tracker and wait_record are both unsynchronized, so a waiter resumed
	 * off-thread must not touch them. The rendezvous is inside the coroutine so that
	 * the frame is already built when it returns and the next thing this thread does
	 * is the acquisition itself.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the semaphore, both counters and the rendezvous are owned by the calling test, which joins every thread before leaving the scope that holds them.
	alt::cancellable_task racing_counting_acquirer(alt::async_semaphore& semaphore, std::atomic<int>& satisfied, std::atomic<int>& released, std::atomic<std::size_t>& arrived, std::size_t parties)
	{
		rendezvous(arrived, parties);

		if(co_await semaphore)
		{
			satisfied.fetch_add(1, std::memory_order_acq_rel);
		}
		else
		{
			released.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	/**
	 * @brief A consuming predicate over a plain int, for driving detail::wait_awaiter
	 *        itself.
	 *
	 * Stands in for async_semaphore in the two machinery tests below. It consumes
	 * exactly as try_acquire() does, and it can do from inside the predicate what a
	 * winning stop callback would do to the node -- which is the one instant in a wait
	 * that nothing outside the awaiter can reach, because it happens under the list's
	 * mutex.
	 *
	 * Deriving from wait_awaiter is what gives it access to the node: the base makes
	 * Derived a friend, so the probe can reach the node its own base holds. A test
	 * cannot otherwise, and the semaphore's own queue is private.
	 */
	/**
	 * Does roughly the bookkeeping an arriving waiter does between its ready check and
	 * its link -- one std::stop_callback registered and destroyed -- so that a stop
	 * requested after it lands near the moment that waiter's link condition runs
	 * instead of well before the waiter has looked at the primitive at all. Both sides
	 * scale with the same work, so this is not a calibration against a wall clock.
	 */
	inline void match_an_arriving_waiters_bookkeeping(const std::stop_token& token)
	{
		const std::stop_callback callback{token, [] {}};
	}

	class probe_awaiter: public alt::detail::wait_awaiter<probe_awaiter>
	{
	public:
		probe_awaiter(alt::detail::waiter_list& list, int& permits) noexcept: m_list(&list), m_permits(&permits)
		{}

		/**
		 * Stores what a winning stop callback's compare-exchange would store. The
		 * callback's claim is the only part of it that runs without the list's mutex, so
		 * this is the whole of what that party does to the node before it reaches the
		 * mutex and finds there is nobody to resume.
		 */
		void claim_the_node() noexcept
		{
			m_node.state.store(alt::detail::waiter_state::claimed, std::memory_order_release);
		}

		/**
		 * Arranges for the same claim to happen from inside the predicate, so that it
		 * lands after the condition has consumed and before the link returns. Calling
		 * waiter_list::claim there would self-deadlock on the mutex link() is holding,
		 * which is why this writes the state word directly.
		 */
		void claim_the_node_while_consuming() noexcept
		{
			m_claim_while_consuming = &m_node;
		}

		/** Consuming, as a semaphore's is. */
		[[nodiscard]] bool ready() const noexcept
		{
			if(*m_permits <= 0)
			{
				return false;
			}

			--*m_permits;

			if(m_claim_while_consuming != nullptr)
			{
				m_claim_while_consuming->state.store(alt::detail::waiter_state::claimed, std::memory_order_release);
			}

			return true;
		}

		[[nodiscard]] alt::detail::waiter_list& list() const noexcept
		{
			return *m_list;
		}

	private:
		alt::detail::waiter_list* m_list;
		int*                      m_permits;
		alt::detail::waiter_node* m_claim_while_consuming = nullptr;
	};
}

class AsyncSemaphoreTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(AsyncSemaphoreTest, AcquiringAnAvailablePermitIsReady)
{
	alt::async_semaphore semaphore{1};

	// Taken through the awaiter directly, because never suspending is a property of
	// await_ready rather than of anything observable after the fact.
	auto awaiter = alt::get_awaiter(semaphore);
	EXPECT_TRUE(awaiter.await_ready());
	EXPECT_TRUE(awaiter.await_resume().has_value());

	// The ready check is what takes the permit, so the count is already down.
	EXPECT_EQ(semaphore.available(), 0);

	// And end to end: nothing resumes this waiter, so it can only have run through.
	alt::async_semaphore second{1};
	wait_record          record;
	const auto           handle = alt::start(acquiring_root(second, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(second.available(), 0);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, AcquiringWithNoPermitsSuspends)
{
	alt::async_semaphore semaphore{0};
	wait_record          record;

	const auto handle = alt::start(acquiring_root(semaphore, record, 1));

	// Parked: the frame is alive and the statement after the co_await has not run.
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_EQ(live_frames, 1);

	semaphore.release();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(live_frames, 0);

	// The permit was handed to the waiter rather than parked in the count.
	EXPECT_EQ(semaphore.available(), 0);
}

TEST_F(AsyncSemaphoreTest, ReleaseResumesOneWaiterPerPermit)
{
	// The test a release that wakes every waiter fails: two permits must reach two
	// waiters and leave the third exactly where it was.
	alt::async_semaphore semaphore{0};
	wait_record          first;
	wait_record          second;
	wait_record          third;

	const auto first_handle  = alt::start(acquiring_root(semaphore, first, 1));
	const auto second_handle = alt::start(acquiring_root(semaphore, second, 2));
	const auto third_handle  = alt::start(acquiring_root(semaphore, third, 3));
	ASSERT_EQ(live_frames, 3);

	semaphore.release(2);

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(first.satisfied);
	EXPECT_TRUE(second.satisfied);

	// The third is still parked, and no permit is sitting in the count either: both
	// went to waiters.
	EXPECT_EQ(third.resumptions, 0);
	EXPECT_EQ(live_frames, 1);
	EXPECT_EQ(semaphore.available(), 0);

	// Which is a real park rather than a dead waiter: one more permit reaches it.
	semaphore.release();
	EXPECT_EQ(third.resumptions, 1);
	EXPECT_TRUE(third.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, ReleaseResumesInFifoOrder)
{
	alt::async_semaphore semaphore{0};
	wait_record          first;
	wait_record          second;
	wait_record          third;

	const auto first_handle  = alt::start(acquiring_root(semaphore, first, 1));
	const auto second_handle = alt::start(acquiring_root(semaphore, second, 2));
	const auto third_handle  = alt::start(acquiring_root(semaphore, third, 3));
	ASSERT_EQ(live_frames, 3);

	semaphore.release(3);

	// Link order is resume order, so the oldest waiter got the first permit.
	EXPECT_EQ(destruction_order, (std::vector<int>{1, 2, 3}));
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, TryAcquireConsumesAPermit)
{
	alt::async_semaphore semaphore{2};

	EXPECT_TRUE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 1);

	EXPECT_TRUE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 0);

	// And it stops at zero rather than lending against the next release.
	EXPECT_FALSE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 0);

	semaphore.release();
	EXPECT_TRUE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 0);
}

TEST_F(AsyncSemaphoreTest, TryAcquireFailsWithNone)
{
	alt::async_semaphore semaphore{0};

	EXPECT_FALSE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 0);

	// Repeatedly, and without driving the count negative.
	EXPECT_FALSE(semaphore.try_acquire());
	EXPECT_FALSE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 0);
}

TEST_F(AsyncSemaphoreTest, AvailableReflectsReleases)
{
	alt::async_semaphore semaphore{0};
	EXPECT_EQ(semaphore.available(), 0);

	semaphore.release();
	EXPECT_EQ(semaphore.available(), 1);

	semaphore.release(3);
	EXPECT_EQ(semaphore.available(), 4);

	ASSERT_TRUE(semaphore.try_acquire());
	EXPECT_EQ(semaphore.available(), 3);

	// A semaphore constructed with permits reports them without being released to.
	const alt::async_semaphore stocked{5};
	EXPECT_EQ(stocked.available(), 5);
}

TEST_F(AsyncSemaphoreTest, AWaiterResumedByAReleaseCanTakeAnotherOfItsPermits)
{
	// release(n) places every permit before it resumes anybody, so the waiter resumed
	// by the first permit finds the second already in the count and takes it on the
	// re-await. Both waits are satisfied by one release(2) either way: what the
	// assertions pin is that outcome, not the route the second permit took, because a
	// permit's route is not observable from outside the semaphore. Which is what makes
	// the ordering inside release() a free choice, and it spends it on resuming last.
	alt::async_semaphore semaphore{0};
	wait_record          first;
	wait_record          second;

	const auto handle = alt::start(twice_acquiring_root(semaphore, first, second));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(first.resumptions, 0);

	semaphore.release(2);

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_TRUE(first.satisfied);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.satisfied);
	EXPECT_EQ(semaphore.available(), 0);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, ReleasingMoreThanThereAreWaitersPutsTheRestInTheCount)
{
	alt::async_semaphore semaphore{0};
	wait_record          record;

	const auto handle = alt::start(acquiring_root(semaphore, record, 1));
	ASSERT_EQ(live_frames, 1);

	semaphore.release(3);

	// One permit to the waiter, the remaining two into the count.
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_EQ(semaphore.available(), 2);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, ReleasingZeroChangesNothing)
{
	alt::async_semaphore semaphore{1};
	wait_record          record;

	semaphore.release(0);
	EXPECT_EQ(semaphore.available(), 1);

	// And with a waiter parked, where it must not look like a permit arriving.
	ASSERT_TRUE(semaphore.try_acquire());
	const auto handle = alt::start(acquiring_root(semaphore, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	semaphore.release(0);
	EXPECT_EQ(record.resumptions, 0);
	EXPECT_EQ(semaphore.available(), 0);

	semaphore.release();
	EXPECT_EQ(record.resumptions, 1);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, AReleasedWaiterHoldsNoPermit)
{
	// The fails-open property. A wait that ends without a permit must say so and must
	// leave the count exactly as it found it: a released waiter that had taken a permit
	// would have spent one nobody holds, and the next acquirer would wait for it.
	alt::async_semaphore semaphore{0};
	wait_record          record;

	auto handle = alt::start(acquiring_root(semaphore, record, 1));
	ASSERT_EQ(record.resumptions, 0);
	ASSERT_EQ(semaphore.available(), 0);

	handle.request_stop();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	// Nothing was consumed on the way out.
	EXPECT_EQ(semaphore.available(), 0);

	// And the next permit really is spendable: a count the released waiter had
	// decremented would be below zero here, so this release would only bring it back up
	// to zero and the acquire below would fail.
	semaphore.release();
	EXPECT_EQ(semaphore.available(), 1);
	EXPECT_TRUE(semaphore.try_acquire());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, AWaiterCancelledBeforeItsAwaitConsumesNoPermit)
{
	// The other way a wait can end without a permit: the chain is abandoned on the way
	// into the suspension point, so the awaiter's ready check never runs at all. The
	// permit must still be there afterwards.
	alt::async_semaphore semaphore{1};
	wait_record          record;

	auto handle = alt::start(parks_then_acquires(semaphore, record));
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	resume_parked(); // reaches the co_await, which observes the stop and destroys the frame

	EXPECT_EQ(record.resumptions, 0);
	EXPECT_EQ(live_frames, 0);
	EXPECT_EQ(semaphore.available(), 1);
}

TEST_F(AsyncSemaphoreTest, CancellingAParkedWaiterReleasesIt)
{
	// The stop callback is the only thing that can reach a parked waiter: nothing in
	// the coroutine machinery runs again until someone resumes the frame.
	alt::async_semaphore semaphore{0};
	wait_record          record;

	auto handle = alt::start(acquiring_root(semaphore, record, 1));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, ACancelledWaiterIsNotResumedAgainByARelease)
{
	// The waiter has gone, so a later release finds an empty queue and must not resume
	// the frame the cancellation already finished.
	alt::async_semaphore semaphore{0};
	wait_record          record;

	auto handle = alt::start(acquiring_root(semaphore, record, 1));
	handle.request_stop();
	ASSERT_EQ(record.resumptions, 1);

	semaphore.release();

	EXPECT_EQ(record.resumptions, 1);

	// With nobody to hand it to, the permit is in the count.
	EXPECT_EQ(semaphore.available(), 1);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, DestroyingTheSemaphoreReleasesParkedWaiters)
{
	wait_record first;
	wait_record second;

	{
		alt::async_semaphore semaphore{0};
		const auto           first_handle  = alt::start(acquiring_root(semaphore, first, 1));
		const auto           second_handle = alt::start(acquiring_root(semaphore, second, 2));
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

TEST_F(AsyncSemaphoreTest, AWaiterThatReAwaitsWhileTheSemaphoreIsBeingDestroyedIsStillReleased)
{
	// As async_event's and async_latch's tests of the same name: the re-await parks in a
	// list that is still whole, and the member's destructor releases it, but only
	// because the semaphore's own destructor released the first round itself.
	wait_record first;
	wait_record second;

	{
		alt::async_semaphore semaphore{0};
		const auto           handle = alt::start(twice_acquiring_root(semaphore, first, second));
		ASSERT_EQ(live_frames, 1);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncSemaphoreTest, AWaitConsumesAtMostOnePermit)
{
	// Driven by hand, because this is a property of how often the ready check is
	// consulted rather than of anything the waiter can see afterwards. The check is
	// what takes the permit, so a wait that consulted it twice on the satisfied path
	// would empty a semaphore that had two.
	alt::async_semaphore semaphore{2};

	auto awaiter = alt::get_awaiter(semaphore);
	ASSERT_TRUE(awaiter.await_ready());
	EXPECT_EQ(semaphore.available(), 1);

	// await_resume only reports what the ready check already decided; it takes nothing.
	EXPECT_TRUE(awaiter.await_resume().has_value());
	EXPECT_EQ(semaphore.available(), 1);
}

TEST_F(AsyncSemaphoreTest, APermitReleasedBetweenTheReadyCheckAndTheLinkIsTakenWithoutParking)
{
	// The window the link's condition closes, reached deterministically by driving the
	// awaiter by hand: its ready check is asked first and fails, the permit arrives
	// second, and only then does it try to park. The condition is the ready check
	// again, so the second consultation is the one that takes the permit -- which is
	// also the other half of AWaitConsumesAtMostOnePermit: one permit for the whole
	// wait, taken on whichever of the two calls succeeded.
	alt::async_semaphore semaphore{0};

	auto awaiter = alt::get_awaiter(semaphore);
	ASSERT_FALSE(awaiter.await_ready());
	ASSERT_EQ(semaphore.available(), 0);

	// Nothing is linked yet, so this permit lands in the count.
	semaphore.release();
	ASSERT_EQ(semaphore.available(), 1);

	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));
	EXPECT_TRUE(awaiter.await_resume().has_value());
	EXPECT_EQ(semaphore.available(), 0);
}

TEST_F(AsyncSemaphoreTest, ASemaphoreWithNoPermitStillParksAtTheLink)
{
	// The converse, so the test above cannot be passed by a link that never parks.
	alt::async_semaphore semaphore{0};

	auto awaiter = alt::get_awaiter(semaphore);
	ASSERT_FALSE(awaiter.await_ready());

	EXPECT_TRUE(awaiter.await_suspend(std::noop_coroutine()));

	// Released before the scope ends. The awaiter's node is linked, and the semaphore
	// was declared first so it outlives the awaiter: left linked, the semaphore's
	// destructor would walk a dead node. The release detaches and resumes it, which
	// also shows the park was a real one and that the permit went to the waiter rather
	// than into the count.
	semaphore.release();
	EXPECT_TRUE(awaiter.await_resume().has_value());
	EXPECT_EQ(semaphore.available(), 0);
}

TEST_F(AsyncSemaphoreTest, AConsumingPredicateThatWinsAtTheLinkIsNeverReportedCancelled)
{
	// The defect this test exists for lived in detail::wait_awaiter rather than here,
	// and only a consuming predicate can expose it. await_suspend used to decide *why*
	// a link had been refused by re-reading the node's state word after dropping the
	// list's mutex. A stop callback winning the node in that gap left the word reading
	// claimed, which the frame could not tell apart from a stop that had won before the
	// condition ever ran -- so a condition that had already taken a permit reported
	// operation_canceled. The permit was then gone for good: nobody held it and nothing
	// would return it, and a bounded-concurrency semaphore under cancellation pressure
	// bleeds down to zero and stops admitting anyone. The same window at a future
	// async_mutex is a lock held by nobody, forever.
	//
	// Driven at the machinery, because the instant that matters is inside link()'s
	// condition, under the list's mutex, where nothing outside the awaiter can reach.
	// The probe consumes exactly as try_acquire() does and then does to the node what
	// the stop callback's compare-exchange would have done.
	alt::detail::waiter_list list;
	int                      permits = 0;

	probe_awaiter awaiter{list, permits};
	ASSERT_FALSE(awaiter.await_ready());

	// The permit arrives between the two consultations, and the stop lands in the same
	// breath, after the condition has consumed it.
	permits = 1;
	awaiter.claim_the_node_while_consuming();

	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));

	// The predicate took the permit, so the wait must say it got one.
	EXPECT_EQ(permits, 0);
	EXPECT_TRUE(awaiter.await_resume().has_value());
}

TEST_F(AsyncSemaphoreTest, AWaiterWhoseNodeIsWonBeforeTheLinkIsStillReportedCancelled)
{
	// The converse, so the test above cannot be passed by an await_suspend that calls
	// every refused link satisfied. link() tests the election before it runs the
	// condition, so a node already won never reaches the predicate: nothing is
	// consumed, and the wait is the released one it was before.
	alt::detail::waiter_list list;
	int                      permits = 0;

	probe_awaiter awaiter{list, permits};
	ASSERT_FALSE(awaiter.await_ready());

	awaiter.claim_the_node();

	// A permit arrives too, so that the only thing keeping the predicate from
	// consuming it is the election having already been lost.
	permits = 1;

	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));
	EXPECT_EQ(permits, 1);

	const auto result = awaiter.await_resume();
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error(), released_error);
}

TEST_F(AsyncSemaphoreTest, AConcurrentReleaseIsNeverMissedByAnArrivingWaiter)
{
	// One waiter arriving while one release runs, which is the smallest shape of the
	// contention and the one ThreadSanitizer reads most cheaply: a waiter that finds
	// no permit and links, against a release landing between those two steps. The
	// rendezvous sits in the waiter's own body immediately before the co_await, so the
	// two threads reach the semaphore within nanoseconds of each other. The semaphore
	// is inspected before it leaves scope because its destructor would release a lost
	// waiter and hide the defect.
	//
	// What this shape does not catch is the lost wakeup itself, and the figure is
	// worth having here rather than in a report nobody reads next year. Against a
	// release that detaches and then raises the count in a second critical section --
	// the defect waiter_list::detach_one's action overload exists to prevent -- this
	// test failed 0 of 20 runs of 100 repeats. The likeliest reason is that the gap
	// between that detach and that increment is a handful of instructions wide and
	// opens as soon as the release starts, while the waiter still has await_ready and
	// its stop-callback registration to get through before it links, so the two
	// systematically miss each other however close together the rendezvous starts
	// them. EveryPermitReachesAWaiterWhenReleasesAndArrivalsContend is the test that
	// catches it, 20 of 20 runs, by putting eight threads on the one mutex so that the
	// gaps and the links are scheduled against each other instead of started
	// together. Both measurements are from the same loop over the same binary, read
	// from its exit status.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_semaphore     semaphore{0};
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		{
			std::jthread waiter{[&] {
				const auto handle = alt::start(racing_acquirer(semaphore, record, arrived));
			}};

			rendezvous(arrived, 2);
			semaphore.release();
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_TRUE(record.satisfied) << "repeat " << repeat;
		ASSERT_EQ(semaphore.available(), 0) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncSemaphoreTest, AStopAndAReleaseDoNotBothResumeAndThePermitIsNotLost)
{
	// End to end, the property the claim election exists for, plus the one a semaphore
	// adds to it: the release and a cancellation reaching for the same parked waiter
	// must not both resume it, and whichever loses, the permit must still be accounted
	// for. The rendezvous is what makes it contended: both threads block on one counter
	// and the second arrival releases both, so the detach and the stop callback's claim
	// reach the same state word together.
	//
	// The two outcomes are both legitimate and the assertion covers them together: if
	// the release won the node the waiter holds the permit and the count is empty, and
	// if the stop won it the release found nothing to hand the permit to and left it in
	// the count. A permit that reached neither would be one the semaphore had destroyed.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		reset_coroutine_test_state();

		alt::async_semaphore     semaphore{0};
		wait_record              record;
		std::atomic<std::size_t> arrived{0};

		auto handle = alt::start(acquiring_root(semaphore, record, 1));
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		{
			std::jthread releaser{[&] {
				rendezvous(arrived, 2);
				semaphore.release();
			}};

			std::jthread stopper{[&handle, &arrived] {
				rendezvous(arrived, 2);
				handle.request_stop();
			}};
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_EQ(semaphore.available(), record.satisfied ? 0 : 1) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncSemaphoreTest, EveryPermitReachesAWaiterWhenReleasesAndArrivalsContend)
{
	// Four acquirers and four releasers, one permit each, every one of the eight
	// threads held at the same rendezvous until the last arrives. Four permits and
	// four acquirers means the outcome is fixed however the eight are scheduled:
	// every acquirer ends up with a permit and the count ends empty. A permit that
	// reached neither a waiter nor the count leaves an acquirer parked, which the
	// satisfied count reads before the semaphore's destructor can release it and hide
	// the defect.
	//
	// The mechanism is the contention itself rather than the rendezvous alone. Eight
	// threads released together serialize their links and their detach-and-deposit
	// sections on the list's one mutex, in an order the scheduler picks, so a link
	// lands between some releaser's detach and its deposit on some iterations -- which
	// is the interleaving that a release deciding those two things in separate
	// critical sections loses a permit to. Against such a release this test failed 20
	// of 20 runs of 100 repeats, read from the binary's exit status. The rendezvous
	// sits inside each acquirer's
	// coroutine, after its frame is built, so that the acquisition is the next thing
	// that thread does.
	//
	// These waiters are resumed on four threads at once, which is why they record into
	// atomics and track no frame: live_frames and wait_record are unsynchronized, so a
	// waiter touching them off-thread would be a data race in the test rather than a
	// finding about the library.
	constexpr std::size_t acquirers = 4;
	constexpr std::size_t parties   = acquirers * 2;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		// Declared ahead of the semaphore, so that anything its destructor resumes is
		// writing into counters that are still alive.
		std::atomic<int>         satisfied{0};
		std::atomic<int>         released{0};
		std::atomic<std::size_t> arrived{0};

		alt::async_semaphore semaphore{0};

		{
			std::vector<std::jthread> threads;
			threads.reserve(parties);

			for(std::size_t index = 0; index < acquirers; ++index)
			{
				threads.emplace_back([&] {
					const auto handle = alt::start(racing_counting_acquirer(semaphore, satisfied, released, arrived, parties));
				});
			}

			for(std::size_t index = 0; index < acquirers; ++index)
			{
				threads.emplace_back([&] {
					rendezvous(arrived, parties);
					semaphore.release();
				});
			}
		}

		ASSERT_EQ(satisfied.load(std::memory_order_acquire), static_cast<int>(acquirers)) << "repeat " << repeat;
		ASSERT_EQ(released.load(std::memory_order_acquire), 0) << "repeat " << repeat;
		ASSERT_EQ(semaphore.available(), 0) << "repeat " << repeat;
	}
}

TEST_F(AsyncSemaphoreTest, NoPermitIsLostWhenAStopAndAReleaseBothRaceAnArrivingWaiter)
{
	// The end-to-end form of
	// AConsumingPredicateThatWinsAtTheLinkIsNeverReportedCancelled, and the shape
	// neither earlier race had: AStopAndAReleaseDoNotBothResumeAndThePermitIsNotLost
	// puts a stop against a waiter that is already parked, and
	// EveryPermitReachesAWaiterWhenReleasesAndArrivalsContend puts releases against
	// waiters that are arriving but stops nothing. The defect needs both at once
	// against an arriving waiter: a permit arriving in time for the link's condition
	// to take it, and a stop landing on the same node in the same breath.
	//
	// The invariant is conservation, and it holds under every schedule: each permit
	// released is either held by an acquirer that reported success or still in the
	// count, so satisfied + available() is exactly the number released. A waiter that
	// consumed a permit and then reported cancelled breaks it by one. Acquirers that
	// are cancelled contribute to neither side, which is the point -- they must hold
	// nothing.
	//
	// Two things make the race occur rather than merely be possible.
	//
	// Each acquirer carries an external stop source created before it starts, so a
	// stopper thread can reach it while it is still on its way to the co_await. The
	// handle alt::start returns cannot do that: it does not exist until the coroutine
	// has already suspended.
	//
	// And the stoppers do one std::stop_callback's worth of bookkeeping after the
	// rendezvous, before they request anything. Without it the stop lands before its
	// target has even looked at the semaphore -- which is a different and benign
	// interleaving, the one cancellation_checked catches on the way in -- and the
	// defect is reached only by schedule noise: measured 2 of 20 runs of 100 repeats.
	// With the stopper doing the work its target must do between its ready check and
	// its link, the request lands near the condition instead, and the defect is caught
	// in 20 of 20 runs. Both figures are against the pre-fix await_suspend, from one
	// loop over the same binary, read from its exit status; against the fix as it
	// stands, 0 of 20.
	constexpr std::size_t acquirers = 4;
	constexpr std::size_t releasers = 4;
	constexpr std::size_t stoppers  = 2;
	constexpr std::size_t parties   = acquirers + releasers + stoppers;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		// Declared ahead of the semaphore, so that anything its destructor resumes is
		// writing into counters that are still alive.
		std::atomic<int>         satisfied{0};
		std::atomic<int>         released{0};
		std::atomic<std::size_t> arrived{0};

		alt::async_semaphore          semaphore{0};
		std::vector<std::stop_source> sources(acquirers);
		std::stop_source              unrelated;

		{
			std::vector<std::jthread> threads;
			threads.reserve(parties);

			for(std::size_t index = 0; index < acquirers; ++index)
			{
				threads.emplace_back([&, index] {
					const auto handle = alt::start(racing_counting_acquirer(semaphore, satisfied, released, arrived, parties), {.token = sources[index].get_token()});
				});
			}

			for(std::size_t index = 0; index < releasers; ++index)
			{
				threads.emplace_back([&] {
					rendezvous(arrived, parties);
					semaphore.release();
				});
			}

			for(std::size_t index = 0; index < stoppers; ++index)
			{
				threads.emplace_back([&, index] {
					rendezvous(arrived, parties);
					match_an_arriving_waiters_bookkeeping(unrelated.get_token());
					sources[index].request_stop();
				});
			}
		}

		ASSERT_EQ(satisfied.load(std::memory_order_acquire) + semaphore.available(), static_cast<std::ptrdiff_t>(releasers)) << "repeat " << repeat;
	}
}

TEST_F(AsyncSemaphoreTest, ConcurrentTryAcquiresHandOutEachPermitOnce)
{
	// Four threads reach for two permits together, rendezvousing immediately before the
	// try_acquire so that they hit the count in the same instant. Exactly two may win: a
	// try_acquire written as a load, a test and a store would let more than one caller
	// see the same permit and drive the count below zero.
	constexpr int parties = 4;
	constexpr int permits = 2;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		alt::async_semaphore     semaphore{permits};
		std::atomic<int>         acquired{0};
		std::atomic<std::size_t> arrived{0};

		{
			std::vector<std::jthread> contenders;
			contenders.reserve(parties);

			for(int index = 0; index < parties; ++index)
			{
				contenders.emplace_back([&] {
					rendezvous(arrived, parties);

					if(semaphore.try_acquire())
					{
						acquired.fetch_add(1, std::memory_order_acq_rel);
					}
				});
			}
		}

		ASSERT_EQ(acquired.load(std::memory_order_acquire), permits) << "repeat " << repeat;
		ASSERT_EQ(semaphore.available(), 0) << "repeat " << repeat;
	}
}

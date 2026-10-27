#include <gtest/gtest.h>

#include <algorithm>
#include <alt/async_mutex.hpp>
#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "coroutine_test_support.hpp"

using namespace alt_test;

// The concept describes context-free awaitables, so this is the check that the
// awaiter still accepts an erased std::coroutine_handle<> and therefore works in a
// coroutine that relays no cancellation context at all.
static_assert(alt::awaitable<alt::async_mutex&>);
static_assert(std::is_same_v<alt::await_result_t<alt::async_mutex&>, std::expected<alt::async_mutex_lock, std::error_code>>);

// A mutex is referred to by the awaiters inside suspended coroutine frames and by
// every live guard, so it can be neither copied nor moved.
static_assert(!std::is_copy_constructible_v<alt::async_mutex>);
static_assert(!std::is_move_constructible_v<alt::async_mutex>);

// The guard's type-level guarantee, which is the whole reason it exists: there is no
// way to build one that does not already hold the lock. A caller cannot name the
// fails-open state, let alone reach it.
static_assert(!std::is_default_constructible_v<alt::async_mutex_lock>);
static_assert(!std::is_constructible_v<alt::async_mutex_lock, alt::async_mutex&>);
static_assert(!std::is_constructible_v<alt::async_mutex_lock, alt::async_mutex*>);

// Move-only, like std::unique_lock, so the lock is released exactly once however
// the guard is passed around.
static_assert(std::is_move_constructible_v<alt::async_mutex_lock>);
static_assert(std::is_move_assignable_v<alt::async_mutex_lock>);
static_assert(!std::is_copy_constructible_v<alt::async_mutex_lock>);
static_assert(!std::is_copy_assignable_v<alt::async_mutex_lock>);

namespace
{
	/** How many times the contended tests repeat their race. */
	constexpr int race_repeats = 100;

	/**
	 * How many waiters the hand-off chain test queues behind one lock. Large enough
	 * that a recursive hand-off is unmistakable rather than merely detectable: the
	 * nested frame measures 400 bytes, so the chain costs 4 MB of stack in a Debug
	 * build and overflows outright under AddressSanitizer.
	 */
	constexpr int handoff_chain_length = 10000;

	/**
	 * How many release-against-arrival rounds AnArrivingLockerIsNeverStrandedByAConcurrentUnlock
	 * runs per repeat. The window it attempts is about a nanosecond wide, so the
	 * figure comes from the measurement recorded there rather than from taste: 25
	 * rounds caught the defect in 18 of 20 runs and 75 caught it in 20 of 20, twice.
	 */
	constexpr int stranding_race_rounds = 75;

	/**
	 * How far the stack pointer may legitimately move across the whole hand-off
	 * chain. An iterative drain resumes every waiter from the same loop at the same
	 * depth, so the measured spread is exactly zero bytes on all four presets; a
	 * recursive one grows by a frame per waiter, which even at a hypothetical 32
	 * bytes a frame is eighty times this allowance.
	 */
	constexpr std::uintptr_t stack_growth_allowance = 4096U;

	/**
	 * The condition every released wait reports, named the way a caller names it:
	 * compared against as a std::errc rather than built into a second
	 * std::error_code to match. See alt::wait_result.
	 */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/** The condition a try_lock that found the mutex held reports. */
	constexpr std::errc busy_error = std::errc::device_or_resource_busy;

	/** What one waiter coroutine recorded about its wait, for the test to inspect. */
	struct lock_record
	{
		/** How many times the statement after the co_await ran. Two is a double resume. */
		int resumptions = 0;

		/** Whether the wait reported success, which for a mutex means the lock. */
		bool satisfied = false;

		/** Whether the guard the wait yielded actually owns a lock. */
		bool held = false;

		/** The error a released wait reported. */
		std::error_code error;
	};

	/** Records one wait's outcome. Shared by every waiter coroutine below. */
	void record_outcome(lock_record& record, const std::expected<alt::async_mutex_lock, std::error_code>& result)
	{
		++record.resumptions;
		record.satisfied = result.has_value();

		// The property the guard's type makes unrepresentable, asserted anyway: a
		// satisfied wait must yield a guard that owns something, and a released one must
		// yield no guard at all.
		record.held = result.has_value() && result->owns_lock();

		if(!result)
		{
			record.error = result.error();
		}
	}

	/** A cancellable locker that records how its wait ended and then releases the lock. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the mutex and the record are owned by the calling test, which holds them until live_frames is back to zero, so neither reference can dangle.
	alt::cancellable_task locking_root(alt::async_mutex& mutex, lock_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		const auto          held = co_await mutex;
		record_outcome(record, held);
	}

	/**
	 * Takes the lock and then parks while still holding it, so a test can hold the
	 * critical section open across other arrivals. The guard lives to the end of the
	 * body, so resuming the park is what releases the lock.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as locking_root above.
	alt::cancellable_task locks_and_holds(alt::async_mutex& mutex, lock_record& record, int tag)
	{
		const frame_tracker tracker{tag};
		const auto          held = co_await mutex;
		record_outcome(record, held);

		if(held)
		{
			co_await park{};
		}
	}

	/**
	 * @brief Releases @p outer from inside its own critical section, and only then
	 *        locks and unlocks @p inner.
	 *
	 * The order is the load-bearing part. This body runs inside @p outer's hand-off
	 * drain, so the release of @p outer is a nested one and does nothing but record
	 * that the lock came back; the lock and unlock of @p inner then runs a whole drain
	 * of its own on top of that record. See
	 * AQueuedLockerStillGetsInWhenAWaiterDrainsASecondMutexAfterReleasing.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the mutexes and the records are owned by the calling test, which holds them until live_frames is back to zero, so no reference can dangle.
	alt::cancellable_task releases_then_drains_another(alt::async_mutex& outer, alt::async_mutex& inner, lock_record& record, lock_record& inner_record, int tag)
	{
		const frame_tracker tracker{tag};
		auto                held = co_await outer;
		record_outcome(record, held);

		if(held)
		{
			held->unlock();

			auto other = inner.try_lock();
			record_outcome(inner_record, other);

			if(other)
			{
				other->unlock();
			}
		}
	}

	/** The same two mutexes in the other order: @p inner drained first, @p outer released after. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as releases_then_drains_another above.
	alt::cancellable_task drains_another_then_releases(alt::async_mutex& outer, alt::async_mutex& inner, lock_record& record, lock_record& inner_record, int tag)
	{
		const frame_tracker tracker{tag};
		auto                held = co_await outer;
		record_outcome(record, held);

		if(held)
		{
			auto other = inner.try_lock();
			record_outcome(inner_record, other);

			if(other)
			{
				other->unlock();
			}

			held->unlock();
		}
	}

	/** Parks on something else first, so a stop can be requested before it ever reaches the mutex. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as locking_root above.
	alt::cancellable_task parks_then_locks(alt::async_mutex& mutex, lock_record& record)
	{
		const frame_tracker tracker{1};
		co_await park{};
		record_outcome(record, co_await mutex);
	}

	/**
	 * @brief Records how deep the stack is at the point a waiter's body runs.
	 *
	 * Deliberately not inlined, so that the frame it reads is its own and sits
	 * directly below the waiter body that called it, and deliberately not a coroutine
	 * local: a coroutine's locals live in its frame, which is heap allocated, and say
	 * nothing about the stack the body is running on.
	 *
	 * The frame address rather than the address of a local, which was measured: under
	 * AddressSanitizer a local whose address escapes is moved to ASan's fake stack,
	 * whose size-class rings put the same call's "stack" address anywhere within a
	 * mebibyte. Reading the frame address instead gave a spread of zero bytes on all
	 * four presets, and 3999600 bytes against a recursive hand-off.
	 */
	[[gnu::noinline]] void note_stack_depth(std::uintptr_t& lowest, std::uintptr_t& highest)
	{
		const auto address = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));

		lowest  = std::min(lowest, address);
		highest = std::max(highest, address);
	}

	/** Enters the critical section, notes the stack depth it ran at, and leaves. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the mutex and the counters are owned by the calling test, which holds them until every waiter has run.
	alt::cancellable_task stack_probing_locker(alt::async_mutex& mutex, int& entered, std::uintptr_t& lowest, std::uintptr_t& highest)
	{
		if(const auto held = co_await mutex)
		{
			++entered;
			note_stack_depth(lowest, highest);
		}
	}

	/**
	 * A locker that rendezvouses immediately before its co_await and writes an
	 * unsynchronized counter inside the critical section. The counter is the point:
	 * it is plain, not atomic, so two coroutines in the section at once is a data
	 * race for ThreadSanitizer to report as well as a count that comes out wrong.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the mutex, the counters and the rendezvous are owned by the calling test, which joins every thread before leaving the scope that holds them.
	alt::cancellable_task counting_locker(alt::async_mutex& mutex, int& guarded, std::atomic<int>& entered, std::atomic<std::size_t>& arrived, std::size_t parties)
	{
		rendezvous(arrived, parties);

		if(const auto held = co_await mutex)
		{
			++guarded;
			entered.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	/**
	 * A locker that rendezvouses immediately before its co_await, so that another
	 * thread's release lands in the window between this locker's own ready check and
	 * its link.
	 *
	 * Tracks no frame, deliberately: the test this serves runs many rounds on one
	 * worker thread while the main thread judges each round, and whichever thread
	 * resumes this waiter is the one that would touch live_frames. The record it does
	 * write is handed over by that test's own round handshake.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the mutex, the record and the counter are owned by the calling test, which joins the thread before leaving the scope that holds them.
	alt::cancellable_task racing_single_locker(alt::async_mutex& mutex, lock_record& record, std::atomic<std::size_t>& arrived)
	{
		rendezvous(arrived, 2);
		record_outcome(record, co_await mutex);
	}

	/**
	 * A locker that rendezvouses immediately before its co_await and records only
	 * into atomics, for the tests whose waiters are resumed on several threads at
	 * once. frame_tracker and lock_record are both unsynchronized, so a waiter
	 * resumed off-thread must not touch them. The rendezvous is inside the coroutine
	 * so that the frame is already built when it returns and the next thing this
	 * thread does is the acquisition itself.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): same reasoning as counting_locker above.
	alt::cancellable_task racing_locker(alt::async_mutex& mutex, std::atomic<int>& entered, std::atomic<std::size_t>& arrived, std::size_t parties)
	{
		rendezvous(arrived, parties);

		if(const auto held = co_await mutex)
		{
			entered.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	/**
	 * Does roughly the bookkeeping an arriving waiter does between its ready check
	 * and its link -- one std::stop_callback registered and destroyed -- so that a
	 * stop requested after it lands near the moment that waiter's link condition runs
	 * instead of well before the waiter has looked at the primitive at all. Both sides
	 * scale with the same work, so this is not a calibration against a wall clock.
	 * Borrowed verbatim from tests/test_async_semaphore.cpp, where it took the
	 * detection rate of the same window from 2 of 20 runs to 20 of 20.
	 */
	inline void match_an_arriving_lockers_bookkeeping(const std::stop_token& token)
	{
		const std::stop_callback callback{token, [] {}};
	}

	/**
	 * The state one repeat of AnArrivingLockerIsNeverStrandedByAConcurrentUnlock
	 * shares between its two threads.
	 *
	 * The mutex is declared last so that it is destroyed first: everything a waiter
	 * its destructor releases might write to is then still alive.
	 */
	struct stranding_race_state
	{
		/** The arriving locker's outcome for the round in flight. */
		lock_record record;

		/** Meets the arriving locker immediately before its co_await. */
		std::atomic<std::size_t> arrived{0};

		/** The round this thread has set up, published to the worker. */
		std::atomic<int> round_to_run{-1};

		/** The round the worker has finished attempting, published back. */
		std::atomic<int> round_finished{-1};

		/** Never stopped. Only here to give the releasing thread a callback to register. */
		std::stop_source unrelated;

		alt::async_mutex mutex;
	};

	/** The worker side of AnArrivingLockerIsNeverStrandedByAConcurrentUnlock. See that test. */
	void drive_stranding_rounds(const std::stop_token& stop, stranding_race_state& state)
	{
		for(int round = 0; round < stranding_race_rounds; ++round)
		{
			while(state.round_to_run.load(std::memory_order_acquire) < round)
			{
				// The test abandons its loop on the first failure, so this is how the
				// worker learns the round it is waiting for is never coming.
				if(stop.stop_requested())
				{
					return;
				}

				std::this_thread::yield();
			}

			const auto handle = alt::start(racing_single_locker(state.mutex, state.record, state.arrived));
			state.round_finished.store(round, std::memory_order_release);
		}
	}

	/** One round of AnArrivingLockerIsNeverStrandedByAConcurrentUnlock. See that test. */
	void run_one_stranding_round(stranding_race_state& state, int repeat, int round)
	{
		state.record  = lock_record{};
		state.arrived = 0;

		auto owner = state.mutex.try_lock();
		ASSERT_TRUE(owner.has_value()) << "repeat " << repeat << ", round " << round;

		state.round_to_run.store(round, std::memory_order_release);

		rendezvous(state.arrived, 2);
		match_an_arriving_lockers_bookkeeping(state.unrelated.get_token());
		owner->unlock();

		while(state.round_finished.load(std::memory_order_acquire) < round)
		{
			std::this_thread::yield();
		}

		// A stranded locker has not run the statement after its co_await. Judged here,
		// before the next round or the mutex's destructor could rescue it.
		ASSERT_EQ(state.record.resumptions, 1) << "repeat " << repeat << ", round " << round;
		ASSERT_TRUE(state.record.satisfied) << "repeat " << repeat << ", round " << round;
		ASSERT_TRUE(state.record.held) << "repeat " << repeat << ", round " << round;
		ASSERT_TRUE(state.mutex.try_lock().has_value()) << "repeat " << repeat << ", round " << round;
	}

	/** One repeat of AnArrivingLockerIsNeverStrandedByAConcurrentUnlock. See that test. */
	void run_one_stranding_race(int repeat)
	{
		stranding_race_state state;

		// Declared after the state, so the join happens while the state is still alive.
		const std::jthread locker{[&state](const std::stop_token& stop) { drive_stranding_rounds(stop, state); }};

		for(int round = 0; round < stranding_race_rounds; ++round)
		{
			ASSERT_NO_FATAL_FAILURE(run_one_stranding_round(state, repeat, round));
		}
	}

	/** One repeat of AStopAndAnUnlockDoNotBothResumeAndTheLockIsNotLost. See that test. */
	void run_one_stop_versus_unlock_race(int repeat)
	{
		reset_coroutine_test_state();

		// Declared ahead of the mutex, so that anything its destructor resumes is
		// writing into storage that is still alive. Unreachable in a passing run, but
		// an assertion below that fires leaves this scope with the waiter still parked,
		// and the destructor's release would then turn the regression this test exists
		// to report into whatever the dead record happened to contain.
		lock_record              record;
		std::atomic<std::size_t> arrived{0};

		alt::async_mutex mutex;

		auto owner = mutex.try_lock();
		ASSERT_TRUE(owner.has_value()) << "repeat " << repeat;

		auto handle = alt::start(locking_root(mutex, record, 1));
		ASSERT_EQ(live_frames, 1) << "repeat " << repeat;

		{
			std::jthread releaser{[&] {
				rendezvous(arrived, 2);
				owner->unlock();
			}};

			std::jthread stopper{[&handle, &arrived] {
				rendezvous(arrived, 2);
				handle.request_stop();
			}};
		}

		ASSERT_EQ(record.resumptions, 1) << "repeat " << repeat;
		ASSERT_EQ(record.satisfied, record.held) << "repeat " << repeat;
		ASSERT_TRUE(mutex.try_lock().has_value()) << "repeat " << repeat;
		ASSERT_EQ(live_frames, 0) << "repeat " << repeat;
	}

	/** One repeat of EveryLockerEntersTheCriticalSectionWhenArrivalsAndUnlocksContend. See that test. */
	void run_one_contended_locking_race(int repeat, std::size_t lockers)
	{
		// Declared ahead of the mutex, so that anything its destructor resumes is
		// writing into counters that are still alive.
		int                      guarded = 0;
		std::atomic<int>         entered{0};
		std::atomic<std::size_t> arrived{0};

		alt::async_mutex mutex;

		auto owner = mutex.try_lock();
		ASSERT_TRUE(owner.has_value()) << "repeat " << repeat;

		{
			std::vector<std::jthread> threads;
			threads.reserve(lockers);

			for(std::size_t index = 0; index < lockers; ++index)
			{
				threads.emplace_back([&] {
					const auto handle = alt::start(counting_locker(mutex, guarded, entered, arrived, lockers + 1));
				});
			}

			rendezvous(arrived, lockers + 1);
			owner->unlock();
		}

		ASSERT_EQ(entered.load(std::memory_order_acquire), static_cast<int>(lockers)) << "repeat " << repeat;
		ASSERT_EQ(guarded, static_cast<int>(lockers)) << "repeat " << repeat;
		ASSERT_TRUE(mutex.try_lock().has_value()) << "repeat " << repeat;
	}

	/** One repeat of NoLockIsLostWhenAStopAndAnUnlockRaceAnArrivingLocker. See that test. */
	void run_one_stop_and_unlock_versus_arrival_race(int repeat, std::size_t lockers, std::size_t stoppers)
	{
		// Declared ahead of the mutex, so that anything its destructor resumes is
		// writing into counters that are still alive.
		std::atomic<int>         entered{0};
		std::atomic<std::size_t> arrived{0};

		alt::async_mutex              mutex;
		std::vector<std::stop_source> sources(lockers);
		std::stop_source              unrelated;

		const std::size_t parties = lockers + stoppers + 1;

		auto owner = mutex.try_lock();
		ASSERT_TRUE(owner.has_value()) << "repeat " << repeat;

		{
			std::vector<std::jthread> threads;
			threads.reserve(lockers + stoppers);

			for(std::size_t index = 0; index < lockers; ++index)
			{
				threads.emplace_back([&, index] {
					const auto handle = alt::start(racing_locker(mutex, entered, arrived, parties), {.token = sources[index].get_token()});
				});
			}

			for(std::size_t index = 0; index < stoppers; ++index)
			{
				threads.emplace_back([&, index] {
					rendezvous(arrived, parties);
					match_an_arriving_lockers_bookkeeping(unrelated.get_token());
					sources[index].request_stop();
				});
			}

			rendezvous(arrived, parties);
			owner->unlock();
		}

		ASSERT_TRUE(mutex.try_lock().has_value()) << "repeat " << repeat << ", lockers that got in: " << entered.load(std::memory_order_acquire);
	}

	/**
	 * @brief Takes the lock and leaves no guard holding it.
	 *
	 * The awaiter's ready check is what acquires (see async_mutex::mutex_awaiter),
	 * and the guard is only built in await_resume, so stopping between the two leaves
	 * the lock held by nothing that has a destructor. That is what lets a test park
	 * waiters behind the lock and then destroy the mutex while they are still parked:
	 * a real guard would hand the lock on as it died and satisfy them instead, and
	 * there is no other way to hold the lock, which is the point of the guard.
	 *
	 * The awaiter is never linked, its ready check having succeeded, so it leaves no
	 * node behind for the mutex's destructor to walk.
	 */
	[[nodiscard]] bool take_the_lock_leaving_no_guard(alt::async_mutex& mutex)
	{
		auto awaiter = alt::get_awaiter(mutex);
		return awaiter.await_ready();
	}
}

class AsyncMutexTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
	}
};

TEST_F(AsyncMutexTest, LockingAnUncontendedMutexIsReady)
{
	alt::async_mutex mutex;

	{
		// Taken through the awaiter directly, because never suspending is a property of
		// await_ready rather than of anything observable after the fact.
		auto awaiter = alt::get_awaiter(mutex);
		EXPECT_TRUE(awaiter.await_ready());

		// The ready check is what takes the lock, so the mutex is already held.
		EXPECT_FALSE(mutex.try_lock().has_value());

		const auto held = awaiter.await_resume();
		ASSERT_TRUE(held.has_value());
		EXPECT_TRUE(held->owns_lock());
	}

	// The guard went out of scope, so the lock came back.
	EXPECT_TRUE(mutex.try_lock().has_value());

	// And end to end: nothing resumes this waiter, so it can only have run through.
	alt::async_mutex second;
	lock_record      record;
	const auto       handle = alt::start(locking_root(second, record, 1));

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_TRUE(record.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(second.try_lock().has_value());
}

TEST_F(AsyncMutexTest, TheGuardUnlocksOnDestruction)
{
	alt::async_mutex mutex;

	{
		auto held = mutex.try_lock();
		ASSERT_TRUE(held.has_value());
		EXPECT_TRUE(held->owns_lock());
		EXPECT_FALSE(mutex.try_lock().has_value());
	}

	EXPECT_TRUE(mutex.try_lock().has_value());

	// And the destruction is a hand-off, not merely a flag being cleared: a waiter
	// parked behind the guard is resumed by the guard dying.
	lock_record record;

	{
		auto held = mutex.try_lock();
		ASSERT_TRUE(held.has_value());

		const auto handle = alt::start(locking_root(mutex, record, 1));
		ASSERT_EQ(record.resumptions, 0);
		ASSERT_EQ(live_frames, 1);
	}

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_TRUE(record.satisfied);
	EXPECT_TRUE(record.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, ASecondLockerSuspendsUntilTheFirstUnlocks)
{
	alt::async_mutex mutex;
	lock_record      first;
	lock_record      second;

	// The first locker takes the lock and parks inside the critical section.
	const auto first_handle = alt::start(locks_and_holds(mutex, first, 1));
	ASSERT_EQ(first.resumptions, 1);
	ASSERT_TRUE(first.held);

	const auto second_handle = alt::start(locking_root(mutex, second, 2));

	// Parked: the frame is alive and the statement after the co_await has not run.
	EXPECT_EQ(second.resumptions, 0);
	EXPECT_EQ(live_frames, 2);

	// The first locker leaves its critical section, which releases the lock.
	resume_parked();

	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.satisfied);
	EXPECT_TRUE(second.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, UnlockHandsOwnershipToTheNextWaiterInFifoOrder)
{
	alt::async_mutex mutex;
	lock_record      first;
	lock_record      second;
	lock_record      third;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	const auto first_handle  = alt::start(locking_root(mutex, first, 1));
	const auto second_handle = alt::start(locking_root(mutex, second, 2));
	const auto third_handle  = alt::start(locking_root(mutex, third, 3));
	ASSERT_EQ(live_frames, 3);

	// One release, and the whole queue drains through it in turn: each waiter's guard
	// dies at the end of its body and hands the lock to the next.
	owner->unlock();

	// Link order is resume order, so the oldest waiter got the lock first.
	EXPECT_EQ(destruction_order, (std::vector<int>{1, 2, 3}));
	EXPECT_TRUE(first.held);
	EXPECT_TRUE(second.held);
	EXPECT_TRUE(third.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, MovingTheGuardTransfersOwnership)
{
	alt::async_mutex                     mutex;
	std::optional<alt::async_mutex_lock> moved;

	{
		auto owner = mutex.try_lock();
		ASSERT_TRUE(owner.has_value());

		moved.emplace(std::move(*owner));
		EXPECT_FALSE(owner->owns_lock());
		EXPECT_TRUE(moved->owns_lock());

		// The moved-from guard is destroyed at the end of this scope.
	}

	// It did not double-unlock on the way out: a moved-from guard that still released
	// would have freed a lock the moved-to guard believes it holds, and this try_lock
	// would succeed -- two owners of one critical section.
	EXPECT_TRUE(moved->owns_lock());
	EXPECT_FALSE(mutex.try_lock().has_value());

	moved.reset();
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, MoveAssigningAGuardReleasesTheLockItHeld)
{
	alt::async_mutex first;
	alt::async_mutex second;

	auto first_guard = first.try_lock();
	ASSERT_TRUE(first_guard.has_value());

	auto second_guard = second.try_lock();
	ASSERT_TRUE(second_guard.has_value());

	// The assignment overwrites a guard that holds first's lock, so that lock has to
	// go somewhere: dropping it on the floor would leave first held by nobody forever.
	*first_guard = std::move(*second_guard);

	EXPECT_TRUE(first_guard->owns_lock());
	EXPECT_FALSE(second_guard->owns_lock());
	EXPECT_TRUE(first.try_lock().has_value());
	EXPECT_FALSE(second.try_lock().has_value());

	// Self-assignment leaves the lock exactly where it was.
	auto& alias  = *first_guard;
	*first_guard = std::move(alias);
	EXPECT_TRUE(first_guard->owns_lock());
	EXPECT_FALSE(second.try_lock().has_value());

	first_guard->unlock();
	EXPECT_TRUE(second.try_lock().has_value());
}

TEST_F(AsyncMutexTest, ExplicitUnlockIsIdempotent)
{
	alt::async_mutex mutex;
	lock_record      holder;
	lock_record      next;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	// Two parked lockers, because a second hand-off is only observable as a second
	// coroutine let into a critical section the first has not left.
	const auto holder_handle = alt::start(locks_and_holds(mutex, holder, 1));
	const auto next_handle   = alt::start(locking_root(mutex, next, 2));
	ASSERT_EQ(holder.resumptions, 0);

	owner->unlock();
	EXPECT_FALSE(owner->owns_lock());
	ASSERT_EQ(holder.resumptions, 1);
	ASSERT_TRUE(holder.held);
	ASSERT_EQ(next.resumptions, 0);

	// The hand-off gave the lock away rather than freeing it: the release left it
	// held throughout, so nothing could come along and take it from the waiter it was
	// already promised to.
	EXPECT_FALSE(mutex.try_lock().has_value());

	// The second call owns nothing, so it hands nothing on.
	owner->unlock();
	EXPECT_FALSE(owner->owns_lock());
	EXPECT_EQ(next.resumptions, 0);

	// And the real release still reaches the queue when the holder leaves.
	resume_parked();
	EXPECT_EQ(next.resumptions, 1);
	EXPECT_TRUE(next.satisfied);
	EXPECT_TRUE(next.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, TryLockSucceedsWhenFreeAndFailsWhenHeld)
{
	alt::async_mutex mutex;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());
	EXPECT_TRUE(owner->owns_lock());

	const auto refused = mutex.try_lock();
	ASSERT_FALSE(refused.has_value());
	EXPECT_EQ(refused.error(), busy_error);

	// Repeatedly, and without disturbing the holder.
	EXPECT_FALSE(mutex.try_lock().has_value());
	EXPECT_TRUE(owner->owns_lock());

	owner->unlock();

	auto again = mutex.try_lock();
	ASSERT_TRUE(again.has_value());
	EXPECT_TRUE(again->owns_lock());

	// A refused try_lock reports the busy error rather than the released one, so a
	// caller can tell "somebody else has it" from "your wait was cancelled".
	EXPECT_NE(busy_error, released_error);
}

TEST_F(AsyncMutexTest, AReleasedWaiterCannotEnterTheCriticalSection)
{
	// The fails-open property, and for a mutex it is the sharpest form of it: a wait
	// that ended without the lock must say so, and must leave the lock with whoever
	// had it. A released waiter that had taken the lock on its way out would hold a
	// critical section nobody is in and nothing will ever leave.
	alt::async_mutex mutex;
	lock_record      record;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	auto handle = alt::start(locking_root(mutex, record, 1));
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	ASSERT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_EQ(record.error, released_error);

	// No guard was yielded at all, rather than one owning nothing.
	EXPECT_FALSE(record.held);

	// The original holder still holds it, so the cancelled waiter took nothing.
	EXPECT_TRUE(owner->owns_lock());
	EXPECT_FALSE(mutex.try_lock().has_value());

	// And the lock is still the holder's to give: a lock the released waiter had
	// taken would stay held after this release, and this try_lock would fail.
	owner->unlock();
	EXPECT_TRUE(mutex.try_lock().has_value());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncMutexTest, CancellingAParkedWaiterReleasesIt)
{
	// The stop callback is the only thing that can reach a parked waiter: nothing in
	// the coroutine machinery runs again until someone resumes the frame.
	alt::async_mutex mutex;
	lock_record      record;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	auto handle = alt::start(locking_root(mutex, record, 1));
	ASSERT_EQ(live_frames, 1);
	ASSERT_EQ(record.resumptions, 0);

	handle.request_stop();

	EXPECT_EQ(record.resumptions, 1);
	EXPECT_FALSE(record.satisfied);
	EXPECT_FALSE(record.held);
	EXPECT_EQ(record.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncMutexTest, ACancelledWaiterIsNotResumedAgainByAnUnlock)
{
	// The waiter has gone, so a later release finds an empty queue and must not resume
	// the frame the cancellation already finished.
	alt::async_mutex mutex;
	lock_record      record;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	auto handle = alt::start(locking_root(mutex, record, 1));
	handle.request_stop();
	ASSERT_EQ(record.resumptions, 1);

	owner->unlock();

	EXPECT_EQ(record.resumptions, 1);

	// With nobody to hand it to, the lock is simply free again.
	EXPECT_TRUE(mutex.try_lock().has_value());
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncMutexTest, AWaiterCancelledBeforeItsAwaitTakesNoLock)
{
	// The other way a wait can end without the lock: the chain is abandoned on the way
	// into the suspension point, so the awaiter's ready check never runs at all. The
	// lock must be untouched afterwards.
	alt::async_mutex mutex;
	lock_record      record;

	auto handle = alt::start(parks_then_locks(mutex, record));
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	resume_parked(); // reaches the co_await, which observes the stop and destroys the frame

	EXPECT_EQ(record.resumptions, 0);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, DestroyingTheMutexReleasesParkedWaiters)
{
	// Declared ahead of the mutex, so that anything its destructor resumes is writing
	// into records that are still alive.
	lock_record first;
	lock_record second;

	{
		alt::async_mutex mutex;

		// Held without a guard, because a guard would release the lock as it died and
		// satisfy these waiters instead of leaving them for the destructor. See
		// take_the_lock_leaving_no_guard.
		ASSERT_TRUE(take_the_lock_leaving_no_guard(mutex));

		const auto first_handle  = alt::start(locking_root(mutex, first, 1));
		const auto second_handle = alt::start(locking_root(mutex, second, 2));
		ASSERT_EQ(live_frames, 2);
		ASSERT_EQ(first.resumptions, 0);
	}

	EXPECT_EQ(first.resumptions, 1);
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
	EXPECT_FALSE(first.held);
	EXPECT_FALSE(second.held);
	EXPECT_EQ(first.error, released_error);
	EXPECT_EQ(second.error, released_error);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(AsyncMutexTest, AnUnlockBetweenTheReadyCheckAndTheLinkIsTakenWithoutParking)
{
	// The window the link's condition closes, reached deterministically by driving the
	// awaiter by hand: its ready check is asked first and fails, the lock is released
	// second, and only then does it try to park. The condition is the ready check
	// again, so the second consultation is the one that takes the lock -- and a wait
	// that takes the lock there must report itself satisfied, never cancelled, which
	// is what detail::wait_awaiter::await_suspend's rule 4 guarantees and what for a
	// mutex would otherwise be a lock held by nobody forever.
	alt::async_mutex mutex;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	auto awaiter = alt::get_awaiter(mutex);
	ASSERT_FALSE(awaiter.await_ready());

	// Nothing is linked yet, so this release finds no waiter and frees the lock.
	owner->unlock();

	EXPECT_FALSE(awaiter.await_suspend(std::noop_coroutine()));

	const auto held = awaiter.await_resume();
	ASSERT_TRUE(held.has_value());
	EXPECT_TRUE(held->owns_lock());
}

TEST_F(AsyncMutexTest, AHeldMutexStillParksAtTheLink)
{
	// The converse, so the test above cannot be passed by a link that never parks.
	alt::async_mutex mutex;

	auto owner = mutex.try_lock();
	ASSERT_TRUE(owner.has_value());

	auto awaiter = alt::get_awaiter(mutex);
	ASSERT_FALSE(awaiter.await_ready());

	EXPECT_TRUE(awaiter.await_suspend(std::noop_coroutine()));

	// Released before this scope ends. The awaiter's node is linked and lives in this
	// frame, which is destroyed before the mutex is, so leaving it linked would hand
	// the mutex's destructor a dead node to walk -- and both sanitizers are blind to
	// that, because a stack slot is only poisoned once every destructor in the scope
	// has run. The release detaches and resumes it, which also shows the park was a
	// real one and that the lock went to the waiter rather than back to free.
	owner->unlock();

	// Still held, and by the waiter: a release that cleared the flag on its way to
	// handing the lock over would show up here as a lock anybody could take while its
	// new owner believed it had it.
	EXPECT_FALSE(mutex.try_lock().has_value());

	const auto held = awaiter.await_resume();
	ASSERT_TRUE(held.has_value());
	EXPECT_TRUE(held->owns_lock());
}

TEST_F(AsyncMutexTest, AHandOffChainOfManyWaitersDoesNotRecurse)
{
	// Phase 3 rejected an inline_scheduler because "the only difference would be that
	// it overflows the stack", and a hand-off that resumed the next waiter from inside
	// the guard destructor of the last one is that same defect wearing a mutex: ten
	// thousand waiters queued behind one lock would be ten thousand nested frames.
	// alt::trampoline_scheduler is the library's existing answer to the same problem,
	// and async_mutex::unlock uses the same per-thread reentrancy flag.
	//
	// All ten thousand run, and they all run at the same stack depth. The depth is
	// read from inside the critical section by note_stack_depth, because a coroutine's
	// own locals live in its heap-allocated frame and say nothing about the stack the
	// body is running on.
	int            entered = 0;
	std::uintptr_t lowest  = std::numeric_limits<std::uintptr_t>::max();
	std::uintptr_t highest = 0;

	alt::async_mutex mutex;

	{
		std::vector<alt::cancellation_handle> handles;
		handles.reserve(handoff_chain_length);

		auto owner = mutex.try_lock();
		ASSERT_TRUE(owner.has_value());

		for(int index = 0; index < handoff_chain_length; ++index)
		{
			handles.push_back(alt::start(stack_probing_locker(mutex, entered, lowest, highest)));
		}

		ASSERT_EQ(entered, 0);

		// One release drains the whole chain.
		owner->unlock();
	}

	EXPECT_EQ(entered, handoff_chain_length);
	ASSERT_LE(lowest, highest);
	EXPECT_LT(highest - lowest, stack_growth_allowance) << "the hand-off chain grew the stack by " << (highest - lowest) << " bytes over " << handoff_chain_length << " waiters";

	// And the mutex came out of it unlocked, with nothing left queued.
	EXPECT_TRUE(mutex.try_lock().has_value());
}

TEST_F(AsyncMutexTest, AQueuedLockerStillGetsInWhenAWaiterDrainsASecondMutexAfterReleasing)
{
	// The one branch of async_mutex::unlock that had no test of its own: the
	// per-thread hand-off state is saved and restored rather than simply set, so that
	// a waiter which locks and unlocks a *different* mutex inside its critical section
	// drains that one without clobbering this drain's record of its own.
	//
	// The shape that needs it is this one, and the order matters. The first waiter's
	// body runs inside the outer mutex's drain. It releases the outer lock, which is a
	// nested release and so only records that the lock came back; then it locks and
	// unlocks the inner mutex, whose unlock is a drain in its own right and clears that
	// record on the way in. Lose the restore and the outer drain reads handed_back as
	// false, breaks with the lock in transit and never handed on, and the third locker
	// below is left parked beside an m_held stuck true -- a permanent deadlock with no
	// diagnostic, since every later locker parks behind it too.
	//
	// Measured rather than argued, and deterministic, so the sample is small on
	// purpose. With the save and restore of handed_back replaced by a plain
	// `state.handed_back = false`, leaving the draining pointer's own save and restore
	// in place, this test failed in 5 of 5 runs of itself, on the four assertions
	// marked below; against the implementation as it stands, 5 of 5 passes. And under
	// that same mutation the whole suite reported exactly one failure, this one, which
	// is the measurement that says the branch had no other guard. Removing the save
	// and restore of both fields instead failed this test 5 of 5 as well, plus eleven
	// other async_mutex tests, the stale draining pointer being a second defect on top
	// of the one this test is about.

	// The records are declared before the mutexes so that anything their destructors
	// release is writing into storage that is still alive. Unreachable in a passing
	// run, but a failing one leaves the third locker parked for ~async_mutex to find.
	lock_record first;
	lock_record second;
	lock_record inner_record;

	alt::async_mutex outer;
	alt::async_mutex inner;

	auto owner = outer.try_lock();
	ASSERT_TRUE(owner.has_value());

	const auto first_handle  = alt::start(releases_then_drains_another(outer, inner, first, inner_record, 1));
	const auto second_handle = alt::start(locking_root(outer, second, 2));
	ASSERT_EQ(live_frames, 2);

	// One release, and the whole sequence above happens inside it.
	owner->unlock();

	EXPECT_TRUE(first.held);
	EXPECT_TRUE(inner_record.held);

	// The queued third locker got in. These four are the assertions the mutation
	// breaks; the inner mutex comes out free either way.
	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(outer.try_lock().has_value());

	EXPECT_TRUE(inner.try_lock().has_value());
}

TEST_F(AsyncMutexTest, AQueuedLockerStillGetsInWhenAWaiterDrainsASecondMutexBeforeReleasing)
{
	// The other order, so the save and restore is covered from both sides: the first
	// waiter drains the inner mutex while still holding the outer lock, and releases
	// the outer lock afterwards.
	//
	// This order is immune to the mutation the test above catches, and knowing that is
	// the point of having measured it rather than assumed it. The last thing this
	// waiter does is the nested release of the outer lock, which sets handed_back
	// true unconditionally, so whatever the inner drain left behind is overwritten
	// before the outer drain reads it: under the mutation described above -- the save
	// and restore of handed_back replaced by a plain `state.handed_back = false` --
	// this test passed in 5 of 5 runs.
	// What it pins is that an inner drain in this position does not disturb the outer
	// one either -- the draining pointer being restored is what keeps the release that
	// follows it recognizable as nested -- and it is the companion the other test
	// needs, not a second detector of the same defect.
	lock_record first;
	lock_record second;
	lock_record inner_record;

	alt::async_mutex outer;
	alt::async_mutex inner;

	auto owner = outer.try_lock();
	ASSERT_TRUE(owner.has_value());

	const auto first_handle  = alt::start(drains_another_then_releases(outer, inner, first, inner_record, 1));
	const auto second_handle = alt::start(locking_root(outer, second, 2));
	ASSERT_EQ(live_frames, 2);

	owner->unlock();

	EXPECT_TRUE(first.held);
	EXPECT_TRUE(inner_record.held);

	EXPECT_EQ(second.resumptions, 1);
	EXPECT_TRUE(second.held);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(outer.try_lock().has_value());
	EXPECT_TRUE(inner.try_lock().has_value());
}

TEST_F(AsyncMutexTest, AnArrivingLockerIsNeverStrandedByAConcurrentUnlock)
{
	// The lost wakeup async_mutex::unlock's one critical section exists to prevent,
	// and for a mutex it strands its victim permanently: a locker that parks in the
	// gap between a release's detach and that release clearing m_held is left
	// suspended beside a free lock, with no later release coming to wake it.
	//
	// The mechanism is a round handshake, and it is built on a measurement rather
	// than on hope. One arrival racing one release is the right shape -- both parties
	// must take the queue's one mutex, so if the detach wins it the link is the very
	// next thing to get the mutex and runs inside the gap -- but the gap itself is
	// two or three instructions wide, roughly a nanosecond, and a thread that blocked
	// on the mutex needs far longer than that to be woken. So the interleaving cannot
	// be arranged; it can only be attempted often. Each round therefore costs a
	// coroutine and two atomic handshakes rather than a thread, which buys
	// race_repeats * stranding_race_rounds attempts instead of race_repeats of them.
	//
	// The releasing thread still does the locker's own pre-link bookkeeping -- one
	// std::stop_callback -- before it releases, which is task 4's device in reverse:
	// both sides scale with the same work rather than one being calibrated against a
	// wall clock. Without it the release goes straight into its detach while the
	// locker still has a ready check and a callback registration to get through, and
	// the two miss each other systematically.
	//
	// Judging a round needs both parties to be finished, which is what the second
	// handshake is for. Once this thread's release has returned and the worker's
	// alt::start has returned, the round's outcome is settled: the waiter has either
	// been resumed -- on either thread -- or is parked, and a parked waiter with the
	// lock free is the defect.
	//
	// Measured against an unlock that detaches in one critical section and clears
	// m_held outside it: caught in 20 of 20 runs, confirmed by a second 20 runs. At 25
	// rounds a repeat it was 18 of 20, which is what set stranding_race_rounds.
	// Against the implementation as it stands, 0 of 20. Every figure is from one loop
	// over the same binary, read from its exit status.
	//
	// No repeat calls reset_coroutine_test_state(): the arriving locker is resumed on
	// whichever of the two threads wins it, so it tracks no frame, and the round
	// handshake hands over everything the assertions read.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_stranding_race(repeat));
	}
}

TEST_F(AsyncMutexTest, AStopAndAnUnlockDoNotBothResumeAndTheLockIsNotLost)
{
	// End to end, the property the claim election exists for, plus the one a mutex adds
	// to it: a release and a cancellation reaching for the same parked waiter must not
	// both resume it, and whichever loses, the lock must end up held by nobody rather
	// than held by nobody *and* still locked. The rendezvous is what makes it
	// contended: both threads block on one counter and the second arrival releases
	// both, so the detach and the stop callback's claim reach the same state word
	// together.
	//
	// Both outcomes are legitimate and the assertion covers them together. If the
	// release won the node, the waiter entered the critical section and left it, so
	// the lock is free. If the stop won, the release found nothing to hand the lock to
	// and freed it. A lock that reached neither is one no later locker could ever take.
	//
	// The per-repeat helper calls reset_coroutine_test_state() because it asserts
	// live_frames: the fixture zeroes it once in SetUp, so without the reset every
	// repeat after the first would be reading the sum of the ones before it.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_stop_versus_unlock_race(repeat));
	}
}

TEST_F(AsyncMutexTest, EveryLockerEntersTheCriticalSectionWhenArrivalsAndUnlocksContend)
{
	// Eight lockers, every one of them held at the same rendezvous until the last
	// arrives, and the lock already held by this thread so that the first thing any of
	// them can do is contend for it. Eight lockers and one lock means the outcome is
	// fixed however the eight are scheduled: each enters the critical section exactly
	// once and the lock ends free.
	//
	// The mechanism is the contention itself rather than the rendezvous alone. Eight
	// threads released together serialize their ready checks, their links and their
	// hand-off critical sections on the list's one mutex, in an order the scheduler
	// picks, so a link lands between some release's detach and its state change on
	// some iterations -- which is the interleaving a release that decides those two
	// things in separate critical sections loses a waiter to, leaving it parked
	// forever with the lock free beside it. The rendezvous sits inside each locker's
	// coroutine, after its frame is built, so that the acquisition is the next thing
	// that thread does.
	//
	// The guarded counter is deliberately a plain int rather than an atomic: it is
	// written only from inside the critical section, so it is both a count that comes
	// out wrong and a data race ThreadSanitizer reports if two coroutines are ever in
	// there at once. entered is atomic because it is the counter the test reads to
	// find a locker that never got in.
	//
	// No repeat calls reset_coroutine_test_state(): these lockers are resumed on
	// several threads at once, so they track no frame, and nothing in the loop reads
	// the state it would clear.
	constexpr std::size_t lockers = 8;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_contended_locking_race(repeat, lockers));
	}
}

TEST_F(AsyncMutexTest, NoLockIsLostWhenAStopAndAnUnlockRaceAnArrivingLocker)
{
	// The end-to-end form of the property async_mutex::mutex_awaiter::ready leans on,
	// and the shape neither earlier race has: AStopAndAnUnlockDoNotBothResumeAndTheLockIsNotLost
	// puts a stop against a waiter that is already parked, and
	// EveryLockerEntersTheCriticalSectionWhenArrivalsAndUnlocksContend puts a release
	// against lockers that are arriving but stops nothing. The defect needs both at
	// once against an arriving locker: the lock becoming free in time for the link's
	// condition to take it, and a stop landing on the same node in the same breath. A
	// waiter that took the lock there and then reported itself cancelled would leave
	// this mutex locked by nobody for the rest of the program.
	//
	// The invariant is therefore the simplest one there is, and it holds under every
	// schedule: when every thread has finished, the lock is free. How many lockers got
	// in is up to the scheduler and is deliberately not asserted, as in
	// tests/test_async_semaphore.cpp's equivalent -- a locker cancelled on its way into
	// the co_await has its frame destroyed from inside await_suspend, so the statement
	// that would have recorded anything never runs.
	//
	// Two things make the race occur rather than merely be possible.
	//
	// Each locker carries an external stop source created before it starts, so a
	// stopper thread can reach it while it is still on its way to the co_await. The
	// handle alt::start returns cannot do that: it does not exist until the coroutine
	// has already suspended.
	//
	// And the stoppers do one std::stop_callback's worth of bookkeeping after the
	// rendezvous, before they request anything, so that the request lands near the
	// moment the link's condition runs rather than before its target has looked at the
	// mutex at all. See match_an_arriving_lockers_bookkeeping for the figures that
	// shape came from.
	//
	// No repeat calls reset_coroutine_test_state(): these lockers are resumed on
	// several threads at once, so they track no frame, and nothing in the loop reads
	// the state it would clear.
	constexpr std::size_t lockers  = 4;
	constexpr std::size_t stoppers = 2;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		ASSERT_NO_FATAL_FAILURE(run_one_stop_and_unlock_versus_arrival_race(repeat, lockers, stoppers));
	}
}

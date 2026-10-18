#include <gtest/gtest.h>

#include <alt/detail/work_queue.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
	// A trivial item, so these tests exercise the queue rather than exercising
	// alt::work_item through it. The schedulers instantiate it with work_item.
	struct tagged
	{
		int tag = 0;
	};

	using queue = alt::detail::work_queue<tagged>;
}

TEST(WorkQueue, StartsEmpty)
{
	const queue q;
	EXPECT_TRUE(q.empty());
	EXPECT_EQ(q.size(), 0U);
	EXPECT_FALSE(q.is_closed());
}

TEST(WorkQueue, PushReportsTheEmptyToNonEmptyTransition)
{
	// This is what an edge-triggered wake hook is built on, so it is the one return
	// value here that a caller's correctness depends on.
	queue q;

	EXPECT_TRUE(q.push(tagged{.tag = 1}));  // empty -> non-empty
	EXPECT_FALSE(q.push(tagged{.tag = 2})); // already non-empty
	EXPECT_EQ(q.size(), 2U);

	tagged popped{};
	EXPECT_TRUE(q.try_pop(popped));
	EXPECT_TRUE(q.try_pop(popped));
	ASSERT_TRUE(q.empty());

	EXPECT_TRUE(q.push(tagged{.tag = 3})); // empty again, so a transition again
}

TEST(WorkQueue, TryPopIsFifoAndNonBlocking)
{
	queue q;
	q.push(tagged{.tag = 1});
	q.push(tagged{.tag = 2});

	tagged popped{};
	ASSERT_TRUE(q.try_pop(popped));
	EXPECT_EQ(popped.tag, 1);
	ASSERT_TRUE(q.try_pop(popped));
	EXPECT_EQ(popped.tag, 2);
	EXPECT_FALSE(q.try_pop(popped));
}

TEST(WorkQueue, PopOrWaitBlocksUntilAnItemArrives)
{
	queue q;

	std::jthread pusher{[&q] { q.push(tagged{.tag = 7}); }};

	tagged popped{};
	EXPECT_TRUE(q.pop_or_wait(popped, nullptr));
	EXPECT_EQ(popped.tag, 7);
}

TEST(WorkQueue, WakeIsConsumedByOneWaiter)
{
	// The latch semantics manual_scheduler already documents and a shipped test
	// pins. One waiter consumes it; it does not stay armed.
	queue q;

	q.wake();

	tagged popped{};
	EXPECT_FALSE(q.pop_or_wait(popped, nullptr)); // released, no item

	std::atomic<bool> second_returned{false};
	std::jthread      waiter{[&] {
    tagged ignored{};
    q.pop_or_wait(ignored, nullptr);
    second_returned.store(true, std::memory_order_release);
  }};

	// A sleep is the only way to observe "still parked": being blocked is not
	// visible from outside. The assertion can only fail in the direction that means
	// the latch was never consumed, so it will not flake red.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(second_returned.load(std::memory_order_acquire));

	q.close(); // releases the waiter so the thread can join
}

TEST(WorkQueue, TheWakeIsConsumedEvenOnThePathThatHandsOutAnItem)
{
	// The reason this header exists, pinned here rather than only through
	// manual_scheduler: the latch is consumed on *every* exit from pop_or_wait,
	// including the one that returns an item. Leaving it armed on that path would
	// make the next call return false on a stale signal, with nothing queued and no
	// wake outstanding.
	queue q;

	q.push(tagged{.tag = 1});
	q.wake(); // a queued item and a wake pending at the same moment

	tagged popped{};
	ASSERT_TRUE(q.pop_or_wait(popped, nullptr));
	EXPECT_EQ(popped.tag, 1);

	// The latch must be gone, so this waiter has to block for a real post.
	std::atomic<bool> returned{false};
	std::atomic<int>  got{0};
	std::jthread      waiter{[&] {
    tagged taken{};
    if(q.pop_or_wait(taken, nullptr))
    {
      got.store(taken.tag, std::memory_order_release);
    }
    returned.store(true, std::memory_order_release);
  }};

	// A sleep is the only way to observe "still parked", as in the test above. The
	// assertion can only fail in the direction that means the latch survived the
	// item hand-off, so it will not flake red.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(returned.load(std::memory_order_acquire));

	q.push(tagged{.tag = 2});
	waiter.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));
	EXPECT_EQ(got.load(std::memory_order_acquire), 2);
}

TEST(WorkQueue, AStopFlagReleasesAWaiterAndArmsNothing)
{
	queue             q;
	std::atomic<bool> stop{false};
	std::atomic<bool> returned{false};

	std::jthread waiter{[&] {
		tagged     ignored{};
		const bool got = q.pop_or_wait(ignored, &stop);
		returned.store(true, std::memory_order_release);
		EXPECT_FALSE(got);
	}};

	stop.store(true, std::memory_order_release);
	q.notify();
	waiter.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));

	// "notify armed nothing" is the claim, and is_closed() cannot test it: notify()
	// does not touch m_closed under any mutation, so that assertion held
	// vacuously. The property that bites is that no signal was left behind for a
	// later waiter to consume, so a following pop_or_wait still blocks.
	std::atomic<bool> second_returned{false};
	std::jthread      second{[&] {
    tagged ignored{};
    q.pop_or_wait(ignored, nullptr);
    second_returned.store(true, std::memory_order_release);
  }};

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(second_returned.load(std::memory_order_acquire));

	q.close(); // releases the waiter so the thread can join
}

TEST(WorkQueue, CloseReleasesEveryWaiterAndStaysReleased)
{
	// What a pool's shutdown needs and a consumable latch cannot give it: every
	// worker has to come back, not just one.
	queue                     q;
	std::atomic<int>          returned{0};
	std::vector<std::jthread> waiters;
	waiters.reserve(4);

	for(int i = 0; i < 4; ++i)
	{
		waiters.emplace_back([&] {
			tagged ignored{};
			while(q.pop_or_wait(ignored, nullptr))
			{
			}
			returned.fetch_add(1, std::memory_order_release);
		});
	}

	q.close();
	waiters.clear(); // joins

	EXPECT_EQ(returned.load(std::memory_order_acquire), 4);
	EXPECT_TRUE(q.is_closed());

	tagged popped{};
	EXPECT_FALSE(q.pop_or_wait(popped, nullptr)); // still released afterwards
}

TEST(WorkQueue, CloseDrainsRatherThanDiscards)
{
	// What makes a pool's destructor drain: workers keep taking items from a closed
	// queue and only come back empty-handed once it is also empty.
	queue q;

	q.push(tagged{.tag = 1});
	q.push(tagged{.tag = 2});
	q.close();

	tagged popped{};
	EXPECT_TRUE(q.pop_or_wait(popped, nullptr));
	EXPECT_TRUE(q.pop_or_wait(popped, nullptr));
	EXPECT_FALSE(q.pop_or_wait(popped, nullptr));
}

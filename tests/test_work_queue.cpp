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
	EXPECT_FALSE(q.is_closed()); // notify armed nothing
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

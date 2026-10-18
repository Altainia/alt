#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <expected>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	struct tally
	{
		std::atomic<int> runs{0};
	};

	void bump(void* state) noexcept
	{
		static_cast<tally*>(state)->runs.fetch_add(1, std::memory_order_release);
	}

	[[nodiscard]] alt::work_item item_for(tally& t) noexcept
	{
		return alt::work_item{.invoke = &bump, .state = &t};
	}

	struct reposter
	{
		alt::thread_pool* pool = nullptr;
		tally*            seen = nullptr;
		std::atomic<int>  again{1};
	};

	void repost(void* state) noexcept
	{
		auto* const r = static_cast<reposter*>(state);
		r->seen->runs.fetch_add(1, std::memory_order_release);

		if(r->again.fetch_sub(1, std::memory_order_acq_rel) > 0)
		{
			r->pool->post(alt::work_item{.invoke = &repost, .state = r});
		}
	}
}

TEST(ThreadPoolConcepts, IsASchedulerAndCannotBeMovedOutOfItsExpected)
{
	static_assert(alt::scheduler<alt::thread_pool>);
	static_assert(!alt::drivable_scheduler<alt::thread_pool>);
	static_assert(!std::is_copy_constructible_v<alt::thread_pool>);
	static_assert(!std::is_move_constructible_v<alt::thread_pool>);

	// The property that makes a non-movable pool safe inside std::expected: the
	// expected is non-movable too, so the pool cannot be moved out of the variable
	// it was created in, and references held elsewhere cannot be left dangling.
	static_assert(!std::is_move_constructible_v<std::expected<alt::thread_pool, std::error_code>>);

	// And the factory is the only way in.
	static_assert(!std::is_default_constructible_v<alt::thread_pool>);
}

TEST(ThreadPool, CreatesAtLeastOneThread)
{
	auto made = alt::thread_pool::create();
	ASSERT_TRUE(made.has_value()) << made.error().message();
	EXPECT_GE(made->thread_count(), 1U);
}

TEST(ThreadPool, AnExplicitThreadCountIsHonored)
{
	auto made = alt::thread_pool::create(3);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	EXPECT_EQ(made->thread_count(), 3U);
}

TEST(ThreadPool, RunsEveryPostedItem)
{
	// Declared before the pool: the pool's destructor joins workers that may still
	// touch it, so it must be destroyed first.
	tally seen;

	auto made = alt::thread_pool::create(4);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	for(int i = 0; i < 200; ++i)
	{
		pool.post(item_for(seen));
	}

	// Workers run concurrently with this thread, so wait on the count itself:
	// empty() says nothing about items already taken and still running.
	while(seen.runs.load(std::memory_order_acquire) < 200)
	{
		std::this_thread::yield();
	}
	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 200);
}

TEST(ThreadPool, DestructionDrainsQueuedWork)
{
	tally seen;

	{
		auto made = alt::thread_pool::create(2);
		ASSERT_TRUE(made.has_value()) << made.error().message();

		for(int i = 0; i < 100; ++i)
		{
			made->post(item_for(seen));
		}
	} // destructor closes the queue, workers finish it, joining waits for them

	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 100);
}

TEST(ThreadPool, AWorkItemMayPostToItsOwnPool)
{
	// Required, not merely allowed: a coroutine resumed on the pool posts its own
	// continuation there.
	tally seen;

	// Declared before the pool so the draining destructor runs while it is alive.
	reposter r{.seen = &seen};

	{
		auto made = alt::thread_pool::create(2);
		ASSERT_TRUE(made.has_value()) << made.error().message();

		r.pool = &*made;
		made->post(alt::work_item{.invoke = &repost, .state = &r});
	}

	// The original plus its one repost, and the destructor waited for both.
	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 2);
}

namespace
{
	struct gate_and_probe
	{
		std::atomic<bool>            started{false};
		std::atomic<bool>            release{false};
		std::atomic<int>             probe_runs{0};
		std::atomic<std::thread::id> probe_thread;
	};

	void hold_worker(void* state) noexcept
	{
		auto* const g = static_cast<gate_and_probe*>(state);
		g->started.store(true, std::memory_order_release);
		while(!g->release.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}

	void record_thread(void* state) noexcept
	{
		auto* const g = static_cast<gate_and_probe*>(state);
		g->probe_thread.store(std::this_thread::get_id(), std::memory_order_release);
		g->probe_runs.fetch_add(1, std::memory_order_release);
	}
}

TEST(ThreadPool, DrainRunsQueuedWorkOnTheCallingThread)
{
	// Deterministic: the pool's only worker is held inside a gate item, so the probe
	// item can only be run by whoever calls drain().
	gate_and_probe g;

	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();

	made->post(alt::work_item{.invoke = &hold_worker, .state = &g});
	while(!g.started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	made->post(alt::work_item{.invoke = &record_thread, .state = &g});
	EXPECT_FALSE(made->empty());

	EXPECT_EQ(made->drain(), 1U);
	EXPECT_EQ(g.probe_runs.load(std::memory_order_acquire), 1);
	EXPECT_EQ(g.probe_thread.load(std::memory_order_acquire), std::this_thread::get_id());
	EXPECT_TRUE(made->empty());

	g.release.store(true, std::memory_order_release);
}

TEST(ThreadPool, DestroyingAnIdlePoolReturns)
{
	// Workers parked on an empty queue must all be released by the close.
	auto made = alt::thread_pool::create(4);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	EXPECT_TRUE(made->empty());
}

TEST(ThreadPool, DrainOnAnEmptyPoolRunsNothing)
{
	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	EXPECT_EQ(made->drain(), 0U);
}

TEST(ThreadPool, ManyProducersAllItemsRunExactlyOnce)
{
	tally seen;

	{
		auto made = alt::thread_pool::create(4);
		ASSERT_TRUE(made.has_value()) << made.error().message();
		alt::thread_pool& pool = *made;

		std::vector<std::jthread> producers;
		producers.reserve(4);
		for(int p = 0; p < 4; ++p)
		{
			producers.emplace_back([&pool, &seen] {
				for(int i = 0; i < 250; ++i)
				{
					pool.post(item_for(seen));
				}
			});
		}
	} // producers join first (declared last), then the pool drains

	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 1000);
}

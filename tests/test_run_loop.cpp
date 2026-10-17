#include <gtest/gtest.h>

#include <alt/run_loop.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <atomic>
#include <latch>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	struct recorder
	{
		std::vector<int>* log = nullptr;
		int               tag = 0;
	};

	void record(void* state) noexcept
	{
		auto* const r = static_cast<recorder*>(state);
		r->log->push_back(r->tag);
	}

	[[nodiscard]] alt::work_item item_for(recorder& r) noexcept
	{
		return alt::work_item{.invoke = &record, .state = &r};
	}

	// Posts one more item to its own loop when it runs, which is how a greedy
	// drain would livelock a host.
	struct self_poster
	{
		alt::run_loop* loop   = nullptr;
		int*           ran    = nullptr;
		int            budget = 0;
	};

	void post_again(void* state) noexcept;

	[[nodiscard]] alt::work_item item_for(self_poster& s) noexcept
	{
		return alt::work_item{.invoke = &post_again, .state = &s};
	}

	void post_again(void* state) noexcept
	{
		auto* const s = static_cast<self_poster*>(state);
		*s->ran += 1;
		if(s->budget > 0)
		{
			s->budget -= 1;
			s->loop->post(item_for(*s));
		}
	}

	// Reposts itself while it has budget, then stops the loop. Terminating is the
	// point: an earlier draft of this test posted a separate stopper that only ran
	// once, so the loop never stopped at all.
	struct finisher
	{
		alt::run_loop* loop   = nullptr;
		int*           ran    = nullptr;
		int            budget = 0;
	};

	void repost_or_stop(void* state) noexcept;

	[[nodiscard]] alt::work_item item_for(finisher& f) noexcept
	{
		return alt::work_item{.invoke = &repost_or_stop, .state = &f};
	}

	void repost_or_stop(void* state) noexcept
	{
		auto* const f = static_cast<finisher*>(state);
		*f->ran += 1;

		if(f->budget > 0)
		{
			f->budget -= 1;
			f->loop->post(item_for(*f));
		}
		else
		{
			f->loop->stop();
		}
	}

	struct wake_counter
	{
		std::atomic<int>             calls{0};
		std::atomic<std::thread::id> last;
	};

	void count_wake(void* state) noexcept
	{
		auto* const w = static_cast<wake_counter*>(state);
		w->calls.fetch_add(1, std::memory_order_release);
		w->last.store(std::this_thread::get_id(), std::memory_order_release);
	}

	// Hops onto the loop and back, so it cannot complete without the loop running.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): loop is owned by the calling test, which drives it until this completes, so the reference cannot dangle.
	alt::task<int> hops(alt::run_loop& loop)
	{
		co_await alt::resume_on(loop);
		co_return 7;
	}
}

TEST(RunLoopConcepts, SatisfiesBothSchedulerConcepts)
{
	static_assert(alt::scheduler<alt::run_loop>);
	static_assert(alt::drivable_scheduler<alt::run_loop>);
	static_assert(!std::is_copy_constructible_v<alt::run_loop>);
	static_assert(!std::is_move_constructible_v<alt::run_loop>);
}

TEST(RunLoop, RunsNothingUntilDriven)
{
	std::vector<int> log;
	recorder         one{.log = &log, .tag = 1};
	alt::run_loop    loop;

	loop.post(item_for(one));
	EXPECT_TRUE(log.empty());
	EXPECT_FALSE(loop.empty());

	EXPECT_EQ(loop.drain(), 1U);
	EXPECT_EQ(log, std::vector<int>{1});
	EXPECT_TRUE(loop.empty());
}

TEST(RunLoop, RunReturnsImmediatelyWhenAlreadyStopped)
{
	alt::run_loop loop;
	loop.stop();
	EXPECT_TRUE(loop.stopped());
	loop.run(); // must not block
}

TEST(RunLoop, StopFromInsideAnItemEndsRunAfterThatItem)
{
	std::vector<int> log;
	alt::run_loop    loop;
	recorder         second{.log = &log, .tag = 2};

	struct stopper
	{
		alt::run_loop*    loop = nullptr;
		std::vector<int>* log  = nullptr;
	};

	stopper s{.loop = &loop, .log = &log};

	loop.post(alt::work_item{.invoke = [](void* state) noexcept {
		                         auto* const st = static_cast<stopper*>(state);
		                         st->log->push_back(1);
		                         st->loop->stop();
	                         },
	                         .state = &s});
	loop.post(item_for(second));

	loop.run();

	// The stopping item ran; the one behind it did not, and is still queued.
	EXPECT_EQ(log, std::vector<int>{1});
	EXPECT_FALSE(loop.empty());
}

TEST(RunLoop, StopDoesNotGateExecution)
{
	// The decision that makes shutdown safe: a stopped loop still runs items, so
	// the ordinary stop-then-destroy does not discard queued work.
	std::vector<int> log;
	recorder         one{.log = &log, .tag = 1};
	alt::run_loop    loop;

	loop.stop();
	loop.post(item_for(one));

	EXPECT_EQ(loop.drain(), 1U);
	EXPECT_EQ(log, std::vector<int>{1});
}

TEST(RunLoop, RunPendingIsBoundedSoASelfPostingItemCannotKeepControl)
{
	// Against a greedy drain this never returns, and a host loop freezes.
	alt::run_loop loop;
	int           ran = 0;
	self_poster   s{.loop = &loop, .ran = &ran, .budget = 100};

	loop.post(item_for(s));

	EXPECT_EQ(loop.run_pending(), 1U); // exactly the one item that was queued
	EXPECT_EQ(ran, 1);
	EXPECT_FALSE(loop.empty()); // its replacement waits for the next call

	EXPECT_EQ(loop.run_pending(), 1U);
	EXPECT_EQ(ran, 2);
}

TEST(RunLoop, RunPendingOnAnEmptyLoopReturnsZeroWithoutBlocking)
{
	alt::run_loop loop;
	EXPECT_EQ(loop.run_pending(), 0U);
}

TEST(RunLoop, DestructionDrainsQueuedWork)
{
	std::vector<int> log;
	recorder         one{.log = &log, .tag = 1};

	{
		alt::run_loop loop;
		loop.post(item_for(one));
		loop.stop(); // the ordinary shutdown, which must not discard
	}

	EXPECT_EQ(log, std::vector<int>{1});
}

TEST(RunLoop, TheWakeHookFiresOnlyOnTheEmptyToNonEmptyTransition)
{
	// Per-post delivery would mean a platform wakeup per coroutine resumption, and
	// would make a hook that posts to its own loop recurse without end. This test
	// fails against per-post delivery, where calls would be 2.
	std::vector<int> log;
	recorder         one{.log = &log, .tag = 1};
	recorder         two{.log = &log, .tag = 2};
	wake_counter     hook;

	alt::run_loop loop{alt::run_loop_options{
	  .on_wake = alt::work_item{.invoke = &count_wake, .state = &hook}}};

	loop.post(item_for(one));
	loop.post(item_for(two));

	EXPECT_EQ(hook.calls.load(std::memory_order_acquire), 1);
	EXPECT_EQ(hook.last.load(std::memory_order_acquire), std::this_thread::get_id());

	EXPECT_EQ(loop.drain(), 2U);

	loop.post(item_for(one)); // empty again, so a second transition
	EXPECT_EQ(hook.calls.load(std::memory_order_acquire), 2);
	EXPECT_EQ(loop.drain(), 1U);
}

TEST(RunLoop, StopFromAnotherThreadReleasesABlockedRun)
{
	alt::run_loop     loop;
	std::atomic<bool> returned{false};

	std::jthread runner{[&] {
		loop.run();
		returned.store(true, std::memory_order_release);
	}};

	loop.stop();
	runner.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(RunLoop, DrivesASyncWait)
{
	alt::run_loop loop;
	EXPECT_EQ(alt::sync_wait(hops(loop), loop), 7);
	EXPECT_TRUE(loop.empty());
}

TEST(RunLoop, DrivesASyncWaitEvenWhenStopped)
{
	// Follows from stop() not gating execution. Had it gated, this would either
	// spin at full CPU or deadlock, since only a work item can set the flag
	// sync_wait waits on.
	alt::run_loop loop;
	loop.stop();
	EXPECT_EQ(alt::sync_wait(hops(loop), loop), 7);
}

TEST(RunLoop, OneThreadMayDriveTheLoopFromInsideARunItem)
{
	// The legal half of the serial precondition, and the pattern a modal flow in a
	// UI framework is written with. It must use the *driving* overload: the blocking
	// one never touches the loop, so nesting it would prove nothing about the guard.
	// Here the inner sync_wait drives the same loop whose run() is below it on the
	// stack, taking the entry guard to depth two with the same owning thread.
	alt::run_loop loop;
	int           inner = 0;

	struct nested
	{
		alt::run_loop* loop  = nullptr;
		int*           inner = nullptr;
	};

	nested n{.loop = &loop, .inner = &inner};

	loop.post(alt::work_item{.invoke = [](void* state) noexcept {
		                         auto* const nn = static_cast<nested*>(state);

		                         // A work item may not throw and sync_wait can, so
		                         // driving the loop from inside one means catching.
		                         // Without this the lambda is noexcept with a throwing
		                         // call in it, and an exception from the awaited work
		                         // would terminate the process.
		                         try
		                         {
			                         *nn->inner = alt::sync_wait(hops(*nn->loop), *nn->loop);
		                         }
		                         catch(...)
		                         {
			                         *nn->inner = -1;
		                         }

		                         nn->loop->stop();
	                         },
	                         .state = &n});

	loop.run();
	EXPECT_EQ(inner, 7);
}

TEST(RunLoop, AWakeHookThatPostsToItsOwnLoopDoesNotRecurse)
{
	// The second reason the hook is edge-triggered. Under per-post delivery the
	// hook's own post fires the hook again, without end. Here its post finds the
	// queue already non-empty, so nothing fires.
	struct reposting_hook
	{
		alt::run_loop*   loop = nullptr;
		std::atomic<int> calls{0};
	};

	reposting_hook hook;

	alt::run_loop loop{alt::run_loop_options{
	  .on_wake = alt::work_item{.invoke = [](void* state) noexcept {
		                            auto* const h = static_cast<reposting_hook*>(state);
		                            h->calls.fetch_add(1, std::memory_order_release);
		                            h->loop->post(alt::work_item{.invoke = [](void*) noexcept {}, .state = nullptr});
	                            },
	                            .state = &hook}}};

	hook.loop = &loop;

	std::vector<int> log;
	recorder         one{.log = &log, .tag = 1};
	loop.post(item_for(one));

	EXPECT_EQ(hook.calls.load(std::memory_order_acquire), 1);
	EXPECT_EQ(loop.drain(), 2U); // the original plus the hook's own post
}

TEST(RunLoop, AnItemPostedDuringRunRunsInTheSameRun)
{
	// run() is not bounded, unlike run_pending(): it owns the thread and has
	// nowhere to hand control back to, so a self-posting item keeps going until it
	// stops the loop itself.
	alt::run_loop loop;
	int           ran = 0;
	finisher      f{.loop = &loop, .ran = &ran, .budget = 3};

	loop.post(item_for(f));
	loop.run();

	EXPECT_EQ(ran, 4); // the original plus its three reposts, all in one run()
}

TEST(RunLoop, RunOnOneThreadRunsWorkPostedFromAnother)
{
	// Owner mode's primary liveness property, which was covered only by composition:
	// a run() blocked on an empty queue wakes for a post from another thread. The
	// latch is declared before the loop, per the rule that what a work item points
	// at must outlive it.
	std::latch    done{3};
	alt::run_loop loop;

	std::jthread runner{[&loop] { loop.run(); }};

	for(int i = 0; i < 3; ++i)
	{
		loop.post(alt::work_item{
		  .invoke = [](void* state) noexcept { static_cast<std::latch*>(state)->count_down(); }, .state = &done});
	}

	// Deterministic without a sleep: the latch cannot reach zero unless the loop
	// thread really ran all three. If a post could not wake a blocked run(), this
	// waits forever rather than passing by luck, which is what the suite's test
	// timeout turns into a named failure.
	done.wait();

	loop.stop();
	runner.join();

	EXPECT_TRUE(loop.empty());
}

#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	alt::task<int> answer()
	{
		co_return 42;
	}
	alt::task<void> nothing()
	{
		co_return;
	}

	alt::task<std::unique_ptr<int>> owned()
	{
		co_return std::make_unique<int>(5);
	}

	alt::task<int> thrower()
	{
		throw std::runtime_error{"boom"};
		co_return 0;
	}

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): subject is owned by the calling test, which reaches sync_wait and blocks until this coroutine completes, so the reference cannot dangle.
	alt::task<std::string&> borrow(std::string& subject)
	{
		co_return subject;
	}

	// Hops onto the scheduler and back, so it cannot complete without pumping.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): scheduler is owned by the calling test, which reaches sync_wait and blocks or pumps until this coroutine completes, so the reference cannot dangle.
	alt::task<int> hops(alt::manual_scheduler& scheduler)
	{
		co_await alt::resume_on(scheduler);
		co_return 7;
	}

	// Suspends and hands the handle to a worker thread, which resumes it. The rest of the
	// chain, including the completion signal, therefore runs on the worker while the
	// calling thread is pumping inside the driving sync_wait.
	struct resumes_on_a_worker
	{
		std::jthread* worker;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}
		void await_suspend(std::coroutine_handle<> awaiting) const
		{
			*worker = std::jthread{[awaiting] {
				// A short delay makes it likely that the calling thread is already
				// blocked in run_one_or_wait when completion lands, which is the
				// ordering the driving overload has to survive.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				awaiting.resume();
			}};
		}
		void await_resume() const noexcept
		{}
	};

	alt::task<int> completes_on_a_worker(std::jthread* worker)
	{
		co_await resumes_on_a_worker{.worker = worker};
		co_return 11;
	}

	// Hops onto the scheduler first, so the calling thread runs a queued item and re-checks
	// the completion flag, and only then moves completion onto a worker thread.
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): scheduler is owned by the calling test, which pumps until this coroutine completes, so the reference cannot dangle.
	alt::task<int> hops_then_completes_on_a_worker(alt::manual_scheduler& scheduler, std::jthread* worker)
	{
		co_await alt::resume_on(scheduler);
		co_await resumes_on_a_worker{.worker = worker};
		co_return 13;
	}

	struct logged_call
	{
		std::vector<int>* log = nullptr;
		int               tag = 0;
	};

	void invoke_logged_call(void* state) noexcept
	{
		auto* const call = static_cast<logged_call*>(state);
		call->log->push_back(call->tag);
	}

	[[nodiscard]] alt::work_item item_for(logged_call& call) noexcept
	{
		return alt::work_item{.invoke = &invoke_logged_call, .state = &call};
	}

	// Asserts that no wake is left armed in scheduler: a thread entering run_one_or_wait
	// must block until a real post arrives, rather than returning on a stale signal.
	void expect_no_stale_wake(alt::manual_scheduler& scheduler, int tag)
	{
		std::vector<int>  log;
		logged_call       call{.log = &log, .tag = tag};
		std::atomic<bool> returned{false};

		std::thread waiter{[&] {
			const bool ran = scheduler.run_one_or_wait();
			returned.store(true, std::memory_order_release);
			EXPECT_TRUE(ran);
		}};

		// A stale wake would have released the waiter by now.
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		EXPECT_FALSE(returned.load(std::memory_order_acquire));

		scheduler.post(item_for(call));
		waiter.join();

		EXPECT_TRUE(returned.load(std::memory_order_acquire));
		EXPECT_EQ(log, std::vector<int>{tag});
	}

	// Holds the pumping thread inside a work item until completion has landed on a worker
	// thread. That is the one ordering in which the driving overload arms a wake with no
	// opportunity for the pump to consume it: the pump is busy rather than blocked, so it
	// returns from the item, sees the work finished and leaves the loop.
	struct pump_gate
	{
		std::latch* pump_inside_item = nullptr;
		std::latch* completion_done  = nullptr;
	};

	void invoke_pump_gate(void* state) noexcept
	{
		auto* const gate = static_cast<pump_gate*>(state);
		gate->pump_inside_item->count_down();
		gate->completion_done->wait();
	}

	// Resumes on a worker only once the pumping thread is known to be inside a work item,
	// then reports that the whole completion path, including the signal, has run.
	struct resumes_once_the_pump_is_busy
	{
		std::jthread* worker           = nullptr;
		std::latch*   pump_inside_item = nullptr;
		std::latch*   completion_done  = nullptr;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> awaiting) const
		{
			*worker = std::jthread{[awaiting, inside = pump_inside_item, done = completion_done] {
				inside->wait();
				awaiting.resume();
				done->count_down();
			}};
		}

		void await_resume() const noexcept
		{}
	};

	alt::task<int> completes_while_the_pump_runs_an_item(std::jthread* worker, std::latch* inside, std::latch* done)
	{
		co_await resumes_once_the_pump_is_busy{.worker = worker, .pump_inside_item = inside, .completion_done = done};
		co_return 17;
	}
}

TEST(SyncWait, ReturnsAValue)
{
	EXPECT_EQ(alt::sync_wait(answer()), 42);
}

TEST(SyncWait, ReturnTypeIsAValueNotAnRvalueReference)
{
	static_assert(std::is_same_v<decltype(alt::sync_wait(answer())), int>);
}

TEST(SyncWait, HandlesAVoidResult)
{
	EXPECT_NO_THROW(alt::sync_wait(nothing()));
}

TEST(SyncWait, RethrowsAnExceptionFromTheBody)
{
	EXPECT_THROW(alt::sync_wait(thrower()), std::runtime_error);
}

TEST(SyncWait, MovesAMoveOnlyResultOut)
{
	const auto taken = alt::sync_wait(owned());
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 5);
}

TEST(SyncWait, PreservesAReferenceResult)
{
	std::string subject{"hello"};
	auto&       observed = alt::sync_wait(borrow(subject));
	EXPECT_EQ(&observed, &subject);
}

TEST(SyncWait, BlockingOverloadWaitsForAnotherThread)
{
	alt::manual_scheduler scheduler;

	// A second thread pumps, so parking is correct here rather than deadlocking.
	std::jthread pump{[&] {
		while(scheduler.run_one_or_wait())
		{
		}
	}};

	EXPECT_EQ(alt::sync_wait(hops(scheduler)), 7);
	scheduler.wake();
}

TEST(SyncWait, DrivingOverloadPumpsWithoutASecondThread)
{
	// The single-threaded case. The blocking overload would deadlock here.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(hops(scheduler), scheduler), 7);
	EXPECT_TRUE(scheduler.empty());
}

TEST(SyncWait, DrivingOverloadRethrows)
{
	alt::manual_scheduler scheduler;
	EXPECT_THROW(alt::sync_wait(thrower(), scheduler), std::runtime_error);
}

TEST(SyncWait, DrivingOverloadSurvivesCompletionOnAnotherThread)
{
	// The driving overload's completion signal lives on this function's stack frame, and
	// the flag it sets is this thread's only exit condition. When completion runs on a
	// worker, that worker must not touch the signal after the store: this thread can be
	// back out of sync_wait with the frame gone by then. Run under the sanitizer preset.
	alt::manual_scheduler scheduler;
	std::jthread          worker;

	EXPECT_EQ(alt::sync_wait(completes_on_a_worker(&worker), scheduler), 11);
}

TEST(SyncWait, DrivingOverloadSurvivesAQueuedItemThenCompletionOnAnotherThread)
{
	// The same hazard on the path that actually exposed it: this thread runs a queued item,
	// returns from run_one_or_wait, re-checks the flag and may exit at exactly the moment
	// the worker publishes completion.
	alt::manual_scheduler scheduler;
	std::jthread          worker;

	EXPECT_EQ(alt::sync_wait(hops_then_completes_on_a_worker(scheduler, &worker), scheduler), 13);
}

TEST(SyncWait, DrivingOverloadLeavesNoStaleWakeAfterSynchronousCompletion)
{
	// An awaitable that never suspends completes inside start(), on this thread. If the
	// completion still armed the scheduler's wake, nothing would ever consume it and the
	// next run_one_or_wait on a scheduler the caller keeps using would return false on the
	// stale signal instead of waiting for a real post.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(answer(), scheduler), 42);
	expect_no_stale_wake(scheduler, 1);
}

TEST(SyncWait, DrivingOverloadLeavesNoStaleWakeAfterPumpedCompletion)
{
	// The same requirement when completion runs inside a work item this thread pumped:
	// run_one_or_wait has already consumed its wake before invoking the item, so a wake
	// armed by the item is left set once the loop exits.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(hops(scheduler), scheduler), 7);
	EXPECT_TRUE(scheduler.empty());
	expect_no_stale_wake(scheduler, 2);
}

TEST(SyncWait, DrivingOverloadLeavesNoStaleWakeWhenCompletionLandsDuringAWorkItem)
{
	alt::manual_scheduler scheduler;
	std::latch            pump_inside_item{1};
	std::latch            completion_done{1};
	pump_gate             gate{.pump_inside_item = &pump_inside_item, .completion_done = &completion_done};

	// Declared last so it is joined before the latches it borrows are destroyed.
	std::jthread worker;

	scheduler.post(alt::work_item{.invoke = &invoke_pump_gate, .state = &gate});

	EXPECT_EQ(alt::sync_wait(completes_while_the_pump_runs_an_item(&worker, &pump_inside_item, &completion_done), scheduler), 17);

	expect_no_stale_wake(scheduler, 17);
}

#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
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

	std::vector<int>  log;
	logged_call       call{.log = &log, .tag = 1};
	std::atomic<bool> returned{false};

	std::thread waiter{[&] {
		const bool ran = scheduler.run_one_or_wait();
		returned.store(true, std::memory_order_release);
		EXPECT_TRUE(ran);
	}};

	// A stale wake would have released the call above by now.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(returned.load(std::memory_order_acquire));

	scheduler.post(item_for(call));
	waiter.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));
	EXPECT_EQ(log, std::vector<int>{1});
}

TEST(SyncWait, DrivingOverloadLeavesNoStaleWakeAfterPumpedCompletion)
{
	// The same requirement when completion runs inside a work item this thread pumped:
	// run_one_or_wait has already consumed its wake before invoking the item, so a wake
	// armed by the item is left set once the loop exits.
	alt::manual_scheduler scheduler;
	EXPECT_EQ(alt::sync_wait(hops(scheduler), scheduler), 7);
	EXPECT_TRUE(scheduler.empty());

	std::vector<int>  log;
	logged_call       call{.log = &log, .tag = 2};
	std::atomic<bool> returned{false};

	std::thread waiter{[&] {
		const bool ran = scheduler.run_one_or_wait();
		returned.store(true, std::memory_order_release);
		EXPECT_TRUE(ran);
	}};

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(returned.load(std::memory_order_acquire));

	scheduler.post(item_for(call));
	waiter.join();

	EXPECT_TRUE(returned.load(std::memory_order_acquire));
	EXPECT_EQ(log, std::vector<int>{2});
}

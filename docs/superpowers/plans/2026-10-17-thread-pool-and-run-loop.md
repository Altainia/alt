# Thread Pool and Run Loop Implementation Plan

> **For agentic workers:** Implement this plan task-by-task, with a review between tasks. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Supply the two schedulers a program actually uses: a thread pool to do work off the main thread, and a run loop to come back to, serving both a host that owns its thread and a platform loop that owns it instead.

**Architecture:** One internal queue holds the mutex, condition variable, deque and wake protocol, and all three schedulers are built on it. `run_loop` adds blocking and non-blocking drains over that queue; `thread_pool` adds worker threads over it. The existing `manual_scheduler` is rebuilt on it so the protocol exists once rather than three times.

**Tech Stack:** C++23, GoogleTest, CMake presets. The first two compiled translation units in the library beyond `version.cpp`.

**Spec:** `docs/superpowers/specs/2026-10-17-thread-pool-and-run-loop-design.md`

**Roadmap:** `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md` (phase 3)

## Global Constraints

- **Branch:** `feature/thread-pool-and-run-loop`. Never push to `main`; work reaches it through a pull request.
- **Language:** C++23. GCC 13 or later, Clang 19 or later. Nothing outside that intersection.
- **Namespace:** public symbols in `namespace alt`, internals in `namespace alt::detail`.
- **Naming:** `snake_case` for variables, functions, types and files. Template typenames `PascalCase`. Private and protected members prefixed `m_`.
- **Const by default:** declare variables `const` unless mutation is required.
- **Doxygen:** `/** */` on every public function, class, struct and concept.
- **Spelling:** American English. The only exception is the `cancelled`, `cancelling`, `cancellable`, `cancellation` family, which is correct here.
- **Formatting:** run `clang-format -i` on every changed file before committing. Never hand-format.
- **Lint:** `tests/.clang-tidy` is appended to, never rewritten. Fix findings at the source, or add a narrowly scoped `NOLINTNEXTLINE(check-name)` whose comment states the real argument, not the check's name.
- **Work item callbacks may not throw.** The type enforces it. Neither component needs an exception policy for the work it runs.
- **Tests:** register every new test file in the `alt_tests` sources list in `tests/CMakeLists.txt`.
- **Commit messages** end with: `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`

## Verification commands

Run the first two after every task. Run all of them before the pull request.

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure

cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure

cmake --preset release-tests && cmake --build --preset release-tests
ctest --preset release-tests --output-on-failure

cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
```

**ThreadSanitizer is a gate for this phase, not a nicety.** Both components are concurrent, and AddressSanitizer does not find data races.

cppcheck matters here for the first time: it only analyzes `src/*.cpp`, so these are the first coroutine-adjacent sources it covers at all.

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `include/alt/detail/work_queue.hpp` | The queue, its mutex and condition variable, the consumable wake latch, the draining close, and the blocking pop. Nothing scheduler-specific. | 1 |
| `include/alt/scheduler.hpp` | `manual_scheduler` rebuilt on the shared queue. Public surface unchanged. | 2 |
| `include/alt/run_loop.hpp`, `src/run_loop.cpp` | The serial executor, in both its owning and guest modes. | 3 |
| `include/alt/thread_pool.hpp`, `src/thread_pool.cpp` | Worker threads over one queue, built by a factory. | 4 |
| `tests/test_work_queue.cpp` | The shared queue, directly. | 1 |
| `tests/test_run_loop.cpp` | The loop, deterministic and threaded. | 3 |
| `tests/test_thread_pool.cpp` | The pool, including the factory's error path. | 4 |
| `tests/test_scheduler_integration.cpp` | A cancellable task over the real schedulers, and cross-posting shutdown. | 5 |
| `docs/run_loop.md`, `docs/thread_pool.md` | Component pages. | 6 |

## Resolving what the design left open

Four choices the design delegated, settled here so the tasks are unambiguous:

- **The bounded drain returns a count**, not an iterator over claimed items. `run_pending` snapshots the queue size and then pops that many times individually. An iterator would need the queue to hand out a batch under its own lock, which buys nothing and complicates the only thing this header must get right.
- **`thread_pool` holds a `std::vector<std::jthread>`**, reserved to the thread count at construction. A fixed array would need the count as a template parameter, which it is not.
- **The serial precondition is asserted with an owning thread id plus a depth count**, not a bool. A bool cannot distinguish the illegal case, two threads at once, from the legal one, a single thread nesting.
- **Naming inside the queue** is as written in Task 1.

---

### Task 1: The shared work queue

**Files:**
- Create: `include/alt/detail/work_queue.hpp`
- Test: `tests/test_work_queue.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing. That is deliberate, see below.
- Produces: `alt::detail::work_queue<Item>` with `push`, `try_pop`, `pop_or_wait`, `size`, `empty`, `wake`, `notify`, `close`, `is_closed`. Tasks 2, 3 and 4 instantiate it as `work_queue<work_item>`.

**It is a template over the item type, and not gratuitously.** `work_item` lives in `include/alt/scheduler.hpp`, and `manual_scheduler` in that same header must use this queue, so a queue that included the scheduler header would be circular. Parameterizing on the item removes the dependency instead of working around it, and it lets this header's tests use a trivial item type, so they test the queue rather than testing `work_item` through it.

This header carries the protocol that already shipped one lost-wakeup defect in `manual_scheduler`. The point of extracting it is that there is one copy to get right.

Two wake signals exist and must not be conflated. `wake()` is a consumable one-shot latch: it releases one waiter, which consumes it. `close()` is permanent and non-consumable: it releases every waiter and keeps them released, and a waiter then returns empty-handed only once the queue is also empty. A pool with several worker threads needs the second; a latch consumed by one worker would leave the rest parked.

- [ ] **Step 1: Write the failing test**

Create `tests/test_work_queue.cpp`:

```cpp
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

	EXPECT_TRUE(q.push(tagged{.tag = 1}));    // empty -> non-empty
	EXPECT_FALSE(q.push(tagged{.tag = 2}));   // already non-empty
	EXPECT_EQ(q.size(), 2U);

	tagged popped{};
	EXPECT_TRUE(q.try_pop(popped));
	EXPECT_TRUE(q.try_pop(popped));
	ASSERT_TRUE(q.empty());

	EXPECT_TRUE(q.push(tagged{.tag = 3}));    // empty again, so a transition again
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
	EXPECT_FALSE(q.pop_or_wait(popped, nullptr));   // released, no item

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

	q.close();   // releases the waiter so the thread can join
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
	EXPECT_FALSE(q.is_closed());   // notify armed nothing
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
	waiters.clear();   // joins

	EXPECT_EQ(returned.load(std::memory_order_acquire), 4);
	EXPECT_TRUE(q.is_closed());

	tagged popped{};
	EXPECT_FALSE(q.pop_or_wait(popped, nullptr));   // still released afterwards
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/detail/work_queue.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/detail/work_queue.hpp`:

```cpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace alt::detail
{

	/**
	 * @brief The queue every alt scheduler is built on.
	 *
	 * Holds the items, the mutex, the condition variable and the wake protocol, so
	 * that protocol exists once rather than once per scheduler. It already shipped
	 * one lost-wakeup defect when it was written by hand; a second hand-written
	 * copy would be a second chance at the same mistake.
	 *
	 * Two wake signals exist here and they are not interchangeable:
	 *
	 * - wake() is a consumable one-shot latch. It releases one waiter, which
	 *   consumes it. This is what alt::manual_scheduler exposes.
	 * - close() is permanent. It releases every waiter and keeps them released,
	 *   and a waiter afterwards returns empty-handed only once the queue is also
	 *   empty. A pool with several worker threads needs this; a latch consumed by
	 *   one worker leaves the rest parked.
	 *
	 * close() drains rather than discards: a closed queue still hands out the items
	 * it holds, which is what lets a pool's destructor finish its queued work.
	 */
	template<typename Item>
	class work_queue
	{
	public:
		work_queue()                             = default;
		work_queue(const work_queue&)            = delete;
		work_queue& operator=(const work_queue&) = delete;
		work_queue(work_queue&&)                 = delete;
		work_queue& operator=(work_queue&&)      = delete;
		~work_queue()                            = default;

		/**
		 * @brief Queues an item.
		 *
		 * @return True when the queue went from empty to non-empty, which is what an
		 *         edge-triggered wake hook fires on. A caller's correctness can depend
		 *         on this value, so it is part of the contract rather than a hint.
		 */
		bool push(Item w)
		{
			bool was_empty = false;
			{
				const std::scoped_lock lock{m_mutex};
				was_empty = m_queue.empty();
				m_queue.push_back(w);
			}
			m_signal.notify_one();
			return was_empty;
		}

		/** Takes the oldest item without blocking. False when the queue was empty. */
		bool try_pop(Item& out)
		{
			const std::scoped_lock lock{m_mutex};
			if(m_queue.empty())
			{
				return false;
			}
			out = m_queue.front();
			m_queue.pop_front();
			return true;
		}

		/**
		 * @brief Blocks until an item is available, then takes it.
		 *
		 * Returns false without an item when the wait ended for another reason: the
		 * optional stop flag became true, wake() released it, or the queue is closed
		 * and empty. A caller that cares which can ask is_closed().
		 *
		 * The stop flag joins the wait condition rather than being signalled through
		 * this queue, so a caller waiting on something this queue knows nothing about
		 * is released without any state being left behind here.
		 */
		bool pop_or_wait(Item& out, const std::atomic<bool>* stop)
		{
			std::unique_lock lock{m_mutex};

			m_signal.wait(lock, [this, stop] {
				return !m_queue.empty() || m_woken || m_closed || (stop != nullptr && stop->load(std::memory_order_acquire));
			});

			// Consume the latch on every path. Leaving it armed after handing out an
			// item would make a later call return false on a stale signal, which is the
			// defect this header exists to retire.
			m_woken = false;

			if(m_queue.empty())
			{
				return false;
			}

			out = m_queue.front();
			m_queue.pop_front();
			return true;
		}

		/** How many items are queued. */
		[[nodiscard]] std::size_t size() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_queue.size();
		}

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_queue.empty();
		}

		/**
		 * @brief Releases one waiter, which consumes the signal.
		 *
		 * A one-shot latch: it stays armed until a waiter takes it, so a wake nobody
		 * consumes is observable by the next call. Use notify() when the condition
		 * being waited on lives outside this queue.
		 */
		void wake()
		{
			{
				const std::scoped_lock lock{m_mutex};
				m_woken = true;
			}
			m_signal.notify_all();
		}

		/**
		 * @brief Prompts waiters to re-test their condition, arming nothing.
		 *
		 * Acquiring the mutex is what closes the lost-wakeup window: a waiter that has
		 * already evaluated its condition holds the mutex until it is genuinely parked,
		 * so a notification cannot arrive before it parks and then be missed.
		 */
		void notify()
		{
			{
				const std::scoped_lock lock{m_mutex};
			}
			m_signal.notify_all();
		}

		/**
		 * @brief Permanently releases every waiter, draining rather than discarding.
		 *
		 * A closed queue still hands out the items it holds; waiters return
		 * empty-handed only once it is also empty. So a pool's workers finish the
		 * queued work and then exit, which is what makes destruction drain. Idempotent.
		 */
		void close()
		{
			{
				const std::scoped_lock lock{m_mutex};
				m_closed = true;
			}
			m_signal.notify_all();
		}

		/** True once close() has been called. */
		[[nodiscard]] bool is_closed() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_closed;
		}

	private:
		mutable std::mutex      m_mutex;
		std::condition_variable m_signal;
		std::deque<Item>        m_queue;
		bool                    m_woken  = false;
		bool                    m_closed = false;
	};

}
```

- [ ] **Step 4: Register the test file and run the tests**

Add `test_work_queue.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`, then:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R WorkQueue
```

Expected: PASS, eight tests.

- [ ] **Step 5: Run under ThreadSanitizer**

Not optional. This header is the one piece of shared mutable state the whole phase rests on.

```bash
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R WorkQueue
```

Expected: PASS with no race reports.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i include/alt/detail/work_queue.hpp tests/test_work_queue.cpp
git add include/alt/detail/work_queue.hpp tests/test_work_queue.cpp tests/CMakeLists.txt
git commit -m "Extract the queue and wake protocol every scheduler shares

One copy of the mutex, condition variable, deque and wake protocol, which
until now was written by hand in manual_scheduler and would have been written
twice more for the run loop and the pool. That protocol already shipped a
lost-wakeup defect once.

Two wake signals, deliberately distinct. wake() is the consumable one-shot
latch manual_scheduler exposes. close() is permanent, releases every waiter,
and drains rather than discards, which is what a pool with several workers
needs: a latch consumed by one worker leaves the rest parked.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Rebuild `manual_scheduler` on the shared queue

**Files:**
- Modify: `include/alt/scheduler.hpp`

**Interfaces:**
- Consumes: `alt::detail::work_queue<work_item>` from Task 1.
- Produces: no new names. `manual_scheduler`'s public surface is unchanged.

**The existing tests in `tests/test_scheduler.cpp` are the regression check and must not be weakened to accommodate this.** If a shipped test needs editing to pass, the extraction is wrong, not the test. In particular one test pins the consumable-latch semantics of `wake()` and another pins that a wake is not left armed after an item runs; both must pass untouched.

- [ ] **Step 1: Confirm the current tests pass, so a later failure means something**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R "Scheduler|Trampoline"
```

Record the count. It must be identical after the change.

- [ ] **Step 2: Replace `manual_scheduler`'s body**

In `include/alt/scheduler.hpp`, add `#include <alt/detail/work_queue.hpp>` at the top, and replace the whole of `manual_scheduler` with:

```cpp
	/**
	 * @brief A scheduler that runs nothing until it is pumped by hand.
	 *
	 * The deterministic scheduler for tests: post work, then decide exactly when it
	 * runs. Thread-safe, so a worker thread may post to a scheduler another thread
	 * pumps. For a scheduler a program would actually run on, see alt::run_loop and
	 * alt::thread_pool, which are built on the same queue.
	 */
	class manual_scheduler
	{
	public:
		/** Queues w. Safe to call from any thread. */
		void post(work_item w)
		{
			m_queue.push(w);
		}

		/** Runs the oldest queued item. Returns false when the queue was empty. */
		bool run_one()
		{
			work_item next{};
			if(!m_queue.try_pop(next))
			{
				return false;
			}
			next.invoke(next.state);
			return true;
		}

		/**
		 * @brief Blocks until an item can be run, then runs it.
		 *
		 * An optional stop flag joins the wait condition, so a caller waiting on
		 * something this scheduler knows nothing about can be released without arming
		 * any state here. That is what the driving alt::sync_wait uses, and it is why
		 * that overload leaves nothing behind in a scheduler the caller goes on using.
		 *
		 * Returns false when the wait ended for another reason: the stop flag became
		 * true, or wake() released it with nothing queued.
		 */
		bool run_one_or_wait(const std::atomic<bool>* stop = nullptr)
		{
			work_item next{};
			if(!m_queue.pop_or_wait(next, stop))
			{
				return false;
			}
			next.invoke(next.state);
			return true;
		}

		/** Runs queued items until the queue is empty. Returns how many ran. */
		std::size_t run_all()
		{
			std::size_t count = 0;
			while(run_one())
			{
				++count;
			}
			return count;
		}

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const
		{
			return m_queue.empty();
		}

		/**
		 * @brief Prompts a blocked run_one_or_wait to re-evaluate its condition.
		 *
		 * Arms nothing, so unlike wake() it cannot be observed by a later call. Use it
		 * when the condition a waiter is waiting on lives outside this scheduler,
		 * which is the case for the driving alt::sync_wait.
		 */
		void notify_waiters()
		{
			m_queue.notify();
		}

		/** Releases a thread blocked in run_one_or_wait without queueing work. */
		void wake()
		{
			m_queue.wake();
		}

	private:
		detail::work_queue<work_item> m_queue;
	};
```

- [ ] **Step 3: Remove includes that are now unused**

`manual_scheduler` no longer names `std::mutex` or `std::condition_variable` directly, so drop `<mutex>` and `<condition_variable>` from `scheduler.hpp` if nothing else in the file uses them. Keep `<deque>`: `trampoline_scheduler`'s per-thread state still uses it. Verify by compiling, not by reading.

- [ ] **Step 4: Run the shipped tests untouched**

```bash
cmake --build --preset debug
ctest --preset debug --output-on-failure -R "Scheduler|Trampoline|WorkQueue"
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure
```

Expected: the same count as Step 1, plus the queue's eight, all passing, with no edit to any existing test. The full suite must also be green, since `sync_wait`'s driving overload depends on this scheduler's wait behavior.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i include/alt/scheduler.hpp
git add include/alt/scheduler.hpp
git commit -m "Rebuild manual_scheduler on the shared queue

Its public surface is unchanged and its shipped tests pass untouched, which is
the point: those tests pin the consumable-latch semantics of wake() and the
absence of a stale wake after an item runs, and they are the regression check
for the extraction.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: `run_loop`

**Files:**
- Create: `include/alt/run_loop.hpp`, `src/run_loop.cpp`
- Test: `tests/test_run_loop.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::work_item`, `alt::detail::work_queue<work_item>`.
- Produces: `alt::run_loop_options`, `alt::run_loop` with `post`, `drain`, `run`, `stop`, `stopped`, `run_pending`, `run_one`, `run_one_or_wait`, `notify_waiters`, `empty`. Task 5 uses it; `sync_wait(awaitable, loop)` must work against it.

Three things this must get right, each of which has a test that fails if it does not:

1. **`stop()` gates `run()` and nothing else.** A stopped loop still runs items through `drain()`, `run_pending()`, `run_one()` and `run_one_or_wait()`, and its destructor still drains. Had it gated execution, the ordinary shutdown of stopping then destroying would discard every queued resumption and teardown.
2. **`run_pending()` is bounded.** It takes the queue length once. An item that posts more work must not keep control.
3. **The wake hook fires on the empty-to-non-empty transition**, not per post.

- [ ] **Step 1: Write the failing test**

Create `tests/test_run_loop.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/run_loop.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <atomic>
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
		alt::run_loop* loop  = nullptr;
		int*           ran   = nullptr;
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
		std::atomic<int>         calls{0};
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
	loop.run();   // must not block
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

	EXPECT_EQ(loop.run_pending(), 1U);   // exactly the one item that was queued
	EXPECT_EQ(ran, 1);
	EXPECT_FALSE(loop.empty());          // its replacement waits for the next call

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
		loop.stop();   // the ordinary shutdown, which must not discard
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

	loop.post(item_for(one));   // empty again, so a second transition
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

								 // A work item may not throw and sync_wait can, so driving the
								 // loop from inside one means catching. Without this the lambda
								 // is noexcept with a throwing call in it, and an exception from
								 // the awaited work would terminate the process.
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
		alt::run_loop*   loop  = nullptr;
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
	EXPECT_EQ(loop.drain(), 2U);   // the original plus the hook's own post
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

	EXPECT_EQ(ran, 4);   // the original plus its three reposts, all in one run()
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/run_loop.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/run_loop.hpp`:

```cpp
#pragma once

#include <alt/detail/work_queue.hpp>
#include <alt/scheduler.hpp>
#include <atomic>
#include <cstddef>
#include <thread>

namespace alt
{

	/** @brief Configuration for an alt::run_loop. Every field is optional. */
	struct run_loop_options
	{
		/**
		 * @brief Invoked when a post makes the queue non-empty, so a host loop can be nudged.
		 *
		 * A host supplies something that wakes its own loop: a posted Win32 message, a
		 * write to a Wayland eventfd. The loop knows nothing about platforms.
		 *
		 * It fires on the empty-to-non-empty transition, not on every post. Per-post
		 * delivery would mean a platform wakeup for every coroutine resumption, and
		 * would make a hook that posts to its own loop recurse without end.
		 *
		 * Invoked on the posting thread, outside the queue's lock. It may not throw,
		 * for the same reason a work item may not. Empty means run() waits on its own
		 * condition variable instead.
		 */
		work_item on_wake{};
	};

	/**
	 * @brief A serial executor for one thread, in either of two modes.
	 *
	 * It is an alt::scheduler, so post() works from any thread. Beyond that it serves
	 * two worlds with one type:
	 *
	 * - It owns its thread: run() drains and blocks when idle until stop().
	 * - It is a guest of a platform loop: run_pending() drains a bounded batch and
	 *   returns, called from a Win32 message handler or a Wayland dispatch.
	 *
	 * **stop() gates run() and nothing else.** A stopped loop still runs items
	 * through drain(), run_pending(), run_one() and run_one_or_wait(), and its
	 * destructor still drains. Were stop() a gate on execution, the ordinary
	 * shutdown of stopping and then destroying would silently discard every queued
	 * coroutine resumption and cancelled-chain teardown.
	 *
	 * Serial across threads, reentrant on one. Two threads must not be inside its
	 * running members at once, which is asserted in debug builds. One thread nesting
	 * them is supported, because that is how a modal flow is written: a work item
	 * that calls sync_wait(awaitable, loop) drives the loop from inside a run()
	 * already below it on the stack. Each nested level consumes stack, and a nested
	 * run_pending() computes its own batch bound.
	 *
	 * Neither copyable nor movable: it owns a thread's loop, and references to it are
	 * held by alt::scheduler_ref and by awaiters inside suspended coroutine frames.
	 */
	class run_loop
	{
	public:
		run_loop() = default;

		/** Constructs a loop with a host wake hook. */
		explicit run_loop(run_loop_options options) noexcept;

		run_loop(const run_loop&)            = delete;
		run_loop& operator=(const run_loop&) = delete;
		run_loop(run_loop&&)                 = delete;
		run_loop& operator=(run_loop&&)      = delete;

		/** Drains whatever is still queued. See the note on stop() above. */
		~run_loop();

		/** Queues an item and nudges the host if a hook was supplied. Any thread. */
		void post(work_item w);

		/** Runs queued items until the queue is empty. Returns how many ran. */
		std::size_t drain();

		/** Drains, blocking when idle, until stop(). */
		void run();

		/** Makes run() return after the current item. Sticky and idempotent. */
		void stop() noexcept;

		/** True once stop() has been called. */
		[[nodiscard]] bool stopped() const noexcept;

		/**
		 * @brief Runs a bounded batch without blocking. Returns how many ran.
		 *
		 * The queue's length is taken once on entry, so an item that posts more work
		 * cannot keep control here. Draining until empty would livelock a host loop.
		 */
		std::size_t run_pending();

		/** Runs the oldest queued item. False when the queue was empty. */
		bool run_one();

		/** Blocks until an item can be run, then runs it. See alt::manual_scheduler. */
		bool run_one_or_wait(const std::atomic<bool>* stop = nullptr);

		/** Prompts a blocked waiter to re-evaluate its condition, arming nothing. */
		void notify_waiters();

		/** True when nothing is queued. */
		[[nodiscard]] bool empty() const;

	private:
		/** Debug-only guard for the serial precondition. Permits nesting on one thread. */
		class entry_guard
		{
		public:
			explicit entry_guard(run_loop& loop) noexcept;
			entry_guard(const entry_guard&)            = delete;
			entry_guard& operator=(const entry_guard&) = delete;
			entry_guard(entry_guard&&)                 = delete;
			entry_guard& operator=(entry_guard&&)      = delete;
			~entry_guard() noexcept;

		private:
			run_loop* m_loop;
		};

		detail::work_queue<work_item> m_queue;
		run_loop_options              m_options{};
		std::atomic<bool>             m_stopped{false};
		std::atomic<std::thread::id>  m_owner{};
		std::atomic<int>              m_depth{0};
	};

}
```

- [ ] **Step 4: Write the source**

Create `src/run_loop.cpp`:

```cpp
#include <alt/run_loop.hpp>

#include <cassert>
#include <thread>

namespace alt
{

	run_loop::entry_guard::entry_guard(run_loop& loop) noexcept: m_loop(&loop)
	{
		const auto self = std::this_thread::get_id();

		if(m_loop->m_depth.load(std::memory_order_acquire) == 0)
		{
			m_loop->m_owner.store(self, std::memory_order_release);
		}
		else
		{
			// Nesting on one thread is legal and supported; two threads at once is not.
			assert(m_loop->m_owner.load(std::memory_order_acquire) == self &&
			       "two threads inside one alt::run_loop at once");
		}

		m_loop->m_depth.fetch_add(1, std::memory_order_acq_rel);
	}

	run_loop::entry_guard::~entry_guard() noexcept
	{
		if(m_loop->m_depth.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			m_loop->m_owner.store(std::thread::id{}, std::memory_order_release);
		}
	}

	run_loop::run_loop(run_loop_options options) noexcept: m_options(options)
	{}

	run_loop::~run_loop()
	{
		// Drains rather than discards: queued items may be coroutine resumptions, or a
		// cancelled chain's teardown posted here as its home scheduler, and discarding
		// either leaks frames invisibly.
		drain();
	}

	void run_loop::post(work_item w)
	{
		const bool became_non_empty = m_queue.push(w);

		if(became_non_empty && m_options.on_wake.invoke != nullptr)
		{
			m_options.on_wake.invoke(m_options.on_wake.state);
		}
	}

	std::size_t run_loop::drain()
	{
		std::size_t ran = 0;
		while(run_one())
		{
			++ran;
		}
		return ran;
	}

	void run_loop::run()
	{
		const entry_guard guard{*this};

		while(!m_stopped.load(std::memory_order_acquire))
		{
			work_item next{};
			if(m_queue.pop_or_wait(next, &m_stopped))
			{
				next.invoke(next.state);
			}
		}
	}

	void run_loop::stop() noexcept
	{
		m_stopped.store(true, std::memory_order_release);
		m_queue.notify();
	}

	bool run_loop::stopped() const noexcept
	{
		return m_stopped.load(std::memory_order_acquire);
	}

	std::size_t run_loop::run_pending()
	{
		const entry_guard guard{*this};

		// Taken once. Anything posted during the drain waits for the next call, which
		// is what stops a self-posting item from freezing a host loop.
		std::size_t remaining = m_queue.size();
		std::size_t ran       = 0;

		while(remaining > 0 && run_one())
		{
			remaining -= 1;
			ran += 1;
		}

		return ran;
	}

	bool run_loop::run_one()
	{
		work_item next{};
		if(!m_queue.try_pop(next))
		{
			return false;
		}
		next.invoke(next.state);
		return true;
	}

	bool run_loop::run_one_or_wait(const std::atomic<bool>* stop)
	{
		const entry_guard guard{*this};

		work_item next{};
		if(!m_queue.pop_or_wait(next, stop))
		{
			return false;
		}
		next.invoke(next.state);
		return true;
	}

	void run_loop::notify_waiters()
	{
		m_queue.notify();
	}

	bool run_loop::empty() const
	{
		return m_queue.empty();
	}

}
```

Note: `run_one()` deliberately takes no `entry_guard`, because `drain()` and `run_pending()` call it in a loop and would otherwise nest the guard on every item. `run_pending()` and `run_one_or_wait()` hold it instead, and `run()` holds it for its whole duration.

- [ ] **Step 5: Register the test, configure, and run**

Add `test_run_loop.cpp` to the `alt_tests` source list in `tests/CMakeLists.txt`. The new `src/run_loop.cpp` is picked up by the existing glob, so re-run configure:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R RunLoop
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R RunLoop
```

Expected: PASS, fifteen tests, under both presets. If `RunPendingIsBoundedSoASelfPostingItemCannotKeepControl` hangs, the drain is greedy.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i include/alt/run_loop.hpp src/run_loop.cpp tests/test_run_loop.cpp
git add include/alt/run_loop.hpp src/run_loop.cpp tests/test_run_loop.cpp tests/CMakeLists.txt
git commit -m "Add run_loop, a serial executor for either side of a platform loop

One type for both worlds: run() owns the thread for a host that has its own
loop, and run_pending() drains a bounded batch for a host whose platform owns
it. The bound is the queue's length taken once, so an item that posts more work
cannot keep control and freeze the host.

stop() gates run() and nothing else. A stopped loop still runs items and its
destructor still drains, because the alternative makes the ordinary
stop-then-destroy shutdown discard every queued resumption and teardown.

The wake hook fires on the empty-to-non-empty transition, which coalesces a
burst into one platform wakeup and stops a hook that posts to its own loop
recursing without end.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: `thread_pool`

**Files:**
- Create: `include/alt/thread_pool.hpp`, `src/thread_pool.cpp`
- Test: `tests/test_thread_pool.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::work_item`, `alt::detail::work_queue<work_item>`.
- Produces: `alt::thread_pool` with `create`, `post`, `drain`, `thread_count`, `empty`. Task 5 uses it.

Three things to get right:

1. **Construction goes through a factory returning `std::expected`**, because creating a thread can fail and a constructor cannot report that without throwing. The pool stays non-movable, which is compatible: the factory constructs in place and the caller binds a reference. The non-movability then propagates to the `expected`, so a pool cannot be moved out of the variable it was created in, and the dangling references a move would leave in `scheduler_ref` and in suspended frames are prevented by the type rather than by documentation.
2. **The constructor must close the queue if it throws partway.** Workers already started are parked in the queue, and `std::jthread`'s destructor joins, so without the close a partial failure hangs forever instead of returning an error.
3. **Destruction drains.** Closing the queue does not discard it: workers keep taking items and exit only once it is empty, and joining them is what makes the destructor wait for the queued work.

- [ ] **Step 1: Write the failing test**

Create `tests/test_thread_pool.cpp`:

```cpp
#include <gtest/gtest.h>

#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <expected>
#include <type_traits>

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
		alt::thread_pool* pool  = nullptr;
		tally*            seen  = nullptr;
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
	auto made = alt::thread_pool::create(4);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	tally seen;
	for(int i = 0; i < 200; ++i)
	{
		pool.post(item_for(seen));
	}

	// The destructor drains, so by the time this scope ends every item has run.
	// Asserted after destruction in the next test; here, drain explicitly.
	while(!pool.empty())
	{
		pool.drain();
	}
	EXPECT_GE(seen.runs.load(std::memory_order_acquire), 1);
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
	}   // destructor closes the queue, workers finish it, joining waits for them

	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 100);
}

TEST(ThreadPool, AWorkItemMayPostToItsOwnPool)
{
	// Required, not merely allowed: a coroutine resumed on the pool posts its own
	// continuation there.
	tally seen;

	{
		auto made = alt::thread_pool::create(2);
		ASSERT_TRUE(made.has_value()) << made.error().message();

		reposter r{.pool = &*made, .seen = &seen};
		made->post(alt::work_item{.invoke = &repost, .state = &r});
	}

	// The original plus its one repost, and the destructor waited for both.
	EXPECT_EQ(seen.runs.load(std::memory_order_acquire), 2);
}

TEST(ThreadPool, DrainRunsQueuedWorkOnTheCallingThread)
{
	// What a consumer uses to quiesce a pool and a loop that post to each other,
	// which no destructor can do on its own.
	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();

	tally seen;
	made->post(item_for(seen));

	while(seen.runs.load(std::memory_order_acquire) == 0)
	{
		made->drain();
	}
	EXPECT_GE(seen.runs.load(std::memory_order_acquire), 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL at compile time with `alt/thread_pool.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/alt/thread_pool.hpp`:

```cpp
#pragma once

#include <alt/detail/work_queue.hpp>
#include <alt/scheduler.hpp>
#include <cstddef>
#include <expected>
#include <system_error>
#include <thread>
#include <vector>

namespace alt
{

	/**
	 * @brief A fixed set of threads running work items from one queue.
	 *
	 * An alt::scheduler, so alt::resume_on hops onto it and a cancelled chain can
	 * name it as its home. Deliberately plain: one shared queue, no work stealing
	 * and no priorities. Two pools cost nothing to create, so a consumer who must
	 * keep long jobs away from latency-sensitive work makes two rather than asking
	 * one pool to arbitrate.
	 *
	 * **Construction goes through create().** Starting a thread can fail, and a
	 * constructor cannot report that without throwing, so the factory returns
	 * std::expected and the error carries the reason the OS gave.
	 *
	 * The pool is neither copyable nor movable, because it owns threads and
	 * references to it are held by alt::scheduler_ref and by awaiters inside
	 * suspended coroutine frames. That is compatible with std::expected: the factory
	 * constructs in place and the caller binds a reference, whose address is stable.
	 * The non-movability then propagates to the expected itself, so the pool cannot
	 * be moved out of the variable it was created in and those references cannot be
	 * left dangling. The price is that a pool cannot be returned from a function or
	 * stored in a container.
	 *
	 * **Destruction drains.** Queued items may be coroutine resumptions, or a
	 * cancelled chain's teardown posted here as its home scheduler, and discarding
	 * either leaks frames with no diagnostic. The cost is that shutdown takes as long
	 * as the queued work; a loud wait is better than a silent leak.
	 *
	 * The pool must outlive every suspension that targets it, which is
	 * alt::scheduler_ref's existing contract. Draining covers queued items, not
	 * references held elsewhere.
	 */
	class thread_pool
	{
	private:
		/** Makes the constructor unreachable except through create(). */
		struct passkey
		{
			explicit passkey() = default;
		};

	public:
		/**
		 * @brief Starts a pool, or reports why it could not.
		 *
		 * @param threads How many threads to start. Zero means
		 *                std::thread::hardware_concurrency(), and the result is clamped
		 *                to at least one, including when that query returns zero.
		 *
		 * A thread that fails to start fails the whole call: threads already started
		 * are released and joined, and the error is returned. A pool silently running
		 * with fewer threads than asked for is a performance mystery later, and this
		 * factory exists so that can be reported instead of guessed at.
		 */
		[[nodiscard]] static std::expected<thread_pool, std::error_code> create(std::size_t threads = 0);

		/** Called only by create(). The passkey is private, so nothing else can. */
		thread_pool(passkey, std::size_t threads);

		thread_pool()                              = delete;
		thread_pool(const thread_pool&)            = delete;
		thread_pool& operator=(const thread_pool&) = delete;
		thread_pool(thread_pool&&)                 = delete;
		thread_pool& operator=(thread_pool&&)      = delete;

		/** Runs whatever is still queued, then joins. See the note above. */
		~thread_pool();

		/** Queues an item. Safe from any thread, including a pool thread. */
		void post(work_item w);

		/**
		 * @brief Runs queued items on the calling thread. Returns how many ran.
		 *
		 * For quiescing a set of schedulers that post to each other, which no
		 * destructor can do alone: call drain() on each in turn until every one
		 * reports empty, then destroy them.
		 */
		std::size_t drain();
		// Superseded during implementation. The shipped comment says the opposite:
		// drain() is not a shutdown protocol, because a worker mid-item is invisible
		// to empty() and may still post, so drain() plus empty() cannot establish that
		// a pool is quiescent. Only destruction can, since it joins the workers.
		// Authority: docs/thread_pool.md, "Shutting down two schedulers that post to
		// each other".

		/** How many threads this pool started. Always at least one. */
		[[nodiscard]] std::size_t thread_count() const noexcept;

		/** True when nothing is queued. In-flight items are not counted. */
		[[nodiscard]] bool empty() const;

	private:
		void worker();

		detail::work_queue<work_item> m_queue;
		std::vector<std::jthread>     m_threads;
	};

}
```

- [ ] **Step 4: Write the source**

Create `src/thread_pool.cpp`:

```cpp
#include <alt/thread_pool.hpp>

#include <algorithm>
#include <utility>

namespace alt
{

	std::expected<thread_pool, std::error_code> thread_pool::create(std::size_t threads)
	{
		const std::size_t requested = threads == 0 ? std::size_t{std::thread::hardware_concurrency()} : threads;
		const std::size_t count     = std::max(std::size_t{1}, requested);

		try
		{
			// Constructed in place: the pool is not movable, so it cannot be built here
			// and moved into the expected. In place is also why the caller's reference
			// into the expected has a stable address.
			return std::expected<thread_pool, std::error_code>{std::in_place, passkey{}, count};
		}
		catch(const std::system_error& failure)
		{
			return std::unexpected(failure.code());
		}
	}

	thread_pool::thread_pool(passkey /*unused*/, std::size_t threads)
	{
		m_threads.reserve(threads);

		try
		{
			for(std::size_t started = 0; started < threads; ++started)
			{
				m_threads.emplace_back([this] { worker(); });
			}
		}
		catch(...)
		{
			// DELICATE. The workers already started are parked in the queue, and
			// std::jthread's destructor joins. Without closing the queue first, a
			// partial failure would hang forever here instead of returning an error.
			m_queue.close();
			throw;
		}
	}

	thread_pool::~thread_pool()
	{
		// A draining close, not a discarding one: workers keep taking items and come
		// back only once the queue is empty, so joining them is what makes this wait
		// for the queued work.
		m_queue.close();
	}

	void thread_pool::post(work_item w)
	{
		m_queue.push(w);
	}

	std::size_t thread_pool::drain()
	{
		std::size_t ran = 0;
		work_item   next{};

		while(m_queue.try_pop(next))
		{
			next.invoke(next.state);
			ran += 1;
		}

		return ran;
	}

	std::size_t thread_pool::thread_count() const noexcept
	{
		return m_threads.size();
	}

	bool thread_pool::empty() const
	{
		return m_queue.empty();
	}

	void thread_pool::worker()
	{
		for(;;)
		{
			work_item next{};

			if(m_queue.pop_or_wait(next, nullptr))
			{
				next.invoke(next.state);
				continue;
			}

			// Came back without an item. Only a close means there will never be another;
			// anything else is spurious and we wait again. Since a closed queue still
			// hands out what it holds, reaching here means it is closed and empty.
			if(m_queue.is_closed())
			{
				return;
			}
		}
	}

}
```

- [ ] **Step 5: Register the test, configure, and run**

Add `test_thread_pool.cpp` to the `alt_tests` source list. `src/thread_pool.cpp` is picked up by the glob, so re-run configure:

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R ThreadPool
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R ThreadPool
```

Expected: PASS, seven tests, under both presets. ThreadSanitizer matters most here: this is the first component in the library with several threads contending on one structure.

- [ ] **Step 6: Note honestly what is not tested**

Two paths cannot be forced portably and are therefore not covered by a test:

- A thread failing to start, which would need a process resource limit set from inside the test.
- `hardware_concurrency()` returning zero.

Both are covered structurally instead: the factory's return type is asserted by `static_assert`, the clamp is a single `std::max` on the line above the only use of the query, and the partial-failure cleanup is one `catch` whose comment states what hangs without it. Say this in the report rather than implying the error path is exercised.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i include/alt/thread_pool.hpp src/thread_pool.cpp tests/test_thread_pool.cpp
git add include/alt/thread_pool.hpp src/thread_pool.cpp tests/test_thread_pool.cpp tests/CMakeLists.txt
git commit -m "Add thread_pool, built by a factory returning std::expected

Starting a thread can fail and a constructor cannot report that without
throwing, so construction goes through create(). The pool stays non-movable
because references to it live in suspended coroutine frames, and that is
compatible with std::expected: the factory constructs in place and the caller
binds a reference. The non-movability propagates to the expected, so the pool
cannot be moved out of the variable it was created in and those references
cannot be left dangling.

A thread failing to start fails the whole call rather than yielding a quietly
undersized pool. The constructor closes the queue before letting the exception
out, because workers already started are parked in it and jthread's destructor
joins; without that, a partial failure hangs instead of returning an error.

Destruction drains. Closing the queue does not discard it: workers keep taking
items and exit only once it is empty.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Integration with the cancellable task

**Files:**
- Create: `tests/test_scheduler_integration.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 1 through 4, plus `alt::cancellable_task`, `alt::start`, `alt::resume_on`, and the shared helpers in `tests/coroutine_test_support.hpp`.
- Produces: nothing.

Phases 1 and 2 were tested against `manual_scheduler`, which never runs anything on its own. This task is the first evidence the cancellation machinery behaves against schedulers that really do run work on other threads. In particular the resumption check from issue #4 was verified through the test scheduler; here it has to hold through a real one.

**The technique for determinism.** A real pool runs an item the moment a thread is free, so a test cannot simply post and then request a stop. Instead, occupy the pool's only thread with an item that blocks on a latch. The coroutine's resumption then queues behind it, the stop is requested while it sits there, and releasing the latch makes the pool thread finish the blocker and take the resumption, which must observe the stop. The ordering is forced rather than hoped for.

- [ ] **Step 1: Write the failing test**

Create `tests/test_scheduler_integration.cpp`:

```cpp
#include <gtest/gtest.h>

#include "coroutine_test_support.hpp"

#include <alt/cancellable_task.hpp>
#include <alt/run_loop.hpp>
#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <latch>

using namespace alt_test;

namespace
{
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool ran_after_pool_hop = false;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): deliberately mutable test state, observed across a suspension.
	bool ran_after_loop_hop = false;

	// Occupies the pool's only thread until released, so a resumption posted behind
	// it is still queued when the stop is requested.
	struct pool_blocker
	{
		std::latch* release = nullptr;
	};

	void hold_a_pool_thread(void* state) noexcept
	{
		static_cast<pool_blocker*>(state)->release->wait();
	}

	alt::cancellable_task hops_pool_then_loop(alt::thread_pool* pool, alt::run_loop* loop)
	{
		const frame_tracker t{1};
		co_await alt::resume_on(*pool);
		ran_after_pool_hop = true;
		co_await alt::resume_on(*loop);
		ran_after_loop_hop = true;
	}
}

class SchedulerIntegrationTest: public ::testing::Test
{
protected:
	void SetUp() override
	{
		reset_coroutine_test_state();
		ran_after_pool_hop = false;
		ran_after_loop_hop = false;
	}
};

TEST_F(SchedulerIntegrationTest, AStopIsObservedWhenARealPoolResumesTheTask)
{
	// Issue #4's resumption check, through a real scheduler on a real thread rather
	// than through the hand-pumped one it was originally verified against.
	alt::run_loop loop;
	std::latch    release{1};
	pool_blocker  blocker{.release = &release};

	{
		auto made = alt::thread_pool::create(1);
		ASSERT_TRUE(made.has_value()) << made.error().message();
		alt::thread_pool& pool = *made;

		pool.post(alt::work_item{.invoke = &hold_a_pool_thread, .state = &blocker});

		auto handle = alt::start(hops_pool_then_loop(&pool, &loop));
		ASSERT_EQ(live_frames, 1);

		// The resumption is queued behind the blocker, so the stop lands first.
		handle.request_stop();
		release.count_down();
	}   // the pool's destructor drains and joins, so the teardown has happened

	EXPECT_FALSE(ran_after_pool_hop);
	EXPECT_FALSE(ran_after_loop_hop);
	EXPECT_EQ(live_frames, 0);
}

TEST_F(SchedulerIntegrationTest, AnUncancelledTaskRunsThroughBothSchedulers)
{
	// The control: without a stop, the same task hops to the pool, back to the loop,
	// and completes. If this fails, the resumption check is firing when it must not.
	alt::run_loop loop;

	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	auto handle = alt::start(hops_pool_then_loop(&pool, &loop));

	// The pool resumes the task, which posts its continuation onto the loop.
	while(!ran_after_loop_hop)
	{
		loop.run_pending();
	}

	EXPECT_TRUE(ran_after_pool_hop);
	EXPECT_TRUE(ran_after_loop_hop);
	EXPECT_FALSE(handle.stop_requested());

	while(pool.drain() + loop.drain() > 0)
	{
	}
	EXPECT_EQ(live_frames, 0);
}
// Superseded during implementation: the alternating drain is unsound for a pool
// and does not appear in the shipped test, which lets destruction establish
// quiescence instead. Authority: docs/thread_pool.md's shutdown section.

TEST_F(SchedulerIntegrationTest, QuiescingTwoCrossPostingSchedulersTearsTheChainDown)
{
	// The case no destructor can handle on its own: the chain is parked on the pool
	// with the loop as its home, so the teardown is posted from a pool thread onto
	// the loop. Alternating drain() until both are empty is the documented way out.
	//
	// Superseded during implementation: it is not. The shipped test relies on
	// destruction order instead -- the pool is destroyed first, which joins its
	// workers, and the loop is destroyed afterwards and drains what they posted.
	// Authority: docs/thread_pool.md's shutdown section.
	alt::run_loop loop;
	std::latch    release{1};
	pool_blocker  blocker{.release = &release};

	auto made = alt::thread_pool::create(1);
	ASSERT_TRUE(made.has_value()) << made.error().message();
	alt::thread_pool& pool = *made;

	pool.post(alt::work_item{.invoke = &hold_a_pool_thread, .state = &blocker});

	auto handle = alt::start(hops_pool_then_loop(&pool, &loop),
	                         {.home = alt::scheduler_ref{loop}});
	ASSERT_EQ(live_frames, 1);

	handle.request_stop();
	release.count_down();

	// The pool thread observes the stop and posts the teardown onto the loop, so it
	// is the loop's drain that frees the frame, not the pool's.
	//
	// Superseded during implementation: this spin does not appear in the shipped
	// test, because a worker mid-item is invisible to empty() and may still post,
	// so draining both in turn cannot establish quiescence. Destruction order does
	// it. Authority: docs/thread_pool.md's shutdown section.
	while(live_frames != 0)
	{
		pool.drain();
		loop.drain();
	}

	EXPECT_FALSE(ran_after_pool_hop);
	EXPECT_EQ(live_frames, 0);
	EXPECT_TRUE(loop.empty());
}
```

- [ ] **Step 2: Run to verify, then run under both sanitizers**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R SchedulerIntegration
cmake --preset debug-tsan && cmake --build --preset debug-tsan
ctest --preset debug-tsan --output-on-failure -R SchedulerIntegration
cmake --preset debug-asan && cmake --build --preset debug-asan
ctest --preset debug-asan --output-on-failure -R SchedulerIntegration
```

Expected: PASS, three tests, under all three. These destroy coroutine frames on pool threads, so a clean AddressSanitizer run is the evidence the cascade survives contact with real threading, and a clean ThreadSanitizer run is the evidence the handoff is synchronized.

If the third test loops forever, the teardown is not reaching the home scheduler.

- [ ] **Step 3: Format and commit**

```bash
clang-format -i tests/test_scheduler_integration.cpp
git add tests/test_scheduler_integration.cpp tests/CMakeLists.txt
git commit -m "Test the cancellable task against the real schedulers

Phases 1 and 2 were verified against the hand-pumped scheduler, which never
runs anything by itself. These are the first tests in which a cancellation is
observed by a resumption on another thread, a chain is torn down on a pool
thread, and a teardown is posted from a pool thread onto a loop as its home.

The interleaving is forced rather than hoped for: an item blocking on a latch
occupies the pool's only thread, so the coroutine's resumption is still queued
when the stop is requested.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Documentation

**Files:**
- Create: `docs/run_loop.md`, `docs/thread_pool.md`
- Modify: `docs/scheduler.md`, `README.md`, `docs/superpowers/specs/2026-09-10-coroutines-roadmap.md`

**Interfaces:**
- Consumes: the finished public API from Tasks 3 and 4.
- Produces: nothing code depends on.

Read `docs/scheduler.md` and `docs/cancellable_task.md` first and match their structure. As with the other coroutine pages, the hazards are the deliverable rather than an appendix.

- [ ] **Step 1: Write `docs/run_loop.md`**

Cover what it is, both modes with a worked example each, the full public API, and these hazards:

| Hazard | Why it has to be stated |
|---|---|
| `stop()` gates `run()` and nothing else | A reader will otherwise assume a stopped loop is inert, and be surprised that `drain()` still runs work. It is also why the shutdown is safe. |
| `run_pending()` is bounded | A reader who expects it to empty the queue will write a loop that never terminates in the guest case. |
| The wake hook is edge-triggered | A host that expects a nudge per post will under-wake. Say it fires on the empty-to-non-empty transition. |
| Serial across threads, reentrant on one | Two threads inside it is a precondition failure; one thread nesting is supported. Both halves matter, because a UI consumer will write the nesting case. |
| The loop must outlive every suspension targeting it | Inherited from `scheduler_ref`, but this is where a caller treats a loop as a local. |
| Driving the loop from inside a work item means catching | A work item callback may not throw and `sync_wait` can, so an exception from the awaited work would terminate the process. Found by clang-tidy on the nesting test during execution, not anticipated in the design. Show the `try`/`catch` in the nesting example. |

- [ ] **Step 2: Write `docs/thread_pool.md`**

Same structure, and these hazards:

| Hazard | Why it has to be stated |
|---|---|
| Construction can fail and returns `std::expected` | It is the whole construction API. |
| The pool cannot be moved, and neither can its `expected` | A reader will try to return one from a function. Say why: references live in suspended frames. |
| Destruction drains, so shutdown takes as long as the queued work | The alternative was a silent leak; the cost should not be a surprise. |
| No priorities; make two pools | Otherwise a reader asks one pool to arbitrate between long and short work. |
| The pool must outlive every suspension targeting it | As above. |

- [ ] **Step 3: Write the cross-posting shutdown section**

This goes on both pages, because it is the one hazard neither component can fix alone: two schedulers that post to each other cannot be shut down by destructors, since each drains only its own queue and draining one can enqueue onto the other after it is gone. Give the alternating-`drain()` loop as the remedy, and say that a cancelled chain with one as its home is exactly how the situation arises.

> **Superseded during implementation.** The alternating-`drain()` loop is not the remedy; the shipped pages warn against it. It cannot establish quiescence for a `thread_pool`, because a worker mid-item is invisible to `empty()` and may post after both queues have been seen empty. Only destruction establishes quiescence, since it closes the queue draining and then joins the workers. What both pages give as the remedy is destruction order: declare the loop before the pool, so reverse-order destruction gives pool-then-loop. Authority: `docs/thread_pool.md`, "Shutting down two schedulers that post to each other".

- [ ] **Step 4: Point the scheduler page at the real schedulers, and update the README**

`docs/scheduler.md` currently offers only a trampoline and a hand-pumped scheduler. Add a line near the top saying that `alt::run_loop` and `alt::thread_pool` are the ones a program runs on, and that all three are built on one internal queue. Add both components to the README's listing in the existing format.

- [ ] **Step 5: Mark phase 3 shipped in the roadmap**

The roadmap's phase table lists phase 3 as unbuilt and its notes say phase 3 "adds the first compiled translation unit with threads in it", which is now true rather than planned. Update both.

- [ ] **Step 6: Verify every documented signature against the headers, and compile every example**

Documentation is the one artifact the build cannot check, so check it by hand:

```bash
grep -nE "^\t\t(\[\[nodiscard\]\] )?[a-z].*\(" include/alt/run_loop.hpp include/alt/thread_pool.hpp
```

Read each documented signature against the header. Then write every example into a scratch translation unit, compile it against `include/`, and run the ones that produce output. Do not ship an example you have not compiled. Report which you compiled and the result.

- [ ] **Step 7: Commit**

```bash
git add docs/run_loop.md docs/thread_pool.md docs/scheduler.md README.md docs/superpowers/specs/2026-09-10-coroutines-roadmap.md
git commit -m "Document the run loop and the thread pool

A page each, carrying the hazards that apply. Both state the one thing neither
component can fix alone: two schedulers that post to each other cannot be shut
down by destructors, because each drains only its own queue.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Version bump

**Files:**
- Modify: `CMakeLists.txt`, `include/alt/version.hpp`, `src/version.cpp`, `tests/test_version.cpp`, `README.md`, `CLAUDE.md`

A standalone commit, never folded into a feature or merge commit.

- [ ] **Step 1: Confirm the part to increase**

Two new public components that break nothing points at a minor bump, from 1.6.0 to 1.7.0. Confirm with the author before committing; the plan does not get to decide this.

- [ ] **Step 2: Change all six files**

| File | What to change |
|------|---------------|
| `CMakeLists.txt` | `project(Alt VERSION 1.7.0 LANGUAGES CXX)` |
| `include/alt/version.hpp` | the `ALT_VERSION_MINOR` macro; the constexpr variables derive from the macros, so only the macro changes |
| `src/version.cpp` | the returned string |
| `tests/test_version.cpp` | the expected components and the string |
| `README.md` | every occurrence, including the package filename and the `find_package` example |
| `CLAUDE.md` | every occurrence, including the install example |

Note that `include/alt/version.hpp` holds the numbers as separate macros rather than as a version string, so it does not appear in a text search for the old version. It is the easy one to miss.

- [ ] **Step 3: Verify nothing was missed, and that the historical records were not touched**

```bash
grep -rn "1\.6\.0" --exclude-dir=build --exclude-dir=.git . | grep -v "docs/superpowers"
grep -rln "1\.6\.0" docs/superpowers/
```

The first must produce no output. The second must still list the documents that recorded 1.6.0 when they were written; those are historical and must not change.

- [ ] **Step 4: Build, test, commit**

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure -R Version
git add CMakeLists.txt include/alt/version.hpp src/version.cpp tests/test_version.cpp README.md CLAUDE.md
git commit -m "Bump version to 1.7.0"
```

---

## Final verification before the pull request

- [ ] **Every gate**

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure
cmake --preset debug-asan && cmake --build --preset debug-asan && ctest --preset debug-asan --output-on-failure
cmake --preset debug-tsan && cmake --build --preset debug-tsan && ctest --preset debug-tsan --output-on-failure
cmake --preset release-tests && cmake --build --preset release-tests && ctest --preset release-tests --output-on-failure
cmake -B build/tidy -DALT_CLANG_TIDY=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/tidy
cmake -B build/check -DALT_CPPCHECK=ON && cmake --build build/check --target cppcheck
```

All six must pass with zero findings. cppcheck now covers `src/run_loop.cpp` and `src/thread_pool.cpp`, the first coroutine-adjacent sources it has ever analyzed, so read its output rather than only its exit code.

- [ ] **Build with the documented minimum compilers**

CI uses GCC 13.3 and Clang 19. Build locally with `clang++-19` at minimum, with `-DALT_WARNINGS_AS_ERRORS=ON`, since warnings-as-errors is what CI enables and an optimized build surfaces warnings a debug build cannot.

- [ ] **Confirm the library still installs and consumes cleanly**

```bash
cmake -B build/local -DCMAKE_BUILD_TYPE=Release -DALT_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/alt-install
cmake --build build/local --parallel && cmake --install build/local
find /tmp/alt-install/include/alt -name '*.hpp' | sort
```

Both new headers must be present. Then compile a consumer against the installed package that creates a pool and a loop and runs a cancellable task across them, which also checks that the two new compiled translation units ended up in the installed library.

- [ ] **Open the pull request against `main`. Do not merge.**

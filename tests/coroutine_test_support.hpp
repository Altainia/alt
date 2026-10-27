#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

namespace alt_test
{

	/** Count of frame_tracker instances currently alive. */
	inline int live_frames = 0;

	/** Tags of destroyed frame_tracker instances, in destruction order. */
	inline std::vector<int> destruction_order;

	/** Tracks a coroutine frame's lifetime for the tests in this suite. */
	struct frame_tracker
	{
		explicit frame_tracker(int tag): m_tag(tag)
		{
			++live_frames;
		}

		~frame_tracker()
		{
			--live_frames;
			destruction_order.push_back(m_tag);
		}

		frame_tracker(const frame_tracker&)            = delete;
		frame_tracker& operator=(const frame_tracker&) = delete;
		frame_tracker(frame_tracker&&)                 = delete;
		frame_tracker& operator=(frame_tracker&&)      = delete;

	private:
		int m_tag;
	};

	/** The handle most recently parked by one of the park* awaiters below. */
	inline std::coroutine_handle<> parked{};

	/** Suspends and hands its handle to the test, standing in for off-thread work. */
	struct park
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
		}

		void await_resume() const noexcept
		{}
	};

	/**
	 * An inner awaiter whose await_suspend returns a handle, covering the
	 * symmetric-transfer path where the wrapper must return a no-op handle.
	 */
	struct park_by_transfer
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
			return std::noop_coroutine();
		}

		void await_resume() const noexcept
		{}
	};

	/**
	 * An inner awaiter whose await_suspend returns bool, covering the path where
	 * the wrapper must return true rather than false.
	 */
	struct park_by_bool
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		bool await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
			return true;
		}

		void await_resume() const noexcept
		{}
	};

	/**
	 * An inner awaiter that is already complete: await_ready returns true and
	 * await_suspend should never run in an uncancelled coroutine. Exercises the
	 * wrapper's await_ready cancellation branch, which must still force a
	 * suspension even though the inner awaiter has nothing left to wait for.
	 */
	struct already_ready
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*h*/) const noexcept
		{}

		void await_resume() const noexcept
		{}
	};

	/** A stand-in promise type used only to type a coroutine_handle in tests below. Never instantiated as a real coroutine. */
	// A real coroutine whose promise type is unrelated to any awaiting coroutine's, so
	// an awaiter can hand back a genuinely typed handle that is also safe to resume.
	// A null handle will not do: [expr.await] has the compiler resume whatever
	// await_suspend returns, and resuming a null handle is undefined behavior, which
	// GCC diagnoses as a potential null dereference once optimizations are on.
	inline int typed_transfers_resumed = 0;

	struct unrelated_coroutine
	{
		struct promise_type
		{
			unrelated_coroutine get_return_object() noexcept
			{
				return unrelated_coroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
			}

			std::suspend_always initial_suspend() noexcept
			{
				return {};
			}

			// Self-destroying, so resuming the handle runs the body and frees the frame.
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
		};

		std::coroutine_handle<promise_type> handle{};
	};

	inline unrelated_coroutine make_unrelated_coroutine()
	{
		++typed_transfers_resumed;
		co_return;
	}

	/**
	 * An inner awaiter whose await_suspend declares a genuinely typed handle
	 * return type, std::coroutine_handle<unrelated_promise>, rather than the
	 * erased std::coroutine_handle<>. [expr.await] permits an awaiter to return
	 * a handle naming any coroutine's promise, not necessarily the awaiting
	 * coroutine's own, so this covers that case. The handle it returns is a real
	 * suspended coroutine, because the compiler resumes whatever comes back.
	 */
	struct park_by_typed_transfer
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		std::coroutine_handle<unrelated_coroutine::promise_type> await_suspend(std::coroutine_handle<> h) const
		{
			parked = h;

			// A live, suspended coroutine of an unrelated promise type. Resuming it runs
			// its body and frees its frame, so the transfer the compiler performs here is
			// real rather than a null dereference waiting to happen.
			return make_unrelated_coroutine().handle;
		}

		void await_resume() const noexcept
		{}
	};

	/** Resumes the most recently parked handle, clearing it first. */
	inline void resume_parked()
	{
		const auto handle = parked;
		parked            = {};
		handle.resume();
	}

	/** Spins before a waiting party starts yielding. See rendezvous below. */
	inline constexpr int spin_limit = 100000;

	/**
	 * @brief Waits until @c released parties have arrived, spinning.
	 *
	 * A rendezvous that leaves both sides running within nanoseconds of each other,
	 * which is what makes a race between them genuinely contended. A condition
	 * variable or a sleep would hand the race to whichever side woke first and the
	 * window under test would never be hit. The yield is only a backstop for a
	 * machine with fewer cores than threads, where an unbounded spin could starve
	 * the party being waited for.
	 */
	inline void rendezvous(std::atomic<std::size_t>& arrived, std::size_t released)
	{
		arrived.fetch_add(1, std::memory_order_acq_rel);

		for(int spins = 0; arrived.load(std::memory_order_acquire) < released;)
		{
			if(spins < spin_limit)
			{
				++spins;
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	/** Resets all shared test state. Call this from each fixture's SetUp. */
	inline void reset_coroutine_test_state()
	{
		typed_transfers_resumed = 0;
		live_frames             = 0;
		destruction_order.clear();
		parked = {};
	}

} // namespace alt_test

#pragma once

#include <coroutine>
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
	struct unrelated_promise
	{};

	/**
	 * An inner awaiter whose await_suspend declares a genuinely typed handle
	 * return type, std::coroutine_handle<unrelated_promise>, rather than the
	 * erased std::coroutine_handle<>. [expr.await] permits an awaiter to return
	 * a handle naming any coroutine's promise, not necessarily the awaiting
	 * coroutine's own, so this covers that case. The returned handle is always
	 * the null default of that type; nothing in these tests ever resumes it.
	 */
	struct park_by_typed_transfer
	{
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		std::coroutine_handle<unrelated_promise> await_suspend(std::coroutine_handle<> h) const noexcept
		{
			parked = h;
			return {};
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

	/** Resets all shared test state. Call this from each fixture's SetUp. */
	inline void reset_coroutine_test_state()
	{
		live_frames = 0;
		destruction_order.clear();
		parked = {};
	}

} // namespace alt_test

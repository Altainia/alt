#pragma once

#include <alt/cancellable_task.hpp>
#include <alt/detail/completion_counter.hpp>
#include <cstddef>
#include <stop_token>

namespace alt
{

	/**
	 * @brief The owner's teardown point for a set of spawned alt::cancellable_task.
	 *
	 * Owns the one stop signal every task spawned through it shares, and counts the
	 * tasks still alive, so that "everything I started is gone" has an answer. The
	 * teardown is three steps: request_stop(), co_await join(), then destroy the
	 * primitives the tasks were using and the group itself. It is finite because the
	 * stop releases a task parked on a primitive through the task's own stop
	 * callback, not through the primitive's destruction, so nothing waits on a
	 * primitive that is itself waiting on the join.
	 *
	 * Every spawned task is told the group's token and the group's completion hook.
	 * Those two alt::start_options fields belong to the group. home and on_exception
	 * pass through untouched.
	 *
	 * The group does not own the tasks' frames, which own themselves. Its destructor
	 * therefore neither stops nor waits, and asserts in debug builds that nothing is
	 * left: destroying a group with live tasks leaves their completion hooks pointing
	 * at freed memory. Blocking in the destructor instead would deadlock whenever a
	 * task needs the destroying thread to make progress.
	 *
	 * What this does not promise: a spawn racing a join that completes may land after
	 * it. An owner that is tearing down stops spawning first.
	 *
	 * Every member but the destructor is safe to call from any thread. Neither
	 * copyable nor movable, as the hooks inside the tasks hold its address.
	 */
	class task_group
	{
	public:
		/** What co_await group.join() is awaited through. Yields alt::wait_result. */
		using join_awaiter = detail::completion_counter::join_awaiter;

		/** Constructs an empty group with no stop requested. */
		task_group();

		/**
		 * @brief Asserts that no task is still alive, and does nothing else.
		 *
		 * Request a stop and join the group first. See the class documentation for why
		 * this neither stops nor waits.
		 */
		~task_group();

		task_group(const task_group&)            = delete;
		task_group& operator=(const task_group&) = delete;
		task_group(task_group&&)                 = delete;
		task_group& operator=(task_group&&)      = delete;

		/**
		 * @brief Starts a task as a member of this group and returns its own handle.
		 *
		 * The task runs immediately, up to its first suspension point, exactly as with
		 * alt::start. It stops when the group does, and also when the returned handle
		 * does, which cancels only that task.
		 *
		 * Not [[nodiscard]], unlike alt::start: the group is the handle an owner keeps,
		 * so dropping the per-task one is the common case.
		 *
		 * @p options must leave token and on_completion empty, which debug builds
		 * assert, because the group supplies both. A caller with an external signal
		 * registers a std::stop_callback on it that calls request_stop() on this
		 * group. A caller wanting a notification when one task finishes uses an
		 * alt::scope_exit local in the task's body, which runs on both the normal and
		 * the cancelled path.
		 */
		cancellation_handle spawn(cancellable_task t, const start_options& options = {});

		/**
		 * @brief Asks every task in the group to stop.
		 *
		 * Returns immediately: the tasks stop at their next suspension point, which for
		 * a task parked on a primitive is now. Returns true when this call was the one
		 * that requested it. A task spawned after this stops at its first suspension.
		 */
		bool request_stop() noexcept;

		/** True once a stop has been requested on the group. */
		[[nodiscard]] bool stop_requested() const noexcept;

		/** How many spawned tasks are still alive. A snapshot: it may be stale on return. */
		[[nodiscard]] std::size_t outstanding() const noexcept;

		/**
		 * @brief Awaits every task spawned so far being gone.
		 *
		 * Yields alt::wait_result: a value when the count reached zero (or was already
		 * there, which costs no suspension), and std::errc::operation_canceled when the
		 * awaiting chain was cancelled. A cancelled join leaves the group unjoined,
		 * so join from somewhere the cancellation does not reach.
		 *
		 * Defined here rather than in src/task_group.cpp because it returns a type that
		 * cannot be spelled without detail/completion_counter.hpp, which this header
		 * includes anyway to hold the counter by value, and awaiter construction lives
		 * in the header for the same reason in alt::async_event and alt::async_mutex.
		 */
		[[nodiscard]] join_awaiter join() noexcept
		{
			return m_counter.join();
		}

	private:
		/** The completion hook every spawned task carries. @p state is the group. */
		static void on_task_complete(void* state) noexcept;

		// Declared counter first, source second, but nothing depends on it. Both are
		// referenced only by live tasks, and a group destroyed with live tasks has
		// already violated the destructor's precondition, so no order here repairs or
		// worsens that. Recorded so the next reader does not re-derive it.
		detail::completion_counter m_counter;
		std::stop_source           m_source;
	};

}

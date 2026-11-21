// Compile check for issue #61, built by the task_await_discard_* tests in
// tests/CMakeLists.txt rather than linked into alt_tests: what it pins is a diagnostic,
// which no running test can observe.
//
// Built twice. With ALT_PROBE_DISCARD defined, a plain coroutine discards the value of an
// awaited alt::task<int>, and the build must fail, because alt::task's await_resume is
// [[nodiscard]] and the probe makes exactly that warning an error. Without it the same
// await is cast to void, and the build must succeed: that control is what makes the first
// failure mean "the discard warned" rather than "this file does not compile".
//
// The coroutine is deliberately not an alt::task. Inside one, every await is wrapped by
// detail::cancellation_checked, which carries its own [[nodiscard]]; a plain coroutine
// reaches the task's own awaiter, which is the case #61 closed.
#include <alt/task.hpp>
#include <coroutine>

namespace
{
	/// A minimal coroutine type with no await_transform, so co_await reaches the task's awaiter.
	struct plain_coroutine
	{
		struct promise_type
		{
			plain_coroutine get_return_object() noexcept
			{
				return {};
			}
			std::suspend_never initial_suspend() noexcept
			{
				return {};
			}
			std::suspend_never final_suspend() noexcept
			{
				return {};
			}
			void return_void() noexcept
			{}
			void unhandled_exception() noexcept
			{}
		};
	};

	alt::task<int> a_value()
	{
		co_return 1;
	}

	alt::task<void> nothing()
	{
		co_return;
	}
} // namespace

plain_coroutine probe_task_await_discard();

plain_coroutine probe_task_await_discard()
{
	// Awaiting a task<void> must never warn: there is no value to discard. Both builds
	// include it, so the control also fails if the attribute ever reaches the void case.
	co_await nothing();
#ifdef ALT_PROBE_DISCARD
	co_await a_value();
#else
	static_cast<void>(co_await a_value());
#endif
}

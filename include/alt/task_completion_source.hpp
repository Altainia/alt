#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/coroutine.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cassert>
#include <exception>
#include <expected>
#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace alt::detail
{

	/**
	 * What a task_completion_source<T> yields on success. T itself, except that a
	 * reference becomes a std::reference_wrapper: std::expected<T&, E> is ill-formed
	 * (libstdc++ rejects it with a static_assert that the value type is not a
	 * reference), so a reference source cannot hand back the reference directly.
	 */
	template<typename T>
	struct completion_value
	{
		using type = T;
	};

	/** @copydoc completion_value */
	template<typename T>
	struct completion_value<T&>
	{
		using type = std::reference_wrapper<T>;
	};

}

namespace alt
{

	/**
	 * @brief What awaiting a task_completion_source<T> yields.
	 *
	 * The value on success, or std::errc::operation_canceled when the wait ended
	 * without the source being set: the awaiting chain was cancelled, or the source
	 * was destroyed with the waiter still parked. For T = void this is exactly
	 * alt::wait_result, and for T = U& the value is a std::reference_wrapper<U>,
	 * because std::expected cannot hold a reference.
	 *
	 * An exception passed to set_exception is *not* carried here. It is rethrown by
	 * the co_await itself.
	 */
	template<typename T>
	using completion_result = std::expected<typename detail::completion_value<T>::type, std::error_code>;

	/**
	 * @brief The bridge from a callback-based API to a co_await.
	 *
	 * A foreign thread calls set_value() or set_exception(), and the coroutine
	 * awaiting the source is resumed with that result. Set once and awaited once.
	 * Awaiting a source that is already set does not suspend.
	 *
	 * Awaiting yields alt::completion_result<T>. Two kinds of failure are kept apart
	 * on purpose. An exception the caller supplied through set_exception() is
	 * rethrown from the co_await, like any other exception a callee throws. The
	 * condition alt itself reports, std::errc::operation_canceled, travels in the
	 * returned std::expected. That is a second convention beside
	 * alt::as_expected's std::expected<T, std::exception_ptr>, and as_expected is
	 * not meant to be composed over this type.
	 *
	 * T may be void, a reference, or a value type with a noexcept move constructor.
	 * A reference source yields a std::reference_wrapper, since std::expected cannot
	 * hold a reference, and the referenced object must outlive the await.
	 *
	 * Setting is safe from any thread. Destruction is not, as ever, and a source
	 * must not be destroyed while a cancellation is in flight against its waiter.
	 * The waiter resumes on the thread that set the source, inside set_value() or
	 * set_exception(), which therefore run its body; co_await
	 * alt::resume_on(scheduler) in a waiter that needs to be somewhere specific.
	 *
	 * Neither copyable nor movable: a foreign callback holds a pointer to the
	 * source, and so does the awaiter inside a suspended coroutine frame. The owner
	 * keeps it alive and in place.
	 *
	 * A source that is never set, never cancelled and never destroyed parks its
	 * waiter forever. Setting it, cancelling the awaiting chain and destroying the
	 * source are the three ways out.
	 *
	 * Setting twice, and awaiting twice, are preconditions: each asserts in a debug
	 * build. A second set is a no-op in a release build.
	 *
	 * The awaited value stays in the source until the awaiting coroutine reads it.
	 * Resumption is synchronous inside the setter and await_resume runs before the
	 * resumed body continues, so the source is alive at every read; the two other
	 * ways out of a wait, the stop callback and the destructor, read nothing. That
	 * relies on the family's rule that a primitive outlives its own resumption walk.
	 * A waiter may destroy the source from its body once it has read the value.
	 *
	 * @tparam T The value delivered: void by default, a value type, or a reference.
	 */
	template<typename T = void>
	class task_completion_source
	{
	public:
		/** The T this source was declared with. */
		using value_type = T;

		/** Constructs an unset source. */
		task_completion_source() noexcept = default;

		/**
		 * @brief Releases a waiter still parked, unsatisfied.
		 *
		 * A backstop rather than the expected path, as async_event's destructor is,
		 * and done here for the same reason it is there: ahead of the waiter_list
		 * member's own destructor, so a waiter that awaits again from inside the walk
		 * parks in a list that is still whole.
		 */
		~task_completion_source()
		{
			detail::resume_claimed(m_waiters.detach_all(), false);
		}

		task_completion_source(const task_completion_source&)            = delete;
		task_completion_source& operator=(const task_completion_source&) = delete;
		task_completion_source(task_completion_source&&)                 = delete;
		task_completion_source& operator=(task_completion_source&&)      = delete;

		/**
		 * @brief Completes the source with a value, resuming the waiter.
		 *
		 * For a value type T. The value is constructed before anything is published,
		 * so a throwing conversion leaves the source unset; moving it into the source
		 * is then required not to throw, because it happens inside the waiter list's
		 * critical section. Setting twice asserts in debug and is a no-op in release.
		 *
		 * The waiter's body runs on this thread before this returns, so this must not
		 * be called with a lock held that the waiter might take.
		 */
		template<typename U>
		  requires(!std::is_void_v<T> && !std::is_reference_v<T> && std::is_constructible_v<T, U &&>)
		void set_value(U&& value)
		{
			static_assert(std::is_nothrow_move_constructible_v<T>, "task_completion_source<T> moves T inside a critical section, so it must not throw");

			T staged(std::forward<U>(value));

			// The store's set_value is not declared noexcept because it forwards an
			// arbitrary U, but this call hands it a T&& it can only move-construct from,
			// which the static_assert above has just promised does not throw.
			// NOLINTNEXTLINE(bugprone-exception-escape): moving a nothrow-move-constructible T into the store cannot throw.
			publish([this, &staged]() noexcept { m_result.set_value(std::move(staged)); });
		}

		/**
		 * @brief Completes a void source, resuming the waiter.
		 *
		 * Setting twice asserts in debug and is a no-op in release. The waiter's body
		 * runs on this thread before this returns.
		 */
		void set_value()
		  requires std::is_void_v<T>
		{
			publish([this]() noexcept { m_result.set_value(); });
		}

		/**
		 * @brief Completes a reference source with the caller's object, resuming the
		 *        waiter.
		 *
		 * The object must outlive the await. Setting twice asserts in debug and is a
		 * no-op in release. The waiter's body runs on this thread before this returns.
		 */
		template<typename U = T>
		  requires std::is_reference_v<U>
		void set_value(std::type_identity_t<U> value)
		{
			publish([this, &value]() noexcept { m_result.set_value(value); });
		}

		/**
		 * @brief Completes the source with an exception, resuming the waiter.
		 *
		 * The waiter's co_await rethrows it. Setting twice asserts in debug and is a
		 * no-op in release. The waiter's body runs on this thread before this returns.
		 */
		void set_exception(std::exception_ptr error)
		{
			publish([this, &error]() noexcept { m_result.set_exception(std::move(error)); });
		}

		/** True once either channel has been filled. */
		[[nodiscard]] bool is_set() const noexcept
		{
			return m_set.load(std::memory_order_acquire);
		}

		/**
		 * @brief One suspended waiter on a task_completion_source.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's
		 * frame. detail::wait_awaiter holds everything but the two questions this
		 * answers and the value a satisfied wait is worth.
		 */
		class completion_awaiter: public detail::wait_awaiter<completion_awaiter>
		{
		public:
			/** The source must outlive the suspension. */
			explicit completion_awaiter(task_completion_source& source) noexcept: m_source(&source)
			{}

			/**
			 * Whether the wait is already over. Also the condition the link is made
			 * under, with the list's mutex held, so it reads one atomic: no lock of its
			 * own, no call back into the list, nothing resumed. See
			 * detail::wait_awaiter for the whole of that contract.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_source->is_set();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_source->m_waiters;
			}

			/**
			 * @brief The outcome: the value, or the released error.
			 *
			 * Hides detail::wait_awaiter's own await_resume rather than extending it,
			 * because this wait yields a value where the other primitives' yield
			 * nothing, exactly as async_mutex's does. The base still decides *whether*
			 * the wait succeeded; this only decides what success is worth.
			 *
			 * Rethrows the exception a set_exception() supplied. Not noexcept for that
			 * reason.
			 */
			[[nodiscard]] completion_result<T> await_resume() const
			{
				const wait_result result = detail::wait_awaiter<completion_awaiter>::await_resume();

				if(!result)
				{
					return completion_result<T>{std::unexpect, result.error()};
				}

				return m_source->take_result();
			}

		private:
			task_completion_source* m_source;
		};

		/** Awaits the source, yielding its value or the released error. */
		[[nodiscard]] completion_awaiter operator co_await() noexcept
		{
			[[maybe_unused]] const bool already_awaited = m_awaited.exchange(true, std::memory_order_relaxed);
			assert(!already_awaited && "awaiting an alt::task_completion_source twice");

			return completion_awaiter{*this};
		}

	private:
		/**
		 * Fills the store and detaches the waiter in one critical section, then
		 * resumes it. Marking the source set and taking the queue are one step for the
		 * reason async_event::set gives: a waiter either links before this section and
		 * is detached by it, or links after and finds the source set and does not park.
		 *
		 * The double-set check is inside that same section, so two threads setting at
		 * once cannot both pass it.
		 */
		template<typename Fill>
		void publish(Fill fill) noexcept
		{
			// noexcept spelled out on the action, as the rest of the family does: an
			// action that threw would terminate.
			detail::waiter_node* const chain = m_waiters.detach_one([this, &fill](bool /*found*/) noexcept {
				assert(!m_result.is_set() && "setting an alt::task_completion_source twice");

				if(!m_result.is_set())
				{
					fill();
					m_set.store(true, std::memory_order_release);
				}
			});

			// The resumption is outside the mutex, and last, because it runs the waiter's
			// body, which may destroy this source outright. Nothing may be read from
			// *this after this call.
			detail::resume_claimed(chain, true);
		}

		/** Reads the stored result in the shape completion_result<T> has. */
		[[nodiscard]] completion_result<T> take_result()
		{
			if constexpr(std::is_void_v<T>)
			{
				// Called for its rethrow; there is no value to return.
				m_result.value();
				return {};
			}
			else if constexpr(std::is_reference_v<T>)
			{
				return std::reference_wrapper<std::remove_reference_t<T>>{m_result.value()};
			}
			else
			{
				// The only shape whose store has an rvalue value(), so the only one that
				// moves rather than copies.
				return std::move(m_result).value();
			}
		}

		/**
		 * Read without the list's mutex by a waiter deciding whether to wait at all,
		 * and under it as the condition on the link. Stored last, after the result is
		 * written, so a reader that sees it set sees the result.
		 */
		std::atomic<bool> m_set{false};

		/**
		 * Exists only for the await-twice precondition asserted in operator co_await.
		 * Atomic because two threads may await. Unconditional rather than wrapped in
		 * #ifndef NDEBUG because a data member of a header-only template that exists in
		 * some translation units and not others makes the class layout differ between
		 * them, an ODR violation, in a library that ships headers and a .deb. One
		 * uncontended atomic exchange per await is the price of avoiding that.
		 */
		std::atomic<bool> m_awaited{false};

		// Declared before m_waiters so the store outlives the list's own destructor
		// walk. Deliberate but not load-bearing: a waiter that walk releases yields the
		// released error and reads no store, so nothing depends on this order today.
		// Recorded so that nobody re-derives it, or "fixes" it, on a hunch.
		detail::result_store<T> m_result;

		detail::waiter_list m_waiters;
	};

}

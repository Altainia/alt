#pragma once

#include <alt/detail/cancellation.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <expected>
#include <optional>
#include <stop_token>
#include <system_error>

namespace alt
{

	/**
	 * @brief What an alt async wait reports: satisfied, or released before it was.
	 *
	 * The result of awaiting an alt::async_event, an alt::async_latch or an
	 * alt::async_semaphore. It holds a value when the primitive satisfied the wait,
	 * and an error when the wait ended without that: the awaiting chain was
	 * cancelled, or the primitive was destroyed with the waiter still parked.
	 *
	 * That error is always std::errc::operation_canceled, whichever primitive
	 * reported it and whichever of the two reasons it was, and the way to name it is
	 * the standard library's own idiom -- compare the std::error_code against the
	 * condition rather than build a second code to match it:
	 *
	 *     const alt::wait_result result = co_await event;
	 *
	 *     if(!result && result.error() == std::errc::operation_canceled)
	 *     {
	 *         // released rather than satisfied
	 *     }
	 *
	 * Since that is the only error any of these waits can report, a bare @c !result
	 * is usually the whole of the check; the comparison is for a caller that has
	 * folded several kinds of failure into one error_code of its own.
	 *
	 * alt::async_mutex does not yield this type, being the one primitive in the
	 * family whose satisfied wait has something to hand back: it yields
	 * std::expected<alt::async_mutex_lock, std::error_code>. The error half is the
	 * same error, named the same way.
	 *
	 * Declared in <alt/detail/async_wait.hpp>, which every one of those primitives'
	 * headers includes, so including the primitive is enough to name this.
	 */
	using wait_result = std::expected<void, std::error_code>;

}

namespace alt::detail
{

	/** A promise that relays an alt cancellation context to its children. */
	template<typename Promise>
	concept relays_cancellation = requires(Promise& p) {
		{ p.cancellation_context_pointer() } -> std::same_as<cancellation_context*>;
	};

	/**
	 * The error a released wait reports. One function, so every primitive in the
	 * library releases its waiters with the same code.
	 *
	 * Internal, and deliberately not the public way to name that error: a caller
	 * compares against std::errc::operation_canceled instead, which is what
	 * alt::wait_result documents and what the standard library's own error_code
	 * comparisons look like. This exists so there is one place the code is built.
	 */
	[[nodiscard]] inline std::error_code wait_released_error() noexcept
	{
		return std::make_error_code(std::errc::operation_canceled);
	}

	/**
	 * @brief Releases one parked waiter when its chain's cancellation is requested.
	 *
	 * The body of the std::stop_callback a wait_awaiter registers. It holds raw
	 * pointers into the primitive and into the waiting coroutine's own frame, both
	 * of which outlive it: the callback is destroyed with the awaiter, ahead of the
	 * node it points at, and std::stop_callback's destructor waits for an
	 * invocation already running on another thread.
	 *
	 * Resuming only what it unlinked is the whole of its contract. A node is
	 * winnable before its waiter has linked it, so this callback can fire while the
	 * waiter is still inside its own await_suspend; resuming there would run a
	 * coroutine that has not finished suspending. Winning without a detach instead
	 * leaves the resumption to that frame, whose link is refused for exactly this
	 * reason. See waiter_list::claim and wait_awaiter::await_suspend.
	 */
	struct waiter_stop_release
	{
		/** The queue the waiter is parked in, or is about to park in. */
		waiter_list* list;

		/** The waiter's node, which lives in its coroutine frame. */
		waiter_node* node;

		void operator()() const noexcept
		{
			if(list->claim(*node) == claim_result::detached)
			{
				resume_claimed(node, false);
			}
		}
	};

	/**
	 * @brief Awaiter base for a primitive that parks a waiter in a waiter_list.
	 *
	 * Holds the node, the stop callback and the protocol that ties the two
	 * together, so that alt::async_event and the primitives after it describe only
	 * when they are satisfied. Derived supplies two members:
	 *
	 * - `bool ready() const` — whether the wait is already over, so no suspension is
	 *   needed. Also consulted a second time under the list's mutex, as the condition
	 *   on the link, so it must take no lock of its own, must not call back into the
	 *   list, and must resume nothing. That middle clause is the one that bites: a
	 *   fair semaphore's natural `m_count > 0 && list().empty()` self-deadlocks on the
	 *   non-recursive mutex link() is already holding.
	 *
	 *   It is consulted at most twice per wait, and the second call happens only if
	 *   the first returned false, so a condition that consumes something — a
	 *   semaphore permit, a mutex — succeeds at most once per wait and may consume on
	 *   either call. Whichever of the two calls returns true, the wait reports
	 *   satisfied; a consuming condition is never told it was cancelled after it has
	 *   already consumed, which is what await_suspend's rule 4 exists to guarantee.
	 * - `waiter_list& list()` — the queue to park in.
	 *
	 * Every wait reports alt::wait_result: a value when the primitive satisfied it,
	 * and std::errc::operation_canceled when it was released without that happening,
	 * by a cancellation or by the primitive being destroyed. A derived awaiter whose
	 * satisfied wait has something to hand back -- alt::async_mutex, with its guard --
	 * hides this await_resume rather than extending it, and still takes the verdict
	 * from the one below.
	 *
	 * Exactly one party resumes a waiter, on every path. Which one is settled by
	 * waiter_list's claim election rather than by anything here.
	 *
	 * Two preconditions this cannot enforce, both inherited from waiter_list:
	 *
	 * - A parked waiter's coroutine frame must not be destroyed from outside, because
	 *   the node is in that frame and nothing here withdraws it from the list. A
	 *   primitive releases its waiters when it is destroyed, and a cancellation
	 *   releases them through the stop callback; those are the two supported ways out
	 *   of a wait.
	 * - The primitive must outlive every claim on its waiters. A stop callback that
	 *   has won the election but not yet taken the list's mutex is reaching for a list
	 *   that must still be there.
	 */
	template<typename Derived>
	class wait_awaiter
	{
	public:
		/** True when the primitive is already satisfied, so the wait costs no suspension. */
		[[nodiscard]] bool await_ready() const
		{
			return derived().ready();
		}

		/**
		 * @brief Parks the waiter, or hands the frame straight back to itself.
		 *
		 * DELICATE. Read this before changing it.
		 *
		 * The coroutine is already suspended when this runs, so every statement here
		 * is one a competing thread may be racing. Three rules, in order:
		 *
		 * 1. **The stop callback is registered before the node is linked.** Not after:
		 *    the instant the node is on the list another party may claim it, resume
		 *    this frame and destroy it, so constructing a member afterwards writes to
		 *    freed storage. This is the same last-statement discipline resume_on and
		 *    checked_resume_on carry, with the link as the publish.
		 *
		 * 2. **Registering first is safe only because of what the callback does.**
		 *    std::stop_callback invokes its callback immediately on construction when
		 *    the token has already been stopped, and that callback then finds a node
		 *    on no list. It must resume nothing there, because this frame has not
		 *    finished suspending; it wins the node instead, which makes the link below
		 *    refuse, and the frame resumes itself. waiter_stop_release and
		 *    waiter_list::claim are where that is enforced. The window is real rather
		 *    than theoretical: cancellation_checked already abandons the chain when a
		 *    stop is pending on the way into a suspension point, but a stop landing
		 *    between that check and this registration still has to be caught, and the
		 *    immediate-fire behavior of std::stop_callback is what catches it.
		 *
		 * 3. **The link is the last statement that touches *this**, and after a
		 *    *successful* one nothing may be read at all: the node is on the list, so
		 *    any party may win it, resume this frame and destroy it. After a *failed*
		 *    link the frame has no resumer but itself, because the node is on no list
		 *    and nothing can reach it, so *this is safe to touch again -- but only the
		 *    frame's own members, never the node.
		 *
		 * 4. **Which of the two refusals happened is decided inside the condition, not
		 *    read off the node afterwards.** The node's state word cannot answer it
		 *    once the mutex is dropped, and an earlier version of this function that
		 *    asked it there was wrong. A condition that refuses the link because the
		 *    primitive is satisfied, followed by a stop callback winning the node a
		 *    moment later, leaves the state word reading @c claimed with nothing
		 *    resumed -- indistinguishable, from here, from a stop that won the node
		 *    before the condition ever ran. Reported as a cancellation, that is a wait
		 *    that took what the primitive gave it and then said it got nothing: for
		 *    alt::async_semaphore a permit consumed and never returned, and for a
		 *    future alt::async_mutex a lock held by nobody. The condition knows which
		 *    case it is, so it says so, under the mutex, where the answer is still
		 *    true.
		 *
		 *    Both refusals cannot happen at once: link() tests the election first and
		 *    does not run the condition at all if it has been lost. So the flag staying
		 *    false means the election refused the link, which is the released case.
		 *
		 *    Guarded by AsyncSemaphoreTest.AConsumingPredicateThatWinsAtTheLinkIsNeverReportedCancelled,
		 *    which drives this function by hand with a consuming predicate that claims
		 *    its own node from inside the condition, and by
		 *    AsyncSemaphoreTest.AWaiterWhoseNodeIsWonBeforeTheLinkIsStillReportedCancelled
		 *    for the converse, so neither answer can be hardwired. They live in
		 *    tests/test_async_semaphore.cpp because a consuming predicate is what
		 *    exposes this at all, not because they are about the semaphore.
		 *
		 * The node must also outlive every claim on it, which is why m_stop is
		 * declared after m_node: it is destroyed first, so no callback can be
		 * mid-flight over a dead node. cancellable_task's promise records having once
		 * carried the same requirement and having removed it structurally, by giving
		 * detail::stop_forwarder a reference-counted copy instead of a pointer into
		 * the frame; read that note before relying on declaration order anywhere else.
		 * Here the callback really does point into the frame, so the order is
		 * load-bearing and nothing in the language checks it.
		 *
		 * @return False when the wait is already over and the coroutine should carry
		 *         straight on, which is this function's only way to say so.
		 */
		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h)
		{
			m_node.handle = h;

			waiter_list& waiters = derived().list();

			if constexpr(relays_cancellation<Promise>)
			{
				cancellation_context* const context = h.promise().cancellation_context_pointer();

				// A null context is a coroutine that relays cancellation but has no root
				// above it, and an empty token can never be stopped. Neither can release
				// this wait, so neither gets a callback.
				if(context != nullptr && context->token.stop_possible())
				{
					m_stop.emplace(context->token, waiter_stop_release{.list = &waiters, .node = &m_node});
				}
			}

			// Written here rather than after the link, which may not be followed by a
			// read of *this at all. See rule 3 above. What makes this plain store safe
			// to read on another thread is the list's mutex: see m_parked.
			m_parked = true;

			// Set by the condition itself, under the list's mutex, so that the two
			// reasons a link can be refused are told apart while they can still be told
			// apart. See rule 4 above; this is a local rather than a member because
			// nothing reads it after this function returns.
			bool satisfied_at_the_link = false;

			const auto still_waiting = [this, &satisfied_at_the_link] {
				if(derived().ready())
				{
					satisfied_at_the_link = true;
					return false;
				}

				return true;
			};

			if(waiters.link(m_node, still_waiting))
			{
				return true;
			}

			// Not linked, so nothing but this frame can resume it, and the frame's own
			// members are safe to touch again. The node is not: a stop callback may be
			// mid-flight over it, and whatever it finds there it resumes nothing, having
			// won a node that is on no list.
			m_parked   = false;
			m_released = !satisfied_at_the_link;
			return false;
		}

		/** The outcome: satisfied, or released with std::errc::operation_canceled. */
		[[nodiscard]] wait_result await_resume() const noexcept
		{
			// A waiter that parked takes its verdict from whoever resumed it; one that
			// never parked already knows its own.
			const bool satisfied = m_parked ? m_node.satisfied : !m_released;

			if(!satisfied)
			{
				return wait_result{std::unexpect, wait_released_error()};
			}

			return wait_result{};
		}

		wait_awaiter(const wait_awaiter&)            = delete;
		wait_awaiter& operator=(const wait_awaiter&) = delete;
		wait_awaiter& operator=(wait_awaiter&&)      = delete;

	private:
		// Everything that constructs or destroys this base is private, so the only
		// thing that can derive from it is the Derived it was named with. A protected
		// constructor would let any class inherit from wait_awaiter<SomeoneElse> and
		// have its awaiter cast to that unrelated type.
		friend Derived;

		/** Constructs a waiter that is on no list and watching no stop state. */
		wait_awaiter() noexcept = default;

		/**
		 * @brief Moves a waiter that has not suspended yet. The only kind there is.
		 *
		 * Needed because detail::cancellation_checked stores the awaiter it wraps by
		 * value, so an awaiter returned from operator co_await is moved once before
		 * any of its await members run. Nothing is carried across, deliberately: a
		 * fresh waiter holds no state, all of it being created in await_suspend, and
		 * neither the node's atomic nor a live std::stop_callback could be moved
		 * anyway. A future field that is set before await_suspend must be carried here
		 * by hand, which the assert is there to catch.
		 */
		wait_awaiter([[maybe_unused]] wait_awaiter&& other) noexcept
		{
			assert(!other.m_node.handle && !other.m_stop.has_value() && !other.m_parked && "moving an alt wait_awaiter that has already suspended");
		}

		/** Destroys the stop callback ahead of the node. See await_suspend. */
		~wait_awaiter() = default;

		[[nodiscard]] Derived& derived() noexcept
		{
			return static_cast<Derived&>(*this);
		}

		[[nodiscard]] const Derived& derived() const noexcept
		{
			return static_cast<const Derived&>(*this);
		}

		/** This waiter's place in the primitive's queue. */
		waiter_node m_node;

		/**
		 * Declared after m_node so that it is destroyed first. A callback already
		 * running on another thread claims that node, and std::stop_callback's
		 * destructor is what waits for it; reverse these two and the wait would be
		 * over storage that has already gone.
		 */
		std::optional<std::stop_callback<waiter_stop_release>> m_stop;

		/**
		 * Whether the node was linked, and so whether its resumer wrote the verdict.
		 *
		 * A plain bool, read in await_resume on whichever thread resumed this waiter,
		 * which is not necessarily the thread that wrote it. Rule 3 says why the write
		 * precedes the link; what makes the read safe elsewhere is the list's mutex.
		 * The store is sequenced before link() acquires that mutex, and every party
		 * that can resume this node -- a satisfier's detach, or a stop callback's claim
		 * -- acquires the same mutex before it touches the node at all, so it reads this
		 * field's final value, and the resumption it then performs is on its own thread.
		 * There is no path by which a waiter is resumed without its resumer having gone
		 * through that mutex first, which is the same argument waiter_list::try_claim
		 * makes for the node's handle and links.
		 */
		bool m_parked = false;

		/** Whether a stop callback won this waiter before it could park. */
		bool m_released = false;
	};

}

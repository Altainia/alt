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

namespace alt::detail
{

	/** A promise that relays an alt cancellation context to its children. */
	template<typename Promise>
	concept relays_cancellation = requires(Promise& p) {
		{ p.cancellation_context_pointer() } -> std::same_as<cancellation_context*>;
	};

	/** What every alt async wait reports: satisfied, or released before it was. */
	using wait_result = std::expected<void, std::error_code>;

	/**
	 * The error a released wait reports. One function, so every primitive in the
	 * library releases its waiters with the same code and a caller can compare
	 * against it without knowing which primitive it came from.
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
	 *   needed. Also consulted a second time under the list's mutex, as the
	 *   condition on the link, so it must take no lock and resume nothing.
	 * - `waiter_list& list()` — the queue to park in.
	 *
	 * Every wait reports std::expected<void, std::error_code>: a value when the
	 * primitive satisfied it, and std::errc::operation_canceled when it was
	 * released without that happening, by a cancellation or by the primitive being
	 * destroyed.
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
		 * 3. **The link is the last statement that touches *this.** Everything
		 *    await_resume reads is written before it. After a *failed* link the frame
		 *    has no resumer but itself, because the node is on no list and nothing can
		 *    reach it, so reading the node's own state there is safe; after a
		 *    successful one, nothing may be read at all.
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
			// read of *this at all. See rule 3 above.
			m_parked = true;

			if(waiters.link(m_node, [this] { return !derived().ready(); }))
			{
				return true;
			}

			// Not linked, so nothing but this frame can resume it, and *this is safe to
			// touch again. The election decides which of the two refusals happened: a
			// node that is still linked was refused by the condition, meaning the
			// primitive was satisfied between await_ready and here, and anything else
			// means a stop callback won this waiter and left the resumption to it.
			m_parked   = false;
			m_released = m_node.state.load(std::memory_order_acquire) != waiter_state::linked;
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

		/** Whether the node was linked, and so whether its resumer wrote the verdict. */
		bool m_parked = false;

		/** Whether a stop callback won this waiter before it could park. */
		bool m_released = false;
	};

}

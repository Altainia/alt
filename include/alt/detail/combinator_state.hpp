#pragma once

#include <alt/detail/completion_counter.hpp>
#include <atomic>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <utility>

namespace alt::detail
{

	class combinator_state;

	/**
	 * @brief The stop callback that forwards a linked token's stop into a combinator_state.
	 *
	 * Holds a pointer to the enclosing block rather than a reference-counted copy of
	 * anything, which is safe only because combinator_state declares the callback's
	 * owner, m_link, as its last member. See the note there. Contrast
	 * detail::stop_forwarder, which holds a copy and so does not depend on order.
	 */
	struct combinator_stop_forwarder
	{
		combinator_state* target;

		/** Cancels the block this points at. Defined once combinator_state is complete. */
		void operator()() const noexcept;
	};

	/**
	 * @brief The reference-counted block a combinator and its children share.
	 *
	 * When the chain awaiting a combinator is cancelled it must be free to die at
	 * once, without waiting for its children. The children are self-owning coroutines
	 * that may still be running, so the result slots' owner, the completion count and
	 * the children's stop source cannot live in the dying frame. They live here.
	 *
	 * The block is reference counted. It starts with one reference, the awaiter's,
	 * and the party fanning the children out calls add_ref() once per child before
	 * starting that child. The last release() calls the erased destroy function, which
	 * owns whatever frees the block: a function pointer and a pointer rather than a
	 * virtual destructor, the shape alt::work_item and alt::completion_handler use.
	 * That keeps this a concrete class and lets a typed owner hold it by composition.
	 *
	 * Neither copyable nor movable: the children refer to it.
	 *
	 * Every member is safe to call from any thread except link(), which must be
	 * called at most once and before the block is shared with another thread.
	 */
	class combinator_state
	{
	public:
		/**
		 * @brief Constructs the block with one reference and @p children + 1 outstanding.
		 *
		 * The extra outstanding item is the starter's: the party fanning the children
		 * out holds it until every child has been started, so that no child completing
		 * synchronously during the fan-out can drive the count to zero. This is
		 * task_group::spawn's "raise the count before the task starts" rule generalized
		 * to N starts.
		 *
		 * What that buys is **defensive in stage A, not load-bearing**, and this is a
		 * measurement rather than an argument. alt::when_all parks on the counter strictly
		 * after its fan-out, so during the fan-out no joiner exists for a zero to resume,
		 * and join_awaiter::ready() is re-read under the waiter list's mutex, so a count
		 * that reached zero early merely refuses the later link. Moving the starter's
		 * retirement above when_all's child-start loop therefore fails nothing:
		 * 0/200 in debug, 0/200 under ThreadSanitizer, and 0 across the whole WhenAllTest
		 * suite. Deleting the retirement altogether does fail, 20/20, so what is guarded
		 * is that the starter is retired, not where. An earlier version of this comment
		 * claimed the position protected a parent "that has not finished suspending";
		 * three passes over this file asserted that before anyone measured it.
		 *
		 * The count stays, for stage B rather than for today: a combinator that parks
		 * before its fan-out ends -- which when_any may -- would need it, and removing it
		 * would change this constructor's semantics for no gain in stage A.
		 *
		 * @param children The number of children that will each retire one item.
		 * @param owner    Passed back to @p destroy when the last reference is released.
		 * @param destroy  Frees whatever owns this block. It must not throw.
		 */
		combinator_state(std::size_t children, void* owner, void (*destroy)(void* owner) noexcept) noexcept:
		  m_owner(owner),
		  m_destroy(destroy)
		{
			m_counter.add(children + 1);
		}

		/**
		 * @brief Destroys the block, normally by way of the erased destroy function.
		 *
		 * Blocks until a cancel() forwarded from a linked token on another thread has
		 * returned. It does not wait for one running on the calling thread, and the last
		 * reference IS released from inside cancel() on the ordinary cancellation path;
		 * what makes that survivable is the counted copy cancel() holds. See the notes
		 * there and on m_link.
		 */
		~combinator_state() = default;

		combinator_state(const combinator_state&)            = delete;
		combinator_state& operator=(const combinator_state&) = delete;
		combinator_state(combinator_state&&)                 = delete;
		combinator_state& operator=(combinator_state&&)      = delete;

		/** Takes one more reference. Call it before the child that will release it starts. */
		void add_ref() noexcept
		{
			m_references.fetch_add(1, std::memory_order_relaxed);
		}

		/**
		 * @brief Drops one reference, running the erased destroy function on the last.
		 *
		 * Nothing may touch this block after the call that may have been the last.
		 */
		void release() noexcept
		{
			if(m_references.fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				// Copied out before the call: m_destroy frees *this.
				const auto  destroy = m_destroy;
				void* const owner   = m_owner;
				destroy(owner);
			}
		}

		/** The count of children (plus the starter) not yet retired. */
		[[nodiscard]] completion_counter& counter() noexcept
		{
			return m_counter;
		}

		/** The token the children observe. Stopped by cancel(). */
		[[nodiscard]] std::stop_token children_token() const noexcept
		{
			return m_children.get_token();
		}

		/**
		 * @brief Marks the wait released and stops the children. Idempotent.
		 *
		 * Runs on whichever thread requests the stop. Safe against this block being
		 * freed by the very stop it requests, which is the ordinary case rather than an
		 * exotic one; see the DELICATE note in the body.
		 */
		void cancel() noexcept
		{
			// The flag before the stop, and nothing may reorder them. A cancelled child
			// still fires its completion hook, so the children's stop can drive the
			// counter to zero and resume the awaiting chain; if that chain read the flag
			// as false it would report a satisfied wait and assemble results from slots
			// no child body ever filled.
			//
			// What guards the order is this class's own test, not a combinator's.
			// Swapping these two statements fails
			// CombinatorStateTest.CancelMarksTheWaitReleasedBeforeStoppingTheChildren
			// 200/200, and fails nothing at all in alt::when_all -- 0/200 against every
			// cancellation test in that suite, 33/33 of them still passing. The reason
			// is in when_all_awaiter's await_resume note: the chain's own stop claims
			// the joiner before cancel() ever runs, so the flag is never the deciding
			// half of that verdict today. It is still stored first, because a
			// combinator that parks before its fan-out ends would make it the deciding
			// half.
			m_cancelled.store(true, std::memory_order_release);

			// DELICATE. The copy is not a style choice. Read this before removing it.
			//
			// The stop below frees this block on the ordinary cancellation path: the
			// children it stops tear their frames down on this thread, inside this call,
			// and each one's completion hook releases its reference as it goes. When the
			// awaiting chain has already dropped the awaiter's reference -- which it has,
			// the chain being released before the children are stopped -- the last child's
			// release is the last reference, and the erased destroy function runs here,
			// with request_stop() still on the stack.
			//
			// A counted copy makes that survivable. The standard library reference counts
			// the stop state, so request_stop() runs on state this local keeps alive even
			// after m_children itself is gone, and nothing in this function touches
			// *this afterwards. This is detail::stop_forwarder's reason for holding a
			// std::stop_source rather than a pointer into the promise, and the same trick
			// at a different level.
			//
			// Measured rather than argued: without the copy,
			// WhenAllTest.TheChildrensStopOutlivesABlockTheLastChildFreed reports
			// heap-use-after-free under AddressSanitizer, reading the freed state's lock
			// word in _Stop_state_t::_M_lock() on the way out of the walk below. The
			// simpler cancellation tests do not expose it, because libstdc++ skips that
			// relock after the LAST callback in the list; the test is built so the release
			// lands during an earlier one.
			const std::stop_source children_source = m_children;
			children_source.request_stop();
		}

		/** Whether cancel() has been called. Pairs with the release in cancel(). */
		[[nodiscard]] bool cancelled() const noexcept
		{
			return m_cancelled.load(std::memory_order_acquire);
		}

		/**
		 * @brief Forwards a stop of @p token into cancel().
		 *
		 * A token already stopped cancels immediately, inside this call. A token that
		 * can never be stopped registers nothing. Call at most once.
		 */
		void link(std::stop_token token)
		{
			if(token.stop_possible())
			{
				m_link.emplace(std::move(token), combinator_stop_forwarder{this});
			}
		}

	private:
		// DELICATE. Declaration order is load-bearing here.
		//
		// m_link is declared last, so it is destroyed first. std::stop_callback's
		// destructor blocks until an invocation already running on ANOTHER thread
		// returns, so destroying it ahead of every other member guarantees no such
		// cancel() is in flight over them. This is wait_awaiter's reason for declaring
		// m_stop after m_node. Unlike cancellable_task_promise, the hazard is not
		// removed structurally: the callback really does point into this object, so
		// nothing but this order protects it, and nothing in the language checks it.
		//
		// The protection covers other threads only. For an invocation running on the
		// CURRENT thread ([stopcallback.cons]) the destructor returns immediately by
		// design, so the order does not protect a reentrant last release: cancel() ->
		// request_stop() -> a child's completion hook drives the counter to zero -> the
		// awaiting chain resumes and drops the last reference -> destroy() -> this
		// destructor frees the block while the outer request_stop() frame is still
		// unwinding through it.
		//
		// That chain is reachable, and on the ordinary cancellation path rather than an
		// exotic one: WhenAllTest.TheChildrensStopOutlivesABlockTheLastChildFreed walks
		// it. An earlier version of this note said the owning combinator had to prevent
		// it and that this class could not. The second half was wrong -- cancel() now
		// holds a counted copy of the stop state across the walk, which is what makes
		// the destruction survivable, and the note there is the one to read. What this
		// order still buys is the other-thread case above, and it remains load-bearing
		// for it: it is the only thing standing between a cancel() in flight on another
		// thread and the members it reads.
		//
		// m_references starts at 1, the awaiter's.
		std::atomic<std::size_t> m_references{1};
		completion_counter       m_counter;
		std::stop_source         m_children;
		std::atomic<bool>        m_cancelled{false};
		void*                    m_owner;
		void (*m_destroy)(void* owner) noexcept;
		std::optional<std::stop_callback<combinator_stop_forwarder>> m_link;
	};

	inline void combinator_stop_forwarder::operator()() const noexcept
	{
		target->cancel();
	}

}

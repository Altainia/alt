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
		 * out holds it until every child has been started, so a child completing
		 * synchronously during the fan-out cannot drive the count to zero and resume a
		 * parent that has not finished suspending. This is task_group::spawn's "raise
		 * the count before the task starts" rule generalized to N starts.
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
		 * returned. It does not wait for one running on the calling thread, so the last
		 * reference must never be released from inside cancel(); see the note on m_link.
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
		 * Runs on whichever thread requests the stop.
		 */
		void cancel() noexcept
		{
			// The flag before the stop, and nothing may reorder them. A cancelled child
			// still fires its completion hook, so the children's stop can drive the
			// counter to zero and resume the awaiting chain; if that chain read the flag
			// as false it would report a satisfied wait and assemble results from slots
			// no child body ever filled.
			m_cancelled.store(true, std::memory_order_release);
			m_children.request_stop();
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
		// unwinding through it. The combinator that owns this block must prevent that
		// structurally (for example by holding a reference across cancel()); this class
		// cannot.
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

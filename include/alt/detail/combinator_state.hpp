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
		 * 0/200 in debug, 0/200 under ThreadSanitizer, and 0 across the whole 35-test
		 * WhenAllTest suite -- 20 repeats of it in debug and 10 under AddressSanitizer.
		 * Deleting the retirement altogether does fail, 20/20, so what is guarded
		 * is that the starter is retired, not where. An earlier version of this comment
		 * claimed the position protected a parent "that has not finished suspending";
		 * three passes over this file asserted that before anyone measured it.
		 *
		 * The count stays, and it stays defensive: a combinator that parks before its
		 * fan-out ends would need it, and removing it would change this constructor's
		 * semantics for no gain. **alt::when_any is not that combinator.** An earlier
		 * version of this note kept the item for one that "may", naming when_any as the
		 * candidate; it parks on the counter strictly after its fan-out, exactly as
		 * when_all does, and when_any_awaiter::await_suspend re-measured the retirement's
		 * position against its own tests rather than inheriting the figure above. So no
		 * shipped combinator needs this item, and the next one that parks early is who it
		 * is kept for.
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
			// Swapping these two statements fails
			// CombinatorStateTest.CancelMarksTheWaitReleasedBeforeStoppingTheChildren
			// 200 times out of 200 repeats, and that was for a while the only test that
			// guarded the order. Two measurements have been added to it since, and the
			// three together are what the order rests on.
			//
			// **IN alt::when_all IT FAILS NOTHING -- IN A DEBUG BUILD.** 0 failures in 200
			// repeats of each of the five cancellation-path tests run one at a time, and in
			// a separate single run of the whole WhenAllTest suite all 35 of its tests still
			// pass. That figure names a preset because the preset is the whole of its
			// scope: UNDER ADDRESSSANITIZER THE SAME SWAP IS A HEAP-USE-AFTER-FREE, reported
			// at the flag store in this function, with
			// WhenAllTest.CancellingTheAwaitingChainReportsOperationCanceled the first test
			// to walk into it and the run aborting there. The children's stop frees this
			// block -- the last child's completion hook drops the last reference, which is
			// cancel()'s ordinary path and the DELICATE note below -- so a flag stored
			// afterwards is stored into freed memory. The order is therefore load-bearing
			// for this block's LIFETIME on this toolchain, and not only for some other
			// implementation's verdict.
			//
			// BOTH HALVES OF THAT RE-TAKEN 2026-11-16, by the whole-branch reviewer, against
			// the tree at 1c016b2, where the suite stood at 799 tests -- though no suite count
			// belongs to these figures, which come from a standalone probe and not from a suite
			// run. The probe replicates
			// WhenAllTest.CancellingTheAwaitingChainReportsOperationCanceled -- a when_all over
			// two children parked on their own async_event, then handle.request_stop().
			// Unmutated, -O0 with asserts on: 0 failures in 200 repeats. Swapped, -O0 with
			// asserts on and NO sanitizer: 0 failures in 200 repeats, so the zero above is
			// today's zero and not only the one it was when first taken. Swapped, -O0 under
			// -fsanitize=address,undefined: heap-use-after-free, WRITE of size 1, at this
			// function's flag store, reached through combinator_stop_forwarder::operator() from
			// cancellation_handle::request_stop. The re-take was worth its two compiles for one
			// reason: PR 1's waiter_list sentinel falsified the nearest relative of this figure
			// by changing what a CLAIM can see, and this path has no claim in it -- the swapped
			// store lands on this block's own flag word, which is not a waiter_list -- so the
			// zero was predicted to survive the sentinel and did.
			//
			// The debug reason the flag is not the deciding half in when_all is in
			// when_all_awaiter's await_resume note: the chain's own stop claims the joiner
			// before cancel() ever runs, so that verdict is settled on !waited. That is a
			// fact about one cancellation context relaying one stop, and a combinator can
			// reach await_resume with the wait SATISFIED and this flag SET by other routes:
			// WhenAnyTest.ASatisfiedWaitWithTheCancellationFlagSetDiscardsItsWinner does it
			// with two contexts answering one chain's two reads, and a cross-thread stop
			// landing after the counter's zero transition does it in production. That test
			// is the FIRST TEST AT COMBINATOR LEVEL TO CATCH THIS SWAP AT ALL: in debug the
			// swap fails it and the CombinatorStateTest above and nothing else in the
			// 799-test suite; under AddressSanitizer the suite never reaches it, the
			// use-after-free above aborting the run in the when_all test first. A
			// combinator that parks before its fan-out ends would be a third route; none
			// does today, which is the constructor note's business rather than this one's.
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
			//
			// One clause on why that citation holds, because it looks like the kind
			// waiter_list's notes reject. The freed object here is libstdc++'s stop
			// state, and the access AddressSanitizer reports is _M_lock()'s
			// read-modify-write of that state's own word, inlined into this translation
			// unit from <stop_token> and so instrumented like any other access. A claim
			// on a destroyed waiter_list reaches it through pthread_mutex_lock inside
			// glibc, which neither sanitizer instruments, and that is why that class
			// needs a liveness sentinel of its own and why a green ASan run says nothing
			// there. The difference is what the freed memory is touched BY, not how
			// exotic the race is; see waiter_list's class notes before generalizing
			// either way.
			const std::stop_source children_source = m_children;
			children_source.request_stop();
		}

		/**
		 * @brief Stops the children without marking the wait cancelled. Idempotent.
		 *
		 * For a winner stopping the losers on the success path, where setting the flag
		 * would make cancelled() true on every successful wait and so no longer mean
		 * "the chain was cancelled". Safe against this block being freed by the stop it
		 * requests, for the reason in cancel()'s DELICATE note.
		 */
		void request_children_stop() noexcept
		{
			// The counted copy cancel() holds, for the reason its DELICATE note gives.
			// The one difference: this never touches m_cancelled.
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
		// THAT HAS A MEASURED GUARD AS OF 2026-11-17, where this note previously asserted
		// the order's importance without one. Declaring m_link first instead of last, so
		// it is destroyed last, is detected by
		// CombinatorStateTest.ACancelRacingAChildsDrainIntoTheBlockIsClean: 15 of 20 runs
		// of that test alone in debug (10 aborting on glibc's "unaligned tcache chunk
		// detected", 5 hanging), 14 of 20 under AddressSanitizer as a double-free or a
		// heap-use-after-free, and 6 of 20 under ThreadSanitizer as a hang -- which is the
		// deadlock ~std::stop_callback's cross-thread wait exists to prevent, arriving as
		// a timeout because that wait is what the mutation strands.
		//
		// Under AddressSanitizer that test is the ONLY guard: with the mutation applied and
		// it alone filtered out, the suite passes 810 of 810. The other two builds do catch
		// it elsewhere -- debug in
		// CombinatorStateTest.ARaceBetweenAStopAndTheLastReleaseIsClean and ThreadSanitizer
		// in WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock -- so what
		// is unique to it is the build that NAMES the fault instead of reporting heap
		// corruption or a timeout. The repeat that does the catching is the one where the
		// child's drain thread drops the block's last reference while the stop's walk still
		// has this callback pending, which is the other-thread case above, reached
		// deliberately rather than by luck.
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

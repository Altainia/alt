#pragma once

#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cstddef>

namespace alt
{

	/**
	 * @brief A single-use countdown that coroutines await.
	 *
	 * Constructed with a count; count_down() lowers it, and the call that brings it
	 * to zero releases every waiter at once. Awaiting a latch whose count is already
	 * zero does not suspend. The latch does not reset: once at zero it stays there,
	 * which is what makes it safe to await from a coroutine that arrives late.
	 *
	 * Awaiting yields alt::wait_result. It holds a value when the count reached zero,
	 * and std::errc::operation_canceled when the wait ended without that: the awaiting
	 * chain was cancelled, or this latch was destroyed with waiters still parked.
	 *
	 * Every member but the destructor is safe to call from any thread, and
	 * concurrently: of several threads counting down at once, exactly one is the call
	 * that reaches zero, and it alone releases the waiters. Destruction is not, as
	 * ever, and in particular a latch must not be destroyed while a cancellation is
	 * in flight against one of its waiters. Resumption happens on the thread that
	 * reached zero, inside count_down(), which therefore runs waiter bodies;
	 * co_await alt::resume_on(scheduler) in a waiter that needs to be somewhere
	 * specific.
	 *
	 * Neither copyable nor movable: the awaiters inside suspended coroutine frames
	 * hold pointers to it, and no copy or move could fix those up.
	 *
	 * Two things a waiter's body may not do, both shared with async_event. It may not
	 * destroy another waiter of the same latch: the releasing count_down() takes the
	 * whole queue before it resumes anyone, so a frame destroyed from inside one
	 * resumption may be one that call is already committed to resuming. And a
	 * coroutine parked here may not have its frame destroyed from outside, because
	 * the node it is parked on lives in that frame; destroying this latch instead
	 * releases them all, which is the supported teardown.
	 *
	 * A waiter that teardown released may not await this latch again. That is
	 * unsupported on all four primitives in this family, by choice rather than by
	 * necessity: the destructor releases its waiters itself, ahead of m_waiters' own
	 * destructor, which does keep the ordinary case from leaking a re-awaiting waiter
	 * -- but that is a defensive measure rather than a promise, and no depth of
	 * re-await is promised. alt::async_mutex's destructor records the reason the
	 * family settled on promising nothing.
	 *
	 * Destroying the latch itself from a resumed waiter's body is a different thing
	 * and is allowed: the releasing count_down() publishes the zero and takes the
	 * queue first, and the resumption is the last thing it does, so nothing is left to
	 * be read afterwards. It carries one qualification a waiter cannot check for
	 * itself, the same one async_semaphore::release carries and for the same reason,
	 * that call resuming one multi-node chain exactly as that one does. It is
	 * committed to resuming every waiter it took, so a waiter that destroys the latch
	 * may be followed by siblings that are still resumed -- correctly, each having
	 * been won before anything ran -- into an object that no longer exists. Those
	 * siblings must not touch the latch after their wait returns. Since no waiter can
	 * tell whether it is the last of a chain, in practice a waiter that destroys the
	 * latch must be one whose siblings do not go back to it, which is what a shutdown
	 * looks like: every waiter is leaving.
	 *
	 * That first rule is why count_down() can resume its waiters from one multi-node
	 * chain (detach_all) instead of one detach_one at a time. An ordinary awaiting
	 * chain cannot create the sibling hazard, because a task is lazy and owns only the
	 * child it awaits, and a task_group's tasks are self-owning roots it never destroys.
	 * alt::when_all (see include/alt/when_all.hpp) and alt::when_any
	 * (include/alt/when_any.hpp) add combinators to that picture and do not create the
	 * hazard either. Three invariants carry that, labeled as those components' own
	 * documentation and docs/when_all.md label them, so that a reader arriving from any
	 * of them can check the one they came for:
	 *
	 * - **(A) Ownership.** No combinator owns or destroys a child frame. The children are
	 *   self-owning alt::cancellable_task roots, and a combinator only ever requests
	 *   their stop.
	 * - **(B) The claim election cannot free an already-claimed node.** A loser whose node
	 *   a satisfier already won loses the compare-and-swap in waiter_list::claim, so its
	 *   waiter_stop_release resumes and frees nothing; the only way a child frame dies is
	 *   the child itself running, which happens only when the walk reaches it.
	 * - **(C) Lifetime overlap.** A combinator may resume the awaiting chain -- and so let
	 *   that chain destroy the combinator's own frame -- while children are still alive.
	 *   This is why the shared state is reference counted off the frame instead of owned
	 *   by it, and (A) is what keeps the sibling hazard away across that overlap: the
	 *   children that outlive the frame destroy nothing but themselves.
	 *
	 * The four components that resume a multi-node chain are async_event::set, this
	 * count_down, async_semaphore::release -- which wins its waiters one detach_one at a
	 * time but resumes them as one chain -- and all four destructors, including the
	 * ~waiter_list backstop each of them runs ahead of, which walks a multi-node chain
	 * of its own and is reachable only through those four. What protects them is (A) and
	 * (B) -- those two facts about ownership and the claim -- not the absence of a
	 * combinator, so the arrival of one did not force a revisit.
	 *
	 * **THE SECOND COMBINATOR FITS THAT ARGUMENT, and it is worth saying why rather than
	 * leaving it to the count.** alt::when_any's children are the same self-owning roots
	 * it can only stop, so (A) holds verbatim, and it destroys no child, so (C)'s first
	 * clause does too. **It also adds no overlap that alt::when_all does not already
	 * have**, which is the part a reader is likeliest to assume rather than check: the two
	 * carry the same completion hook, when_any's being a byte-for-byte copy of when_all's
	 * rather than a second design (each note says so and names the other), both park
	 * exactly one joiner on a detail::completion_counter, and both are resumed by the same
	 * zero transition inside that counter's complete(). So (C)'s overlap is the same two
	 * shapes in both, and neither is peculiar to either component:
	 *
	 * - On a SATISFIED wait that parked, the chain is resumed from inside the LAST child's
	 *   completion hook. That hook fires from the child's FRAME DESTRUCTION and calls
	 *   complete(), whose zero transition resumes the joiner before it returns, and only
	 *   then does the hook release(). So the resuming child -- its frame being destroyed
	 *   around the call -- holds a block reference across the resumption, while the resumed
	 *   chain runs on and may destroy the combinator's frame. ~when_all_awaiter builds its
	 *   first bullet on exactly that reference. One child, in both components.
	 *
	 *   **The FRAME is what the reference hangs on, not the body**, and the two come apart
	 *   in when_any: its last retiree may be a LOSER the winner's stop tore down without
	 *   finishing -- when_any_child's second and fourth cases -- where a satisfied when_all
	 *   has no stop and its last retiree ran its body to the end. Either way the hook fires
	 *   from the destruction and the reference is held across the resumption, which is why
	 *   the shape is the same. An earlier draft of this bullet said "its body already
	 *   finished" and was false of when_any for exactly that reason.
	 *
	 *   A when_any that resumed on the election instead would be the component that
	 *   differed here -- it would resume with its losers still running -- and that is
	 *   precisely the design it rejected: the await parks until the last child retires.
	 * - On a CANCELLATION the chain is resumed RELEASED by its own stop callback, with
	 *   children still parked or unwinding and their teardown possibly posted to a home
	 *   scheduler. Many children, really alive, in both components.
	 *
	 * **BOTH SHAPES ARE DERIVED, NOT MEASURED.** They are read off the hook, the counter
	 * and ~when_all_awaiter's note. No test in the suite fails if the satisfied-path
	 * overlap stops being what this says, and no instrument watches it: the nearest guard
	 * is the hook's own statement order, which a debug build misses entirely and only
	 * AddressSanitizer catches, and what that measures is the use-after-free, not this
	 * shape. The CONCLUSION survives the gap, resting on (A) rather than on the shape, so a
	 * wrong description here cannot make the library unsafe; the DESCRIPTION does not, so
	 * check it against the code rather than citing this note as evidence for it. Figures
	 * elsewhere in these components are mutation measurements with presets and suite sizes
	 * attached: this paragraph is not one of them.
	 *
	 * An earlier version of this paragraph made (C) a matter of FREQUENCY: the overlap
	 * cancellation-only in when_all, every-success in when_any. Both halves were false.
	 * ~when_all_awaiter's first bullet refutes the first, resting as it does on the
	 * resuming child's reference on the SATISFIED path; and when_any's park-until-zero
	 * guarantee refutes the second, no loser being left RUNNING when a won await returns --
	 * the last retiree's frame is mid-destruction there, which is the first bullet's point
	 * and not a loser still in flight. The conclusion they were offered in support of
	 * never needed either: (A) is what keeps the sibling hazard away across both shapes,
	 * however often each happens, because the children that outlive the frame destroy
	 * nothing but themselves. So a reader need not re-derive this per combinator.
	 *
	 * What WOULD break it is not another combinator but a change to who claims. A change
	 * to waiter_list::claim or to resume_claimed would: resuming one node per detach, and
	 * not holding the next node across a resumption, is what each of the four would then
	 * have to change to. So would a combinator that parked a second joiner on one
	 * completion counter, which is a different enumeration again and is recorded in
	 * detail::when_any_awaiter's note.
	 */
	class async_latch
	{
	public:
		/**
		 * Constructs a latch that releases its waiters after @p count calls to
		 * count_down(), counted by their arguments. Zero is a latch that is already
		 * released. The count must not be negative.
		 */
		explicit async_latch(std::ptrdiff_t count);

		/**
		 * @brief Releases every waiter still parked, unsatisfied.
		 *
		 * A backstop rather than the expected path, like waiter_list's own: a waiter
		 * nobody resumes stays suspended forever and its frame is never freed, so
		 * releasing them beats leaking them. Each one sees the released error, which
		 * is how it tells this apart from the count actually reaching zero.
		 */
		~async_latch();

		async_latch(const async_latch&)            = delete;
		async_latch& operator=(const async_latch&) = delete;
		async_latch(async_latch&&)                 = delete;
		async_latch& operator=(async_latch&&)      = delete;

		/**
		 * @brief Lowers the count by @p n, releasing every waiter if that reaches zero.
		 *
		 * Counting down past zero is harmless rather than an error: the count stops at
		 * zero, and only the call that actually brought it there releases anyone, so a
		 * latch is never released twice. @p n must not be negative; zero changes
		 * nothing.
		 *
		 * Waiter bodies run on this thread before this returns when this call reaches
		 * zero, so it must not be called with a lock held that a waiter might take.
		 */
		void count_down(std::ptrdiff_t n = 1);

		/** True when the count has reached zero, so an await would not suspend. */
		[[nodiscard]] bool try_wait() const noexcept;

		/**
		 * @brief One suspended waiter on an async_latch.
		 *
		 * Constructed by operator co_await and lives in the awaiting coroutine's
		 * frame. detail::wait_awaiter holds everything but the two questions this
		 * answers: whether the wait is over, and where to park.
		 */
		class latch_awaiter: public detail::wait_awaiter<latch_awaiter>
		{
		public:
			/** The latch must outlive the suspension. */
			explicit latch_awaiter(async_latch& latch) noexcept: m_latch(&latch)
			{}

			/**
			 * Whether the wait is already over. Also the condition the link is made
			 * under, with the list's mutex held, so it reads one atomic: no lock of its
			 * own, no call back into the list, nothing resumed. See
			 * detail::wait_awaiter for the whole of that contract.
			 */
			[[nodiscard]] bool ready() const noexcept
			{
				return m_latch->try_wait();
			}

			/** The queue this waiter parks in. */
			[[nodiscard]] detail::waiter_list& list() const noexcept
			{
				return m_latch->m_waiters;
			}

		private:
			async_latch* m_latch;
		};

		/** Awaits the latch, yielding satisfied-or-released. */
		[[nodiscard]] auto operator co_await() noexcept
		{
			return latch_awaiter{*this};
		}

	private:
		/**
		 * Atomic because it is read without the list's mutex by a waiter deciding
		 * whether to wait at all, and under it as the condition on the link, which is
		 * what keeps a count_down that reaches zero between those two steps from
		 * leaving a waiter parked on a released latch. Written only by compare-exchange
		 * so that exactly one caller observes the step onto zero. See count_down.
		 */
		std::atomic<std::ptrdiff_t> m_count;

		detail::waiter_list m_waiters;
	};

}

#pragma once

#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/async_wait.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/detail/combinator_results.hpp>
#include <alt/detail/combinator_state.hpp>
#include <alt/detail/completion_counter.hpp>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace alt::detail
{

	/**
	 * @brief The shared block plus the result slots one when_all needs.
	 *
	 * Owns the block by composition rather than by inheritance, so that
	 * detail::combinator_state stays a concrete non-template class that a test can
	 * drive without instantiating a combinator. The erased destroy function the block
	 * calls on its last release is this class's own, which is what makes the block's
	 * reference count the lifetime of the slots too.
	 *
	 * Allocated by when_all_awaiter::await_suspend and freed by nothing else: the last
	 * reference to the block frees it, whichever party drops it.
	 */
	template<typename Slots>
	class when_all_state
	{
	public:
		/** Constructs the slots and a block with @p children outstanding plus the starter. */
		explicit when_all_state(const std::size_t children):
		  m_slots(make_combinator_slots<Slots>(children)),
		  m_shared(children, this, &destroy)
		{
		}

		/** Destroys the block first and the slots after it. See the note on the members. */
		~when_all_state() = default;

		when_all_state(const when_all_state&)            = delete;
		when_all_state& operator=(const when_all_state&) = delete;
		when_all_state(when_all_state&&)                 = delete;
		when_all_state& operator=(when_all_state&&)      = delete;

		/** The refcounted block the children and the awaiter share. */
		[[nodiscard]] combinator_state& shared() noexcept
		{
			return m_shared;
		}

		/** The slots the children write their results into. */
		[[nodiscard]] Slots& slots() noexcept
		{
			return m_slots;
		}

	private:
		/** Frees this object. Handed to the block as its erased destroy function. */
		static void destroy(void* owner) noexcept
		{
			// Reclaimed through a std::unique_ptr rather than a bare delete, which
			// cppcoreguidelines-owning-memory rejects for a pointer that is not a
			// gsl::owner. The block is freed as this goes out of scope.
			const std::unique_ptr<when_all_state> reclaimed{static_cast<when_all_state*>(owner)};
		}

		// DELICATE. Declaration order is load-bearing here.
		//
		// m_shared is declared after m_slots, so the block -- and with it the block's
		// m_link, whose destructor blocks until a cancel() forwarded on another thread
		// has returned -- is destroyed first, before the slots that cancel() reaches
		// through the children are gone.
		Slots            m_slots;
		combinator_state m_shared;
	};

	/**
	 * @brief The completion hook every child of a combinator carries.
	 *
	 * @p state is the detail::combinator_state the child shares with its siblings and
	 * with the awaiting chain. Fired from the child's frame destruction, which is the
	 * only point both the normal and the cancelled path pass through.
	 *
	 * **THIS FUNCTION HAS A DUPLICATE: detail::on_when_any_child_complete, in
	 * when_any.hpp.** Same two statements, same order, byte for byte. It is a copy
	 * rather than a call into this header so that including alt/when_any.hpp does not
	 * drag this whole component into every translation unit that awaits a when_any.
	 * The consequence is a constraint on THIS function, which is why it is recorded
	 * on this side too: **the order of the two statements below cannot be revisited
	 * here alone.** A change to it must be made in both, and the derivation that change
	 * would invalidate lives in each component's own awaiter destructor -- ~when_all_-
	 * awaiter below, and ~when_any_awaiter there, which re-derives rather than cites.
	 * If a third combinator ever wants this hook, move it to combinator_state.hpp and
	 * carry this note with it rather than making a third copy.
	 */
	inline void on_combinator_child_complete(void* state) noexcept
	{
		auto* const shared = static_cast<combinator_state*>(state);

		// complete() may reach zero and resume the awaiting chain on this thread,
		// whose body may drop its own reference. release() must follow it, and
		// nothing may be read from shared after release(): this may be the last one.
		//
		// This order is load-bearing a second time, somewhere a reader of this function
		// would not look. ~when_all_awaiter's note derives the satisfied path's immunity
		// to ITS hazard from exactly this statement order: complete() before release()
		// means the child that drove the count to zero still holds a reference while the
		// chain runs on, and -- on the caller shapes where that destructor runs within
		// complete()'s dynamic extent -- while the chain's awaiter is destroyed too.
		//
		// Do not carry that immunity away from here as unconditional. It is not: the
		// condition it needs, and the legal caller shape that fails it, are the first and
		// fourth bullets of that destructor's note, which is the only place either is
		// stated. What holds unconditionally is the implication, which is the part that
		// constrains these two lines: swapping them removes the reference the immunity
		// rests on whether the condition is met or not, so the swap opens a window there
		// as well as the use-after-free described below. Read that destructor before
		// touching these two lines.
		//
		// Measured rather than argued, and the instrument matters. **EVERY FIGURE BELOW
		// IS A MEASUREMENT OF THIS COPY OF THE HOOK**, when_all's. Swapping these two
		// lines, measured 2026-11-16 against the suite at 799 tests:
		//
		// - DEBUG, whole suite: ABORTS after 749 tests, in
		//   WhenAllTest.TheChildrenInheritTheChainsHomeSchedulerForTheirTeardown, on
		//   waiter_list's liveness assert inside detach_all -- "detaching every alt
		//   waiter on a destroyed list".
		// - DEBUG, one test at a time: 10 of the 35 WhenAllTest tests abort the same
		//   way. Eight of them were run five times each and aborted every time; the
		//   other two were run once.
		// - ADDRESSSANITIZER, on that same test: heap-use-after-free in
		//   completion_counter::complete(), this function one frame above it. The
		//   earlier 200/200 count for it was not re-taken.
		// - RELEASE-TESTS, whole suite: PASSES, 794 of 799 with 5 skipped.
		//
		// **THE DEBUG CATCH EXISTS ONLY BECAUSE AN ASSERT EXISTS**, which is what that
		// last line measures: a build with NDEBUG and no sanitizer, where the assert is
		// gone and the swap runs green with the use-after-free still there. It does not
		// say the fault becomes undetectable without the assert -- complete() opens by
		// touching the freed counter's own word, which is where AddressSanitizer
		// reported it above. DO NOT READ A GREEN RUN AS EVIDENCE THAT THE ORDER DOES
		// NOT MATTER.
		//
		// Which of waiter_list's three windows this lands in, because it is not the
		// obvious one: release() has FREED the block before complete() touches the
		// counter, so this is the second or third -- freed-and-poisoned, or
		// freed-and-reused -- not the first, which is destroyed-but-not-yet-freed and
		// the only one the sentinel covers with certainty. In the freed windows it only
		// rejects every value but alive_pattern, so the debug figure above is what those
		// ten tests did with the bytes they had. ASan's report comes earlier still, at
		// the counter's own atomic word, and that citation is sound where a waiter_list
		// one would not be: an ordinary instrumented access, not a pthread mutex inside
		// glibc.
		//
		// One superseded figure, kept for its lesson: this note used to record "fails
		// nothing in a debug build, 0/200 ... 763 tests at the time", and what falsified
		// it was PR 1's liveness sentinel rather than any edit here. A measured zero can
		// go stale because the instrument changed, so quote a figure with the date and
		// the suite size it was taken at.
		//
		// The guards of record, by preset. In debug the first the suite reaches is
		// WhenAllTest.TheChildrenInheritTheChainsHomeSchedulerForTheirTeardown. Under
		// AddressSanitizer: that same test, which reports it first across the
		// pre-existing suite, and
		// WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock, which
		// reports it among the combinator tests added for this.
		//
		// A third joined that list on 2026-11-17, and it is the only one that calls this
		// function directly rather than reaching it through a child's frame destruction:
		// CombinatorStateTest.ACancelRacingAChildsDrainIntoTheBlockIsClean, which detects
		// the swap 20 of 20 runs of itself under AddressSanitizer and 0 of 20 in debug.
		// Its report lands one frame deeper than the figures above -- at
		// waiter_list::alive() inside detach_all() inside complete(), not at the
		// counter's own atomic word -- because the block it builds has a joiner PARKED on
		// the counter, so the zero transition reaches the list at all. Nothing about this
		// function changed to earn that citation; a sharper instrument did, which is this
		// note's own lesson one paragraph above.
		shared->counter().complete();
		shared->release();
	}

	/**
	 * @brief Runs one child of a combinator as its own cancellation root.
	 *
	 * The awaitable is taken by value in this frame, with @c A the type the
	 * combinator's argument tuple stored: a value for an rvalue argument, which this
	 * frame then owns, and @c T& for an lvalue argument, which the caller must keep
	 * alive for as long as the children run. See alt::when_all for that precondition.
	 *
	 * @p state is deliberately unused in the body. The counter is retired by
	 * on_combinator_child_complete at this frame's destruction, which is the only point
	 * both the normal and the cancelled path pass through; the parameter stays so that
	 * the hook's state pointer and this frame's lifetime are visibly the same thing.
	 */
	template<typename A, typename Slot>
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): A is a reference exactly when the caller passed an lvalue, which alt::when_all documents as a precondition the caller must honor: the awaitable and everything it refers to outlive the children. Slot and state point into the refcounted block, which outlives every child by construction.
	cancellable_task when_all_child(A awaitable, Slot* slot, combinator_state* /*state*/)
	{
		using result_type = alt::await_result_t<A>;

		// The try encloses the whole co_await, not just its result: an awaitable whose
		// await_suspend throws would otherwise escape to unhandled_exception, and a
		// child has no exception handler, so that is std::terminate.
		try
		{
			if constexpr(std::is_void_v<result_type>)
			{
				co_await static_cast<A&&>(awaitable);
				slot->set_value(std::monostate{});
			}
			else
			{
				slot->set_value(co_await static_cast<A&&>(awaitable));
			}
		}
		catch(...)
		{
			slot->set_exception(std::current_exception());
		}
	}

	/**
	 * @brief The awaiter alt::when_all suspends the awaiting chain on.
	 *
	 * @tparam Results       The tuple (or vector) of values a satisfied wait yields.
	 * @tparam Slots         The tuple (or vector) of result stores the children write into.
	 * @tparam Storage       What the awaitable object holds: a std::tuple of awaitables
	 *                       for the argument-list form, or the range itself for the range
	 *                       form, whose Results and Slots are then vectors.
	 *
	 * Allocates the shared block on suspension, fans the children out as self-owning
	 * alt::cancellable_task roots, and parks the awaiting chain on the block's
	 * completion counter. Neither copyable nor assignable; moved exactly once, into
	 * detail::cancellation_checked's storage, before any await member runs.
	 */
	template<typename Results, typename Slots, typename Storage>
	class when_all_awaiter
	{
	public:
		/** Constructs an awaiter over a tuple of awaitables, or a range, that outlives it. */
		explicit when_all_awaiter(Storage* awaitables) noexcept: m_storage(awaitables)
		{
		}

		/**
		 * @brief Moves an awaiter that has not suspended yet. The only kind there is.
		 *
		 * detail::cancellation_checked stores the awaiter it wraps by value, so the
		 * awaiter returned from operator co_await is moved once before any of its await
		 * members run. The block pointer transfers and the source is left null, so only
		 * one of the two releases the reference. detail::wait_awaiter's own move
		 * constructor carries the same assert for the same reason: a field set before
		 * await_suspend would have to be carried here by hand.
		 */
		when_all_awaiter(when_all_awaiter&& other) noexcept:
		  m_storage(other.m_storage),
		  m_state(std::exchange(other.m_state, nullptr))
		{
			assert(!other.m_join.has_value() && "moving an alt when_all_awaiter that has already suspended");
		}

		when_all_awaiter(const when_all_awaiter&)            = delete;
		when_all_awaiter& operator=(const when_all_awaiter&) = delete;
		when_all_awaiter& operator=(when_all_awaiter&&)      = delete;

		/**
		 * @brief Deregisters the joiner's stop callback, then drops the awaiter's
		 *        reference to the shared block.
		 *
		 * DELICATE. This is reference 1, the one the block is constructed with, and
		 * where it is dropped is the whole of three invariants. The reset is the third
		 * of them. It is ordering rather than tidying, but after the refused-link fix in
		 * detail::wait_awaiter::await_suspend it is no longer the fix for a fault any
		 * path reaches: read the measured account below before deciding what it is for.
		 *
		 * The awaiter holds the reference for as long as the awaiting chain is parked,
		 * so the block can never reach zero references with a joiner still on the
		 * counter: the counter is destroyed sixth of the block's seven members, and
		 * ~completion_counter resumes every still-parked joiner, whose body would then
		 * run against five already-destroyed members -- among them m_cancelled, which
		 * await_resume reads. The joiner is always off the counter before this runs --
		 * claimed by the zero transition, claimed by the chain's cancellation, or never
		 * linked at all -- because this destructor runs only at the end of the co_await
		 * full-expression, after await_resume.
		 *
		 * A node being off the counter is NOT the same as that node's stop callback
		 * being deregistered. detail::wait_awaiter::await_suspend emplaces its
		 * std::stop_callback on the chain's token BEFORE it attempts the link, holding a
		 * waiter_stop_release that points at the node and at the counter's waiter_list,
		 * which lives inside the block. What that leaves behind differs by path, and
		 * only one of the two paths still has anything to do with this destructor.
		 *
		 * THE REFUSED LINK -- every child retired before the join could link, the path
		 * await_suspend's false return names -- leaves no callback at all any more.
		 * await_suspend deregisters m_stop in its refused-link tail, ahead of everything
		 * else it writes there, so by the time this destructor runs nothing is
		 * registered that could claim the waiter_list the release below is about to
		 * free. That path is also the only one on which the reference count here is
		 * reliably 1, which is what made it the dangerous one: releasing first freed the
		 * list while a registered callback still pointed into it. It is now
		 * structurally impossible rather than guarded, and the ordering below is not
		 * what closes it.
		 *
		 * Measured on this branch rather than argued, because the test named below used
		 * to be cited here as this statement's guard and no longer is. Moving the reset
		 * under the release passes
		 * WhenAllTest.AStopRacingTheEndOfAnAwaitWhoseJoinerNeverLinked 20 of 20 in each
		 * of the debug, AddressSanitizer and ThreadSanitizer builds, and passes the
		 * whole suite 20 whole-suite runs per preset. Removing await_suspend's reset
		 * instead, and leaving this order alone, also passes it 20 of 20 in all three.
		 * Removing BOTH fails it 20 of 20 in all three, every failure on waiter_list's
		 * liveness assert. So that test guards the conjunction -- the callback is gone
		 * before the release -- and await_suspend's reset is what satisfies it today.
		 *
		 * THE LINKED path keeps its callback until the awaiter dies, which is here. The
		 * joiner is off the counter, claimed by the zero transition or by its own stop,
		 * but off the counter is not deregistered, so this destructor is where the
		 * deregistration happens. Whether the release below can free the list from under
		 * it is a question about the reference count, and both halves of the answer are
		 * measured:
		 *
		 * - WHICH PATH CAN GET HERE AT ALL IS STRUCTURAL. A linked joiner's verdict says
		 *   who resumed it, and both answers are forced. A SATISFIED joiner was resumed by
		 *   the zero transition: that is the only resumption in completion_counter which
		 *   writes satisfied -- resume_claimed(m_joiners.detach_all(), true), inside
		 *   complete() -- and when_all's only complete() that can reach a parked joiner is
		 *   the one in on_combinator_child_complete, which calls it BEFORE its release().
		 *   So the child that drove the count to zero still holds its reference while the
		 *   chain runs on, and -- PROVIDED THIS DESTRUCTOR RUNS WITHIN complete()'s
		 *   DYNAMIC EXTENT -- while this destructor runs as well, in which case the
		 *   awaiter's release cannot be the block's last and that path has no window at
		 *   all. That proviso is a condition on the CALLER rather than on anything here,
		 *   and it is not always met: the ordinary auto r = co_await when_all(...);
		 *   statement guarantees it, a full-expression holding a later suspension point
		 *   does not. The fourth entry in the breakage list below is that shape, and on it
		 *   the satisfied path has a window too. The only other complete() is the
		 *   starter's, in await_suspend, which runs before the park. A
		 *   RELEASED joiner that linked was resumed by its own waiter_stop_release: the two
		 *   other parties that resume a joiner unsatisfied, ~completion_counter and
		 *   ~waiter_list, can only reach one still parked when the block is destroyed,
		 *   which the second paragraph of this note rules out. So on the one path that has
		 *   a window, the registered callback and the resuming invocation are the same
		 *   thing by construction, not by observation.
		 * - The count therefore CAN be 1 here with the callback still registered, on the
		 *   released path and only there, and one test reaches it. Instrumented across the
		 *   whole suite that coincidence is 1 of 7,542 awaiter destructions under
		 *   ThreadSanitizer, 0 of 15,080 in debug and 0 of 7,540 under AddressSanitizer;
		 *   that single hit localizes to
		 *   WhenAllTest.ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock, and run
		 *   alone that test reaches it 7 times in 12,000 destructions in debug and 5 times
		 *   in 12,000 under ThreadSanitizer, still none under AddressSanitizer -- a rate
		 *   that depends on what else the process has just been doing, which is why the
		 *   figures differ between the two ways of running it. All thirteen hits -- the one
		 *   whole-suite coincidence and the twelve isolated ones -- reported the
		 *   node released rather than satisfied, which corroborates the bullet above rather
		 *   than carrying it: the chain's own stop callback claimed the parked joiner and
		 *   resumed the chain, and the one child's completion hook retired and released its
		 *   reference before the chain reached this destructor. So the awaiter's release is
		 *   the block's last, and with the reset moved under it the list is freed with
		 *   m_stop still engaged.
		 * - Nothing can arrive on that freed list, which is why no instrument reports it.
		 *   Four parties touch the counter's waiter_list, and this list is meant as an
		 *   exhaustive one: the joiner's own waiter_stop_release; link() from the joiner's
		 *   own await_suspend; detach_all() from the complete() that reaches zero; and the
		 *   drains in ~completion_counter and ~waiter_list, which are the party that runs
		 *   INSIDE the window described here, during the block's own teardown. The link is
		 *   long past. A complete() cannot be in flight, because
		 *   on_combinator_child_complete calls complete() before release(), so a count of 1
		 *   here means every child's release -- and therefore every child's complete() --
		 *   has already returned. The two destructor drains find an empty list, the joiner
		 *   being off the counter before this destructor runs at all. And the callback is
		 *   the invocation that resumed this chain, per the first bullet: it runs on this
		 *   thread, it is past its claim, and ~std::stop_callback does not block for an
		 *   invocation on the current thread. A second invocation is not reachable either,
		 *   one request_stop() per stop state running the callbacks. This inertness
		 *   argument carries the first bullet's proviso with it, and fails where that
		 *   proviso does: on a SATISFIED chain the registered callback has never run, so
		 *   where a satisfied chain does reach here as the block's last reference, a
		 *   racing request_stop() would invoke it against a freed list for real instead of
		 *   meeting a same-thread invocation already past its claim.
		 *
		 * So on the ordinary single-suspension co_await statement this order prevents no
		 * fault that is reachable today. That is NOT the stronger claim that no fault is
		 * reachable at all: the fourth entry below needs no change to this library, only a
		 * legal caller shape, to put a SATISFIED chain's awaiter here holding the block's
		 * last reference with its callback still registered -- and on that shape, with
		 * these two statements swapped, a racing stop is a use-after-free. Nothing in the
		 * shipped tree faults; this order is what prevents it. The honest statement of
		 * what the order buys is: the callback is deregistered before the list can be
		 * freed by construction, rather than by the argument above. Four things break that
		 * argument:
		 *
		 * - Swapping complete() and release() in on_combinator_child_complete. The
		 *   satisfied path's immunity is exactly "the resumer still holds a reference", and
		 *   that swap removes it, so the window would open on every satisfied chain instead
		 *   of on a rare released one. The swap is already forbidden for a different reason
		 *   and with its own measurement; the note on those two statements points back here.
		 * - A joiner resumption that is not inline in complete(). Posting the chain's
		 *   resumption to a scheduler would decouple the resumer's reference from this
		 *   destructor, and the satisfied path would gain the window the same way.
		 * - A second claimer on the counter's list: a combinator that parks more than one
		 *   joiner on it, or one that parks before its fan-out has ended, has a claimer that
		 *   is neither this thread's own callback nor an already-retired child. No shipped
		 *   combinator does either -- alt::when_any was the candidate for the second and
		 *   parks strictly after its fan-out, measured there -- so this is a condition on a
		 *   future one.
		 * - A CALLER whose co_await full-expression holds a SECOND suspension point. This
		 *   one is not a change to the library: consume(co_await when_all(a, b), co_await
		 *   other) is legal C++ today. This awaiter is a temporary of that full-expression
		 *   -- see when_all_awaitable's note below, which says the same of the awaitable
		 *   -- so it is destroyed only once the second suspension has resumed, which is
		 *   long after complete() returned and the hook's release() dropped the completing
		 *   child's reference. Compiled rather than inferred: GCC 15.2 and clang 19.1 both
		 *   destroy the first co_await's awaiter after the second one has resumed. So on
		 *   that shape a SATISFIED chain reaches this destructor holding the block's last
		 *   reference, which costs the first bullet its immunity and the third bullet its
		 *   inertness argument at once, the registered callback never having run. With the
		 *   two statements swapped this is the cheapest fault on the list to reach: a
		 *   legal caller shape plus a racing stop, and none of the three changes above.
		 *
		 * In all four the window above is a use-after-free with nothing left to argue
		 * about. Keep the reset first.
		 *
		 * AND FOR THE NEXT COMBINATOR. If one parks more than one joiner on a counter, or
		 * parks before its fan-out has ended, every enumeration in this note -- the two
		 * complete() calls, the one resume_claimed that writes satisfied, the three
		 * unsatisfied resumers, the four parties that touch the counter's list -- must be
		 * RE-DERIVED FROM SCRATCH rather than cited. Each of them is correct today, for
		 * when_all, and three readers checked all four; none of the three was looking at
		 * the lifetime of the awaiter itself, which is exactly where the gap in the fourth
		 * bullet turned out to be. An enumeration over call sites is the instrument that
		 * already failed once in stage A, and citing one of these from a component with a
		 * different parking shape repeats that failure at greater length.
		 *
		 * alt::when_any, which this paragraph was addressed to before it existed, parks one
		 * joiner strictly after its fan-out, so the derivation does carry to it -- and
		 * ~when_any_awaiter re-derived all four enumerations against its own call sites
		 * anyway rather than taking them on trust. That is the standard for the one after
		 * it too.
		 *
		 * The ordering is also still what satisfies detail::wait_awaiter's stated
		 * precondition locally, "the primitive must outlive every claim on its waiters":
		 * ~std::stop_callback both deregisters the callback and blocks for an invocation
		 * already running on ANOTHER thread, and here it does so while the block is
		 * still referenced. when_all is the first component in the library that can free
		 * the primitive from inside its own awaiter. Member destruction will not do this
		 * on its own: the destructor body runs before any member is destroyed, so
		 * ~m_join -- and with it ~m_stop -- would otherwise run after the release.
		 *
		 * There is no guard of record for this statement, which is the state of it
		 * rather than an omission. No test in the suite fails when these two statements
		 * are swapped. What is pinned is the mechanism that would report the fault if a
		 * later change made the window reachable:
		 * WaiterListDeathTest.AClaimThatWouldLoseTheElectionOnADestroyedListAborts, for
		 * waiter_list::claim asserting its list alive BEFORE the election, so that even
		 * the late claim a resumed joiner's node produces -- one that would lose and
		 * touch nothing -- aborts instead of returning quietly. It reaches that by
		 * destroying a list by hand, not through this destructor.
		 *
		 * The reference is still dropped after await_resume has finished reading the
		 * slots, which is why the release is here and not there, and the reset does not
		 * disturb that: await_resume has long returned by the time any of this runs, and
		 * it reads the joiner's verdict out of the node rather than out of the callback.
		 *
		 * What this does NOT make unreachable is the block's last reference going while
		 * a cancel() is still unwinding. The chain is released before the children are
		 * stopped, so this destructor runs first and a child's completion hook then
		 * drops the last reference from inside combinator_state::cancel(). That is the
		 * ordinary cancellation path, and what makes it survivable is the counted copy
		 * of the stop state cancel() holds -- not anything here. Read that note before
		 * changing either.
		 */
		~when_all_awaiter()
		{
			// First, and see above. Nothing can reach the list in the window that
			// swapping these two opens -- measured, no test fails on it -- but what keeps
			// that true is an argument about which party resumes a joiner and who holds a
			// reference while that happens, not anything local to this frame. This order is
			// the local half, and it is the half each of the three changes named above would
			// leave standing.
			m_join.reset();

			if(m_state != nullptr)
			{
				m_state->shared().release();
			}
		}

		/**
		 * Unconditionally false, so the block is allocated and the children started in
		 * await_suspend, which is the only place that has the awaiting handle. A
		 * when_all with no children therefore still allocates -- the block, and
		 * whatever the stop source the block holds for the children costs, which on
		 * libstdc++ is a stop state -- and enters a suspension point it returns from
		 * without parking. alt::when_all lists what allocates.
		 */
		// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the compiler calls this through an awaiter instance at every co_await of a when_all; static here would move readability-static-accessed-through-instance to those call sites instead.
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		/**
		 * @brief Allocates the block, fans the children out, and parks on the counter.
		 *
		 * DELICATE. Read this before changing it. Every statement's position is
		 * load-bearing.
		 *
		 * @return False when the wait is already over -- every child retired before the
		 *         join could link -- so the awaiting coroutine carries straight on.
		 */
		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h)
		{
			m_state = std::make_unique<when_all_state<Slots>>(child_count()).release();

			start_options child_options{};
			child_options.on_completion = {.invoke = &on_combinator_child_complete, .state = &m_state->shared()};

			if constexpr(relays_cancellation<Promise>)
			{
				if(cancellation_context* const context = h.promise().cancellation_context_pointer(); context != nullptr)
				{
					// The children inherit the chain's home, so a child holding
					// thread-affine state is destroyed on the thread start_options::home
					// promises. The shared block is what makes that safe: the chain may
					// be gone long before the home is drained.
					child_options.home = context->home;

					// Before any child starts, and that order is measured rather than
					// assumed. A stop that lands during the fan-out -- through an
					// already-stopped token here, or from another party mid-loop -- marks
					// the block cancelled and stops the child source, so children started
					// below die at their first suspension point instead of running work
					// the chain will never read.
					//
					// The guard of record is
					// WhenAllTest.AStopDuringTheFanOutStopsTheChildrenStartedAfterIt,
					// where the first child requests the stop from inside its own start
					// and the second counts whether its awaitable was reached at all:
					// moving this line below the loop fails it 200/200 in debug and
					// 200/200 under ThreadSanitizer. It fails
					// CancellingTheAwaitingChainRequestsEveryChildsStop 0/200, that test
					// requesting its stop long after await_suspend has returned, so do not
					// read that one as covering this.
					m_state->shared().link(context->token);
				}
			}

			// DELICATE. The children observe the BLOCK's stop source, not the chain's
			// token, and the two are not interchangeable even though both are stop
			// tokens and the chain's token is what ends up stopping the children anyway.
			//
			// m_children is the source combinator_state::cancel() holds its counted copy
			// of. Assign context->token here instead -- which means moving this line up
			// into the block above, the only place context is in scope -- and each
			// child's stop_forwarder is registered on the chain's own token, in the same
			// callback list as this block's m_link. libstdc++ walks that list in reverse
			// registration order, so the children are torn down before m_link runs; a
			// child's teardown can then drop the block's last reference, and destroying
			// the block destroys an m_link whose ~stop_callback releases the last
			// ownership reference to a stop state whose request_stop() is still on the
			// stack. cancel()'s counted copy does not save it: that copy is of m_children,
			// which under such a change nobody is registered on.
			//
			// Measured, and only one test in the suite sees it:
			// WhenAllTest.ANestedWhenAllInsideAChildIsItsOwnRoot fails 0 pass / 91 fail /
			// 109 hang out of 200 repeats in debug, and 0 pass / 200 fail under
			// AddressSanitizer, where the report is heap-use-after-free in
			// _Stop_state_t::_M_lock() reached from stop_forwarder::operator() -- an
			// instrumented access to a freed stop state's own word, which is why the
			// tool sees this one; combinator_state::cancel's note says why that differs
			// from a claim on a freed waiter_list, which it would not see. The other
			// four combinator tests added alongside it pass 200/200. Re-measured against
			// the 35-test suite this file now has, at 20 repeats rather than 200: 0 pass /
			// 9 fail / 11 hang, the same character. A whole-suite run in debug prints 33
			// passes -- every test ahead of that one in the file -- and then dies in it, so
			// the one test after it never runs at all. One level of nesting is what makes
			// the hazard observable, because what gets freed mid-walk is a child's own stop
			// state, held alive only by an inner block's m_link once the child's frame is
			// gone.
			//
			// The other half of that mutation, relaying the chain's cancellation_context
			// into each child instead of giving it the block's source, is inexpressible
			// rather than merely unmeasured: cancellable_task_promise::configure
			// (cancellable_task.hpp) always installs its own m_source.get_token() as the
			// context token and only links an external one, so a cancellable_task cannot
			// be handed a parent context to relay. Do not re-attempt it.
			child_options.token = m_state->shared().children_token();

			// One add_ref per child, before that child is started, for the same reason
			// task_group::spawn raises its count before start: a body with no suspension
			// point completes inside start() and fires its hook there.
			// No rollback if start throws, exactly as task_group::spawn records. Here the
			// consequence is a leaked block, which alt::when_all documents.
			start_children(child_options);

			// Retire the starter. Until this returns no child can have driven the count
			// to zero, and nothing resumes here either: no joiner is parked yet.
			//
			// That this happens after the loop rather than before it is defensive, and
			// measured to be unobservable in stage A: the park below is strictly after
			// the fan-out, so no joiner exists for an early zero to resume, and
			// join_awaiter::ready() is re-read under the list's mutex, so an early zero
			// only refuses the link. Moving this above the loop fails nothing -- 0/200
			// debug and 0/200 TSan against the one fan-out test, and 0 across the whole
			// 35-test WhenAllTest suite at 20 repeats in debug and 10 under
			// AddressSanitizer -- while deleting it fails 20/20. See the combinator_state
			// constructor for why the count stays anyway.
			m_state->shared().counter().complete();

			// Park on the counter. completion_counter::join_awaiter is reused as-is rather
			// than hand-rolling a single-joiner path: the resumption election between
			// "the last child retired" and "the chain was cancelled" is exactly what
			// waiter_list::claim already decides, and a bespoke election would be that
			// protocol rewritten without waiter_list's tests. The cost is one mutex and
			// one node per combinator for a queue that holds at most one waiter, which
			// is cheaper than the proof.
			//
			// join_awaiter registers its own waiter_stop_release on this chain's token
			// and re-reads outstanding() under the list's mutex, so a count that already
			// reached zero refuses the link and this returns false. Nothing may be read
			// from *this after the call: a successful link publishes the node, and any
			// party may then resume this frame and destroy it.
			// That this parks until the LAST child retires, and does not return false
			// once the first exception is in a slot, is what lets a successful sibling
			// finish before the rethrow. Measured by replacing the park with
			// `return false`, in debug: WhenAllTest.ASuccessfulSiblingRunsToItsEndAfterAnotherChildThrows
			// fails 200/200, on its EXPECT_FALSE(returned) assertion -- the await returning
			// while the sibling is still parked -- and on its frames_at_return assertion,
			// which reads live_frames at the instant the await returned and finds the
			// sibling's frame still alive. Not on the exception assertions. Its first
			// revision raced a releasing thread against the rethrow and failed only 8/200
			// and 4/200 of the same mutation, on its sibling-finished assertion, which is
			// why it is single-threaded now.
			//
			// The park is load-bearing far beyond that one test. Run each of the 35
			// WhenAllTest tests alone against the same mutation and they split 16 / 1 /
			// 18. Sixteen abort outright, all on one assert -- result_store's "reading an
			// alt coroutine result before it was produced" in coroutine.hpp -- because a
			// chain that carries on without parking assembles slots no child has filled.
			// One fails: the test named above, on the assertions named above. Together
			// those 17 are exactly the tests whose children park: every test in which a
			// child's completion, a stop or a scheduler drain is what resumes the chain.
			// The 18 that still pass are the ones whose children all retire inside the fan-out,
			// where the park has nothing to wait for, which is also why
			// EveryChildAlreadySatisfiedCompletesWithoutSuspending cannot guard this. The
			// list is reproducible from this paragraph rather than cited: no measurement
			// report ships with the library.
			m_join.emplace(m_state->shared().counter());
			return m_join->await_suspend(h);
		}

		/**
		 * @brief The outcome: every result positionally, or the released error.
		 *
		 * The verdict does not come from the counter alone. A cancelled child still
		 * fires its completion hook, so the children's stop can drive the count to zero
		 * and resume this chain through the *satisfied* path with every slot unfilled.
		 * combinator_state::cancel() setting its flag before requesting the children's
		 * stop is what makes the two tellable apart here.
		 *
		 * DO NOT delete the cancelled() test as dead code. It is defensive against an
		 * order the standard leaves unspecified, and the measurements say both halves of
		 * that plainly:
		 *
		 * - Dropping it alone fails nothing, 0/200 in debug and 0/200 under
		 *   ThreadSanitizer. Whenever the chain's token is stopped before the zero
		 *   transition claims the joiner, the joiner's waiter_stop_release has claimed its
		 *   node, so the verdict is already !waited: a token stopped before the park
		 *   (the fan-out test's path, where m_link fired long before any joiner existed)
		 *   makes registering the callback in wait_awaiter::await_suspend claim the node,
		 *   waiter_list::link then refuses it in the claim election without evaluating
		 *   the condition, and m_released is set; a stop after the park claims and
		 *   resumes the parked node.
		 * - Nothing in [stopcallback] fixes that order. Register the link after the join
		 *   park instead -- making cancel() run first, which is what any other order
		 *   amounts to -- and dropping this test fails 200/200: a satisfied verdict over
		 *   unfilled slots, then result_store's "reading an alt coroutine result before it
		 *   was produced" assert. With the test present, that same reordering passes.
		 *
		 * A stop that races a completion is reported as a cancellation. If the last child
		 * retires legitimately, the zero transition wins the claim, and the chain's stop
		 * then lands before this reads the flag, every slot is filled and the result is
		 * still std::errc::operation_canceled: the cancelled() test discards a fully
		 * assembled satisfied result as well as guarding unfilled slots.
		 *
		 * May rethrow a child's exception, lowest position first.
		 */
		[[nodiscard]] std::expected<Results, std::error_code> await_resume()
		{
			// m_join is engaged by await_suspend, which always runs first: await_ready is
			// unconditionally false, so there is no path to a resumption without it.
			// Asserted rather than left implicit because clang-tidy-19's
			// bugprone-unchecked-optional-access cannot see across the two functions.
			assert(m_join.has_value() && "resuming an alt when_all that never suspended");

			const alt::wait_result waited = m_join.value().await_resume();

			if(!waited || m_state->shared().cancelled())
			{
				return std::expected<Results, std::error_code>{std::unexpect, wait_released_error()};
			}

			// Satisfied and not cancelled, so every child's body ran to its end and
			// filled its slot: the only thing that stops a child body is cancel(), and
			// that sets the flag tested above. The block is still alive because this
			// awaiter has not been destroyed, which is where its reference goes.
			return assemble_combinator_results<Results>(std::move(m_state->slots()));
		}

	private:
		/** True for the range form, whose storage is the range itself and not a tuple. */
		static constexpr bool is_range_form = std::ranges::range<Storage>;

		/** How many children this combinator fans out. */
		[[nodiscard]] std::size_t child_count() const
		{
			if constexpr(is_range_form)
			{
				return static_cast<std::size_t>(std::ranges::size(*m_storage));
			}
			else
			{
				return std::tuple_size_v<Storage>;
			}
		}

		/** Starts every child, in argument order for a tuple and in range order for a range. */
		void start_children(const start_options& options)
		{
			if constexpr(is_range_form)
			{
				// The range is iterated here and nowhere else, once. Slot i belongs to the
				// i-th element, so the children are started, and their slots filled, in
				// range order and assembly reads them back in that order.
				//
				// alt::start's cancellation_handle is discarded here and in start_child,
				// deliberately: nothing in stage A cancels a single child, so the only
				// thing the handle could do is stop one of them on its own, which the
				// children's shared stop source exists to prevent. The children are
				// self-owning roots, so dropping the handle leaks nothing.
				using element_type = std::ranges::range_value_t<Storage>;

				[[maybe_unused]] const std::size_t count = child_count();
				std::size_t                        index = 0;

				for(auto it = std::ranges::begin(*m_storage); it != std::ranges::end(*m_storage); ++it, ++index)
				{
					// A conforming sized_range cannot yield more elements than its size:
					// the slots and the counter were both sized by it.
					assert(index < count && "a sized range yielded more elements than its size");

					m_state->shared().add_ref();

					// The element is constructed from the iterator's rvalue reference in
					// place of the child's by-value parameter, so the child's frame owns
					// it and the caller's range is left holding moved-from elements.
					static_cast<void>(alt::start(
					  when_all_child<element_type>(element_type(std::ranges::iter_move(it)), &m_state->slots()[index], &m_state->shared()), options));
				}

				// Fewer elements than size() would leave the counter above zero forever.
				assert(index == count && "a sized range yielded fewer elements than its size");
			}
			else
			{
				[&]<std::size_t... I>(std::index_sequence<I...>) {
					(start_child<I>(options), ...);
				}(std::make_index_sequence<std::tuple_size_v<Storage>>{});
			}
		}

		/** Raises the count for child @p I and starts it. */
		template<std::size_t I>
		void start_child(const start_options& options)
		{
			// The type the argument tuple stored: a value for an rvalue argument, moved
			// into the child's frame, and T& for an lvalue one.
			using child_type = std::tuple_element_t<I, Storage>;

			m_state->shared().add_ref();

			static_cast<void>(alt::start(when_all_child<child_type>(std::forward<child_type>(std::get<I>(*m_storage)),
			                                                        &std::get<I>(m_state->slots()),
			                                                        &m_state->shared()),
			                             options));
		}

		/** The caller's awaitables or range, owned by the awaitable object this came from. */
		Storage* m_storage;

		/** The shared block, from await_suspend until this awaiter is destroyed. */
		when_all_state<Slots>* m_state = nullptr;

		/** This chain's place in the block's completion counter. */
		std::optional<completion_counter::join_awaiter> m_join;
	};

	/**
	 * @brief Owns the awaitables a when_all was given until the await completes.
	 *
	 * Stored by value inside detail::cancellation_checked_awaitable, which lives for
	 * the whole full-expression, so the pointer the awaiter holds into this tuple is
	 * stable -- the same arrangement detail::expected_awaitable uses.
	 */
	template<typename... Awaitables>
	class [[nodiscard]] when_all_awaitable
	{
	public:
		/** The tuple of values a satisfied wait yields. */
		using results_type = std::tuple<when_all_value_t<Awaitables>...>;

		/** The tuple of result stores the children write into. */
		using slots_type = std::tuple<when_all_slot_t<Awaitables>...>;

		/** The tuple the arguments are stored in: a value per rvalue, a reference per lvalue. */
		using awaitables_type = std::tuple<Awaitables...>;

		/** Takes the awaitables, moving each rvalue and binding each lvalue. */
		explicit when_all_awaitable(Awaitables&&... awaitables): m_awaitables(std::forward<Awaitables>(awaitables)...)
		{
		}

		/** The awaiter, pointing at this object's tuple. Rvalue-only: it is a temporary. */
		auto operator co_await() && noexcept
		{
			return when_all_awaiter<results_type, slots_type, awaitables_type>{&m_awaitables};
		}

	private:
		awaitables_type m_awaitables;
	};

	/**
	 * @brief Owns the range a when_all was given until the await completes.
	 *
	 * The range form's counterpart of detail::when_all_awaitable. @p Range is the
	 * deduced forwarding type: a value type owns the range here, an lvalue reference
	 * binds the caller's. Either way the elements are moved out, never copied; see
	 * alt::when_all.
	 */
	template<typename Range>
	class [[nodiscard]] when_all_range_awaitable
	{
	public:
		/** The type each element is moved into a child as. */
		using element_type = std::ranges::range_value_t<Range>;

		/** The vector of values a satisfied wait yields. */
		using results_type = std::vector<when_all_value_t<element_type>>;

		/** The vector of result stores the children write into. */
		using slots_type = std::vector<when_all_slot_t<element_type>>;

		/** Takes the range, moving an rvalue and binding an lvalue. */
		explicit when_all_range_awaitable(Range&& range): m_range(std::forward<Range>(range))
		{
		}

		/** The awaiter, pointing at this object's range. Rvalue-only: it is a temporary. */
		auto operator co_await() && noexcept
		{
			return when_all_awaiter<results_type, slots_type, std::remove_reference_t<Range>>{&m_range};
		}

	private:
		std::conditional_t<std::is_lvalue_reference_v<Range>, Range, std::remove_cvref_t<Range>> m_range;
	};

}

namespace alt
{

	/**
	 * @brief Awaits every argument concurrently and yields every result positionally.
	 *
	 * Each awaitable runs inside its own alt::cancellable_task, so the children run
	 * concurrently and each is its own cancellation root. Awaiting the result yields
	 * @c std::expected<std::tuple<detail::when_all_value_t<Awaitables>...>,
	 * std::error_code>: position @c i holds what awaiting argument @c i produced, with
	 * a void child contributing @c std::monostate and a reference child a
	 * @c std::reference_wrapper, since std::expected cannot hold a reference.
	 *
	 * Cancellation is an observable *result*, not a teardown. When the awaiting chain
	 * is cancelled the await yields @c std::errc::operation_canceled and the awaiting
	 * body runs on, because the check ahead of a suspension point cannot fire on the
	 * way out of one; the body is torn down at its next suspension point as usual. Test
	 * for it the way alt::wait_result documents:
	 *
	 *     const auto results = co_await alt::when_all(fetch(), load());
	 *
	 *     if(!results && results.error() == std::errc::operation_canceled)
	 *     {
	 *         // the chain was cancelled; the children have been asked to stop
	 *     }
	 *
	 * A child's exception is rethrown at the await, the lowest position's first, and
	 * the other children's results are then discarded with the slots.
	 *
	 * **A child that fails to start leaks the shared block.** If starting child @c i
	 * throws -- its coroutine frame allocation failing is the realistic case -- the
	 * exception propagates out of the await and nothing releases the reference that
	 * child was given, so the block, its result slots and the children's stop source are
	 * never freed. Children @c 0 to @c i-1 are already running and run to completion
	 * unobserved; their results are discarded. There is no hang, because the chain never
	 * parks. This is the same no-rollback contract as alt::task_group::spawn, and a
	 * caller sizing an out-of-memory budget should count it as a leak per failed start.
	 *
	 * One precondition, in the family's existing shape: **an awaitable passed as an
	 * lvalue, and everything it refers to, must outlive the children** -- which with an
	 * alt::start_options::home scheduler means outliving its drain, since the children's
	 * frames stay alive until that scheduler runs their teardown. An awaitable passed as
	 * an rvalue is moved into its child's frame, which owns the object -- but not what
	 * the object refers to, which must still outlive the children. The lvalue case
	 * is not made safe by copying, because alt::task is not copyable, which is the
	 * point of the distinction.
	 *
	 * **Passing the same awaitable object twice is a precondition violation.** Two
	 * children would then await one object concurrently, which no awaitable in the
	 * library supports, which needs lvalues. Moving one named awaitable into two
	 * positions is no better: it compiles, and the second child gets a moved-from
	 * object. Nothing checks for either.
	 *
	 * What awaiting this allocates, written as a list of what allocates rather than a
	 * total, because a total would be a claim about the standard library's internals:
	 * the shared block, one make_unique, with the slot tuple inline in it; the stop
	 * state of the stop source the block holds for the children, which a
	 * default-constructed std::stop_source allocates on libstdc++ and an
	 * implementation storing it inline would not; and per child a coroutine frame
	 * plus that child's own stop state, each child being a cancellable_task root
	 * whose promise holds a std::stop_source of its own. With no arguments at all
	 * there are no children, so the block and its stop state are all that is left:
	 * not free, though the result is then an empty tuple rather than a cancellation.
	 *
	 * The children inherit the awaiting chain's home scheduler. The shared block is
	 * what makes that safe: it is reference counted, held by every child and by the
	 * awaiter, and so outlives the chain, which may be gone long before the home
	 * scheduler runs a child's teardown. The children share one stop source: nothing
	 * cancels a single child on its own.
	 *
	 * **Each child is its own cancellation root**, and the reason this is safe rests on
	 * three invariants. **(A) Ownership.** No combinator owns or destroys a child frame;
	 * the children are self-owning alt::cancellable_task roots, and a combinator only
	 * ever requests their stop. **(B) The claim election cannot free an already-claimed
	 * node:** a loser whose node a satisfier already won loses the compare-and-swap in
	 * waiter_list::claim, so its waiter_stop_release resumes and frees nothing; the only
	 * way a child frame dies is the child itself running, which happens only when the
	 * walk reaches it. **(C) Lifetime overlap.** A combinator may resume the awaiting
	 * chain -- and so let that chain destroy the combinator's own frame -- while children
	 * are still alive; this is why the shared state is reference counted off the frame
	 * instead of owned by it, and (A) is what keeps the sibling hazard away across that
	 * overlap, the children that outlive the frame destroying nothing but themselves. The
	 * argument is recorded where the hazard is: see the note on alt::async_latch, which
	 * states the same three under the same labels, and which also says why alt::when_any
	 * fits the same argument unchanged: it carries a copy of this component's completion
	 * hook, parks one joiner on the same detail::completion_counter, and keeps the same
	 * park-until-zero discipline, so it adds no overlap this component does not already
	 * have.
	 *
	 * A when_all awaited in a promise with no cancellation context -- an alt::task
	 * driven by alt::sync_wait, with no alt::cancellable_task root above it -- is simply
	 * uncancellable. There is no token to link, so nothing can stop the children and the
	 * await can only report the satisfied result. That is not a special case in this
	 * component: it is what "cancellation is relayed from a root" means for every
	 * awaitable in the library.
	 */
	template<typename... Awaitables>
	  requires(awaitable<Awaitables> && ...)
	[[nodiscard]] auto when_all(Awaitables&&... awaitables)
	{
		return detail::when_all_awaitable<Awaitables...>{std::forward<Awaitables>(awaitables)...};
	}

	/**
	 * @brief Awaits every element of @p range concurrently and yields a vector of results.
	 *
	 * The range form of the argument-list overload above, with the same children, the
	 * same cancellation result and the same exception rule. Awaiting the result yields
	 * @c std::expected<std::vector<V>, std::error_code> with
	 * @c V = detail::when_all_value_t<std::ranges::range_value_t<R>>, element @c i holding
	 * what awaiting element @c i produced, in range order. A void element contributes
	 * @c std::monostate, so the vector's element type is uniform. An empty range yields an
	 * empty vector, not a cancellation, and still allocates the shared block and the stop
	 * state of the block's stop source.
	 *
	 * This form allocates what the overload above lists, with a vector of slots in place
	 * of the inline slot tuple and the result vector the await yields alongside it. Both
	 * vectors are sized from the range, so an empty range allocates neither.
	 *
	 * **Every element is moved into its child, so the child owns it.** A
	 * @c std::vector<alt::task<int>> therefore yields a @c std::vector<int>: the value
	 * is moved out of the child's frame before it dies. Because of that this overload
	 * does not inherit the lvalue precondition of the argument-list form -- nothing the
	 * children run refers into the caller's range. An rvalue range is owned by the
	 * returned awaitable; an lvalue range is bound, not copied, and is left holding
	 * moved-from elements, so it must not be awaited twice. A const range is accepted
	 * only if its elements are copyable, and they are then copied. A range of references
	 * to awaitables the caller keeps alive is out of scope.
	 *
	 * **The range must be a std::ranges::sized_range, not merely an input range.** The
	 * child count is needed before the first child starts, because the shared block's
	 * counter is constructed with the children plus the starter. Infinite and streaming
	 * ranges are a non-goal and are excluded by this. The range is iterated once.
	 *
	 * A type that is both an awaitable and a sized range is one awaitable child, not a
	 * range of children: the argument-list overload takes it.
	 *
	 * The leak when starting a child throws, and the home and stop-source sharing, are
	 * the argument-list form's; see there.
	 */
	template<std::ranges::sized_range R>
	  requires(!awaitable<R>) && awaitable<std::ranges::range_value_t<R>> &&
	          std::constructible_from<std::ranges::range_value_t<R>, std::ranges::range_rvalue_reference_t<R>>
	[[nodiscard]] auto when_all(R&& range)
	{
		return detail::when_all_range_awaitable<R>{std::forward<R>(range)};
	}

}

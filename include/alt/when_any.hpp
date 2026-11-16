#pragma once

#include <alt/awaitable.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/async_wait.hpp>
#include <alt/detail/cancellation.hpp>
#include <alt/detail/combinator_election.hpp>
#include <alt/detail/combinator_results.hpp>
#include <alt/detail/combinator_state.hpp>
#include <alt/detail/completion_counter.hpp>
#include <alt/detail/coroutine.hpp>
#include <alt/detail/when_any_result.hpp>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace alt::detail
{

	/**
	 * The slot position i of a when_any over A writes into.
	 *
	 * The same store detail::when_all_slot_t names, over the same value mapping: a
	 * when_any child's result is stored exactly as a when_all child's is, and only the
	 * reading differs. Spelled out here rather than reused under when_all's name so
	 * that nothing in this component has to be read as a when_all.
	 */
	template<alt::awaitable A>
	using when_any_slot_t = result_store<when_any_value_t<A>>;

	/**
	 * @brief The shared block, the result slots and the election one when_any needs.
	 *
	 * detail::when_all_state with the election added. Owns the block by composition
	 * rather than by inheritance, for the reason that class gives, and hands the block
	 * the same erased destroy function, which is what makes the block's reference count
	 * the lifetime of the slots and of the election too.
	 *
	 * Allocated by when_any_awaiter::await_suspend and freed by nothing else: the last
	 * reference to the block frees it, whichever party drops it.
	 */
	template<typename Slots>
	class when_any_state
	{
	public:
		/** Constructs the slots, the election and a block with @p children outstanding plus the starter. */
		explicit when_any_state(const std::size_t children):
		  m_slots(make_combinator_slots<Slots>(children)),
		  m_shared(children, this, &destroy)
		{
		}

		/** Destroys the block first, then the election, then the slots. See the note on the members. */
		~when_any_state() = default;

		when_any_state(const when_any_state&)            = delete;
		when_any_state& operator=(const when_any_state&) = delete;
		when_any_state(when_any_state&&)                 = delete;
		when_any_state& operator=(when_any_state&&)      = delete;

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

		/** The election that decides which slot the awaiter reads. */
		[[nodiscard]] combinator_election& election() noexcept
		{
			return m_election;
		}

	private:
		/** Frees this object. Handed to the block as its erased destroy function. */
		static void destroy(void* owner) noexcept
		{
			// Reclaimed through a std::unique_ptr rather than a bare delete, which
			// cppcoreguidelines-owning-memory rejects for a pointer that is not a
			// gsl::owner. The block is freed as this goes out of scope.
			const std::unique_ptr<when_any_state> reclaimed{static_cast<when_any_state*>(owner)};
		}

		// DELICATE. Declaration order is load-bearing here, exactly as in
		// detail::when_all_state and for the reason that class's note gives.
		//
		// m_shared is declared last, so the block -- and with it the block's m_link,
		// whose destructor blocks until a cancel() forwarded on another thread has
		// returned -- is destroyed first, before the members that cancel() reaches
		// through the children are gone.
		//
		// The election is on that list as well as the slots, and for a reason that is
		// this component's rather than when_all's: a child that cancel() stops while it
		// is parked is resumed released rather than torn down, so it runs on, writes its
		// slot and then calls m_election.elect() -- observed by
		// WhenAnyTest.ACancelledRaceReportsOperationCanceled, see when_any_child. Both
		// members are therefore touched from inside cancel()'s walk, and both must
		// outlive it.
		Slots               m_slots;
		combinator_election m_election;
		combinator_state    m_shared;
	};

	/**
	 * @brief Builds an argument-list when_any's result out of the winning slot.
	 *
	 * Walks the positions at compile time until @c I is the one the election named, so
	 * a run-time winner selects a compile-time std::in_place_index. That is what the
	 * indexed result requires: alt::when_any_result reads by position, never by type,
	 * because duplicate alternatives are expected.
	 *
	 * The slot's value is MOVED out, which is not a micro-optimization: the const
	 * lvalue alt::when_any_result::get yields a @c const @c T&, so a winner that is
	 * move-only -- alt::task<std::unique_ptr<T>> being the case that proves it --
	 * reaches the caller only if it is moved out here. result_store::value() rethrows
	 * a stored exception instead of returning, which is where a winner that completed
	 * by throwing is rethrown at the await.
	 *
	 * **@p winner is in range by construction, so nothing here checks it.** The three
	 * things that make that true are all elsewhere: the positions handed to elect() are
	 * @c I from make_index_sequence<tuple_size_v<Storage>> in start_child, the slot
	 * tuple is the same length by make_combinator_slots's own assert, and
	 * when_any_awaiter::await_resume rules out no_winner before calling. So the walk
	 * cannot fall off the end, and the final branch below is position @c I by
	 * elimination rather than by assumption. An assert here would be one that cannot
	 * fire; it was written and removed for that reason.
	 *
	 * This function is also where an argument-list when_any over NO awaitables is
	 * rejected, and it is rejected at COMPILE time by the static_assert below -- see
	 * when_any_awaiter::await_suspend, which distinguishes that from the range form's
	 * empty case.
	 *
	 * @param slots  The children's slots. Only the winner's is read.
	 * @param winner The elected position. Must be a position of @p slots.
	 */
	template<typename Result, std::size_t I = 0, typename Slots>
	[[nodiscard]] Result when_any_result_at(Slots& slots, const std::size_t winner)
	{
		// A when_any over no awaitables, which cannot be answered at run time: this
		// function is instantiated by await_resume whatever await_suspend returned, so
		// an early return there could not rescue a zero-length tuple. The range form's
		// empty case is a different matter and is not caught here.
		static_assert(std::tuple_size_v<Slots> > 0, "a when_any over no awaitables can elect no winner, so there is no slot to assemble from");

		if constexpr(I + 1 < std::tuple_size_v<Slots>)
		{
			if(winner == I)
			{
				return Result{std::in_place_index<I>, std::move(std::get<I>(slots)).value()};
			}

			return when_any_result_at<Result, I + 1>(slots, winner);
		}
		else
		{
			// The last position, reached once every earlier one has been ruled out. See
			// the note above for why that makes it the winner's rather than a guess.
			return Result{std::in_place_index<I>, std::move(std::get<I>(slots)).value()};
		}
	}

	/**
	 * @brief Builds a when_any's result out of the winning slot, in either form.
	 *
	 * The argument-list form's winner is a variant alternative and needs the
	 * compile-time walk above; the range form's is a run-time index beside one value
	 * type, which is why alt::when_any_range_result is a class of its own.
	 *
	 * @p winner indexes the range form's vector unchecked, and is in range for the
	 * counterpart of the reason the walk above gives: the positions handed to elect()
	 * are the loop index in start_children, which asserts itself under the range's own
	 * size, and the vector was sized from that same size. An assert here would be one
	 * that cannot fire.
	 */
	template<typename Result, typename Slots>
	[[nodiscard]] Result assemble_when_any_result(Slots& slots, const std::size_t winner)
	{
		if constexpr(tuple_like_slots<Slots>)
		{
			return when_any_result_at<Result>(slots, winner);
		}
		else
		{
			return Result{winner, std::move(slots[winner]).value()};
		}
	}

	/**
	 * @brief The completion hook every child of a when_any carries.
	 *
	 * @p state is the detail::combinator_state the child shares with its siblings and
	 * with the awaiting chain. Fired from the child's frame destruction, which is the
	 * only point both the normal and the cancelled path pass through.
	 *
	 * The same two statements in the same order as detail::on_combinator_child_complete
	 * in when_all.hpp. That is where the measured account of why the order is
	 * load-bearing lives -- a swap is invisible in a debug build and 200/200 under
	 * AddressSanitizer there -- and it is not restated here. Two things to carry away
	 * from it rather than re-derive:
	 *
	 * - complete() may reach zero and resume the awaiting chain on this thread, whose
	 *   body may drop its own reference. release() must follow it, and nothing may be
	 *   read from shared after release(): this may be the last one.
	 * - The order is what ~when_any_awaiter's note below rests on, as it is what
	 *   ~when_all_awaiter's rests on: complete() before release() means the child that
	 *   drove the count to zero still holds a reference while the chain runs on.
	 *
	 * This is a second copy rather than a call into when_all.hpp, so that including
	 * alt/when_any.hpp does not drag a whole unrelated component in. The cost is that
	 * the two must be changed together: revisiting the order there means revisiting it
	 * here. **That constraint is recorded on both sides** -- the note on
	 * on_combinator_child_complete names this function and says the order cannot be
	 * revisited there alone -- because when_all.hpp is where a reader looking to change
	 * the order starts, holding as it does the measurement.
	 */
	inline void on_when_any_child_complete(void* state) noexcept
	{
		auto* const shared = static_cast<combinator_state*>(state);

		shared->counter().complete();
		shared->release();
	}

	/**
	 * @brief Runs one child of a when_any as its own cancellation root, and elects.
	 *
	 * The awaitable is taken by value in this frame, with @c A the type the
	 * combinator's argument tuple stored: a value for an rvalue argument, which this
	 * frame then owns, and @c T& for an lvalue argument, which the caller must keep
	 * alive for as long as the children run.
	 *
	 * **The slot is written before elect() is called, and that order is a
	 * requirement rather than a convenience.** detail::combinator_election's note
	 * states it as fact and builds its publication argument on it: the slot write is
	 * ordered before the successful compare-exchange's release, so a reader that goes
	 * through winner() and sees a position synchronizes with that release and sees the
	 * slot. Electing first and storing afterwards would make that note false.
	 *
	 * A consequence worth stating, because it is the cost of that order: EVERY child
	 * writes its slot, not only the winner, since a child cannot know it has won until
	 * it has elected. A loser's slot is written and never read -- the awaiter reads the
	 * elected position and nothing else -- so a loser's value is still discarded, just
	 * not unwritten. No slot is shared: each child has its own, so two children
	 * writing concurrently write to different objects.
	 *
	 * A stopped child does not always skip the election, and the two cases differ. One
	 * already PARKED on an alt wait primitive is resumed *released* rather than torn
	 * down -- that is what those primitives do with a parked waiter -- so it runs on,
	 * writes a released value into its slot and reaches this election too. One stopped
	 * before it reached any suspension point is torn down at its first one and never
	 * resumes, so it reaches neither the store nor the election; that is the child
	 * started after a winner has already stopped the source. The first case is why a
	 * winner is elected on the cancelled path as well, and why
	 * when_any_awaiter::await_resume tests the cancellation flag ahead of the election
	 * rather than after it.
	 *
	 * That first case is OBSERVED rather than asserted:
	 * WhenAnyTest.ACancelledRaceReportsOperationCanceled races two children whose
	 * bodies increment a counter in the statement after their wait, and both increments
	 * land on the cancelled path. From there to the election is the straight-line code
	 * below -- store, then elect, with no suspension point between -- so both children
	 * reached it. What that test cannot observe, and what await_resume's note marks as
	 * arranged but unwitnessed, is the elected winner then being discarded.
	 *
	 * A child that completes by throwing stores the exception in its slot and still
	 * elects, so it can win: the exception is then rethrown at the await, by
	 * result_store::value() inside the assembly above.
	 *
	 * @p state is used, unlike detail::when_all_child's: a winner stops its siblings
	 * through it. request_children_stop() rather than cancel(), because setting the
	 * cancellation flag on the success path would make cancelled() true on every
	 * successful race.
	 */
	template<typename A, typename Slot>
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): A is a reference exactly when the caller passed an lvalue, which the when_any overloads document as a precondition the caller must honor: the awaitable and everything it refers to outlive the children. Slot, state and election all point into the refcounted block, which outlives every child by construction.
	cancellable_task when_any_child(A awaitable, std::size_t position, Slot* slot, combinator_election* election, combinator_state* state)
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

		// The slot is filled on every path above, so the election below cannot name a
		// position whose slot is unset.
		if(election->elect(position))
		{
			// Only from a winner, and only here. This walk resumes every still-parked
			// sibling, released, on this thread and before it returns -- but it does not
			// follow that every sibling is GONE when it returns. A sibling that runs on
			// and reaches a further suspension point is torn down there by
			// abandon_chain, which POSTS when start_options::home is set, so its frame
			// can outlive this call by as long as that scheduler takes to drain.
			//
			// Neither conclusion below depends on that, which is why the qualification
			// is only wording. It cannot free the block from under this frame: this
			// child's own reference is dropped by its completion hook, which has not run
			// yet. Nor can it resume the awaiting chain, because the counter still holds
			// this child's own item -- a sibling whose teardown is posted has not
			// retired either, so it holds one too.
			state->request_children_stop();
		}
	}

	/**
	 * @brief The awaiter alt::when_any suspends the awaiting chain on.
	 *
	 * @tparam Result  What a satisfied race yields: an alt::when_any_result for the
	 *                 argument-list form, an alt::when_any_range_result for the range
	 *                 form.
	 * @tparam Slots   The tuple (or vector) of result stores the children write into.
	 * @tparam Storage What the awaitable object holds: a std::tuple of awaitables for
	 *                 the argument-list form, or the range itself for the range form,
	 *                 whose Slots is then a vector.
	 *
	 * Allocates the shared block on suspension, fans the children out as self-owning
	 * alt::cancellable_task roots, and parks the awaiting chain on the block's
	 * completion counter. detail::when_all_awaiter's shape exactly, with the election
	 * added and the assembly replaced. Neither copyable nor assignable; moved exactly
	 * once, into detail::cancellation_checked's storage, before any await member runs.
	 *
	 * **The await returns only once every child has retired, not when the winner is
	 * elected.** The counter is what resumes the chain, and the losers retire into it
	 * like the winner. So a won race leaves no child running, which is what
	 * WhenAnyTest.NoChildIsRunningWhenAWonAwaitReturns reads off live_frames at the
	 * instant the await returns. Returning on the election instead would hand the
	 * caller a result while sibling frames were still alive, and would need a
	 * resumption mechanism this component does not have.
	 *
	 * **EXACTLY ONE JOINER PARKS ON THE BLOCK'S COUNTER**, and that is a property of
	 * this design rather than an accident of this file. It is recorded here because
	 * another component's assert depends on it. The three parties:
	 *
	 * - The awaiting chain, parked by await_suspend below. The only
	 *   completion_counter::join_awaiter this component constructs is the m_join member;
	 *   the library's only other two are detail::when_all_awaiter's, over its own block's
	 *   counter, and alt::task_group's, over a counter of its own. Verified by
	 *   enumerating every reference to combinator_state::counter() and to join_awaiter in
	 *   include/ and src/, not by reading this component alone.
	 * - The children park on their OWN awaitables, each on a node in its own frame and
	 *   on some other primitive's waiter_list. None of them touches
	 *   combinator_state::counter() except through the completion hook, which retires an
	 *   item and parks nothing.
	 * - combinator_state::request_children_stop(), which a winner calls, parks nobody:
	 *   it requests a stop.
	 *
	 * What that buys is the position of waiter_list::claim's liveness assert, which
	 * sits AHEAD of the election rather than after it, so that even a claim which would
	 * lose -- and therefore touch nothing at all -- aborts on a destroyed list instead
	 * of returning quietly. That position is only safe while no such late losing claim
	 * can reach a list that has already been freed. With one joiner it cannot: the
	 * single party that can make one is that joiner's own stop callback, and
	 * ~when_any_awaiter deregisters it before dropping the block's reference. The
	 * enumeration behind that is in that destructor's note below.
	 *
	 * THE CONDITION THAT WOULD BREAK IT: any later change that parks a SECOND joiner on
	 * one combinator's counter. A second joiner has its own stop callback and its own
	 * reference-drop point, neither ordered against the first's, so a benign race
	 * between two resumers could put a late losing claim on a freed list -- at which
	 * point WaiterListDeathTest.AClaimThatWouldLoseTheElectionOnADestroyedListAborts is
	 * the mechanism that aborts on it, on a race that harms nothing. Such a change
	 * re-opens the question of where that assert belongs; it is a decision about
	 * waiter_list, not about this file, and it must be made deliberately rather than
	 * discovered from a red build.
	 *
	 * This component also parks STRICTLY AFTER its fan-out, as when_all does. That
	 * settles a question combinator_state's constructor note leaves open: it keeps the
	 * starter's outstanding item "for stage B rather than for today: a combinator that
	 * parks before its fan-out ends -- which when_any may -- would need it". when_any
	 * as built does not, so the starter's item is still defensive here and not
	 * load-bearing. The measurement is in await_suspend below.
	 */
	template<typename Result, typename Slots, typename Storage>
	class when_any_awaiter
	{
	public:
		/** Constructs an awaiter over a tuple of awaitables, or a range, that outlives it. */
		explicit when_any_awaiter(Storage* awaitables) noexcept: m_storage(awaitables)
		{
		}

		/**
		 * @brief Moves an awaiter that has not suspended yet. The only kind there is.
		 *
		 * detail::cancellation_checked stores the awaiter it wraps by value, so the
		 * awaiter returned from operator co_await is moved once before any of its await
		 * members run. The block pointer transfers and the source is left null, so only
		 * one of the two releases the reference. detail::when_all_awaiter's own move
		 * constructor carries the same assert for the same reason: a field set before
		 * await_suspend would have to be carried here by hand.
		 */
		when_any_awaiter(when_any_awaiter&& other) noexcept:
		  m_storage(other.m_storage),
		  m_state(std::exchange(other.m_state, nullptr))
		{
			assert(!other.m_join.has_value() && "moving an alt when_any_awaiter that has already suspended");
		}

		when_any_awaiter(const when_any_awaiter&)            = delete;
		when_any_awaiter& operator=(const when_any_awaiter&) = delete;
		when_any_awaiter& operator=(when_any_awaiter&&)      = delete;

		/**
		 * @brief Deregisters the joiner's stop callback, then drops the awaiter's
		 *        reference to the shared block.
		 *
		 * DELICATE. The reset comes first, and that order is ordering rather than
		 * tidying. ~when_all_awaiter's note is the long form of why, and this is the
		 * one place in this component where that note may NOT simply be cited: it ends
		 * by saying that a combinator which parks more than one joiner on a counter, or
		 * parks before its fan-out has ended, must re-derive every enumeration in it
		 * from scratch. when_any does neither -- see this class's note -- so the
		 * derivation does carry; but it was re-derived against this component's own
		 * call sites rather than taken on trust, and this is the result:
		 *
		 * - TWO complete() calls can reach this block's counter, as in when_all:
		 *   on_when_any_child_complete, which calls complete() before release(), and the
		 *   starter's in await_suspend, which runs before the park.
		 *   request_children_stop() adds no third -- it requests a stop, and the children
		 *   it stops retire through the hook like any other child.
		 * - ONE resumption writes satisfied: completion_counter::complete()'s
		 *   resume_claimed(m_joiners.detach_all(), true) on the zero transition. That is a
		 *   fact about completion_counter and is unchanged by anything here.
		 * - THREE parties resume a joiner unsatisfied: its own waiter_stop_release,
		 *   ~completion_counter and ~waiter_list. Unchanged.
		 * - FOUR parties touch the counter's waiter_list: the joiner's
		 *   waiter_stop_release; link() from the joiner's own await_suspend; detach_all()
		 *   from the complete() that reaches zero; and the drains in ~completion_counter
		 *   and ~waiter_list. The election and the children's stop touch neither the
		 *   counter nor its list, so this component adds no fifth.
		 *
		 * All four therefore hold with the same values, and the conclusions that rest on
		 * them hold here too, including the proviso that the satisfied path's immunity is
		 * a condition on the CALLER: a co_await full-expression holding a second
		 * suspension point lets the resuming child's reference go before this destructor
		 * runs. Read that note for what is at stake and for what breaks it; what is
		 * local to this frame is the order of the two statements below, and it is the
		 * half that stands whatever else changes.
		 *
		 * The reference is still dropped after await_resume has finished reading the
		 * winner's slot, which is why the release is here and not there.
		 */
		~when_any_awaiter()
		{
			// First, and see above.
			m_join.reset();

			if(m_state != nullptr)
			{
				m_state->shared().release();
			}
		}

		/**
		 * Unconditionally false, so the block is allocated and the children started in
		 * await_suspend, which is the only place that has the awaiting handle. A race
		 * therefore always enters a suspension point, even one it returns from without
		 * parking.
		 */
		// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the compiler calls this through an awaiter instance at every co_await of a when_any; static here would move readability-static-accessed-through-instance to those call sites instead.
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		/**
		 * @brief Allocates the block, fans the children out, and parks on the counter.
		 *
		 * DELICATE. Every statement's position is load-bearing, and the order is stage
		 * A's rather than a new one: allocate the state, wire the completion hook, link
		 * the chain's token, take the children's token from the block, fan out, retire
		 * the starter, park on the counter. detail::when_all_awaiter::await_suspend
		 * states and measures why each statement sits where it does -- the link before
		 * the fan-out, the block's stop source rather than the chain's token in the
		 * child options, the park after everything -- and this function follows it.
		 * Do not re-derive those here; read them there before moving anything.
		 *
		 * Nothing precedes the allocation, deliberately, and which no-children case that
		 * leaves room for is worth being exact about, because the two forms fail
		 * differently and only one of them can be answered from here:
		 *
		 * - AN ARGUMENT-LIST when_any OVER NO AWAITABLES IS A COMPILE ERROR, and no
		 *   early return can change that. Its Slots is an empty std::tuple, so
		 *   when_any_result_at's static_assert rejects it as soon as await_resume is
		 *   instantiated -- which the compiler does at the co_await, whatever this
		 *   function would have returned at run time. (Its Result would be a
		 *   std::variant<> besides.)
		 * - AN EMPTY RANGE IS A RUN-TIME CASE and is the one this space is for. Its types
		 *   are all well formed, the child count is simply zero, the counter drains as the
		 *   starter retires, the join refuses the link and await_resume then finds no
		 *   winner. That case must report its outcome before anything is allocated and
		 *   before the chain suspends, which means ahead of the statement below.
		 *
		 * No overload exists yet that can build either, so neither is reachable today;
		 * the no-winner case await_resume asserts is the second of the two.
		 *
		 * @return False when the race is already over -- every child retired before the
		 *         join could link -- so the awaiting coroutine carries straight on.
		 */
		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h)
		{
			m_state = std::make_unique<when_any_state<Slots>>(child_count()).release();

			start_options child_options{};
			child_options.on_completion = {.invoke = &on_when_any_child_complete, .state = &m_state->shared()};

			if constexpr(relays_cancellation<Promise>)
			{
				if(cancellation_context* const context = h.promise().cancellation_context_pointer(); context != nullptr)
				{
					// The children inherit the chain's home, so a child holding
					// thread-affine state is destroyed on the thread start_options::home
					// promises. The shared block is what makes that safe: the chain may be
					// gone long before the home is drained.
					child_options.home = context->home;

					// Before any child starts. when_all's note on this line gives the
					// mechanism: a stop that lands during the fan-out marks the block
					// cancelled and stops the child source, so children started below die at
					// their first suspension point instead of running work the chain will
					// never read. Its figure, 200/200 against its own fan-out test, is
					// when_all's; this component has no fan-out stop test of its own, so
					// there is no number here.
					m_state->shared().link(context->token);
				}
			}

			// The children observe the BLOCK's stop source, not the chain's token, and
			// the two are not interchangeable. when_all's note on this line measures what
			// substituting the chain's token costs -- a stop state freed while its own
			// request_stop() is on the stack. That argument is about combinator_state's
			// m_link and m_children and about libstdc++'s reverse callback walk, so it is
			// the block's reasoning rather than when_all's and it transfers unchanged; the
			// figure attached to it does not, having been taken against a when_all test.
			// This component leans on the same source a second time, which when_all does
			// not: it is what a winner stops its siblings through.
			child_options.token = m_state->shared().children_token();

			// One add_ref per child, before that child is started, for the same reason
			// task_group::spawn raises its count before start: a body with no suspension
			// point completes inside start() and fires its hook there. A race reaches that
			// shape readily -- WhenAnyTest.EveryChildAlreadyCompleteYieldsAWinnerWithout-
			// Suspending is a whole fan-out of it -- so it is not a corner case here.
			//
			// No rollback if start throws, exactly as task_group::spawn records, and the
			// consequence is when_all's unchanged because the raise precedes the throw
			// here too: the exception propagates out of the await, nothing releases the
			// reference that child was given, and **the block leaks** along with its
			// slots, its election and the children's stop source. Children already
			// started run to completion unobserved and their results are discarded. There
			// is no hang, the chain never having parked. One leak per failed start.
			start_children(child_options);

			// Retire the starter. Until this returns no child can have driven the count
			// to zero, and nothing resumes here either: no joiner is parked yet.
			//
			// After the loop rather than before it, as in stage A, and re-measured here
			// rather than inherited, because stage A's own note says its figure was taken
			// against a component that parks strictly after its fan-out and this one might
			// not have. It does: this component parks after the fan-out too, so no joiner
			// exists for an early zero to resume, and join_awaiter::ready() is re-read
			// under the list's mutex, so an early zero only refuses the link.
			//
			// Moving this above the loop therefore fails nothing here either: 0 of 200
			// repeats in debug and 0 of 20 under each of AddressSanitizer and
			// ThreadSanitizer, for each of the five WhenAnyTest tests run alone, plus one
			// whole-suite run of all 788 tests green in each of the three presets. So the
			// POSITION is defensive and not load-bearing. DELETING the retirement is what
			// fails, and that is the same instrument reporting: TheFirstChildToComplete...
			// and NoChildIsRunning... fail 5 of 5, EveryChildAlreadyComplete... HANGS 5 of
			// 5 -- the counter never reaches zero, so the chain parks forever -- and only
			// ACancelledRace... still passes, its verdict coming from the chain's own stop
			// rather than from the counter draining. Those four are what say the zeros
			// above are a real negative rather than a blind harness. See the
			// combinator_state constructor for why the count stays.
			m_state->shared().counter().complete();

			// Park on the counter. completion_counter::join_awaiter is reused as-is for
			// the reason when_all gives: the resumption election between "the last child
			// retired" and "the chain was cancelled" is what waiter_list::claim already
			// decides.
			//
			// That this parks until the LAST child retires, and not until the election is
			// settled, is what makes the component's central guarantee true: no child is
			// left running when a won await returns. It also means the chain is resumed
			// through the counter and never through the election, so the winner's slot is
			// published to this chain by the counter -- by detach_all()'s mutex on the
			// parked path, and by the fetch_sub/outstanding() acquire pair on the path
			// where the link is refused. combinator_election's note says the same from the
			// other side: rely on the counter for visibility, not on the election.
			//
			// Nothing may be read from *this after the call: a successful link publishes
			// the node, and any party may then resume this frame and destroy it.
			m_join.emplace(m_state->shared().counter());
			return m_join->await_suspend(h);
		}

		/**
		 * @brief The outcome: the winner's position and value, or the released error.
		 *
		 * THE ORDER OF THE TWO TESTS BELOW IS THE DECISION THIS FUNCTION EXISTS FOR.
		 * The cancellation flag outranks the election: a cancellation that arrives after
		 * a winner was elected reports std::errc::operation_canceled and discards the
		 * winner. The flag and a winner are not alternatives, because a cancelled race
		 * really does elect one: a child already PARKED on an alt wait primitive is
		 * resumed released rather than torn down, so it runs on, writes whatever a
		 * released wait is worth into its slot and reaches its election. (A child
		 * stopped before it reached any suspension point is torn down at its first one
		 * and reaches neither.)
		 *
		 * BUT NO PATH IN THE SHIPPED TREE ELECTS BEFORE THIS FUNCTION READS THE FLAG,
		 * and that is the honest statement of what the order below is for. The two have
		 * to be told apart, because an earlier draft of this note cited
		 * WhenAnyTest.ACancelledRaceReportsOperationCanceled as the witness and that
		 * test cannot be one:
		 *
		 * - ON THE CANCELLED PATH THE ELECTIONS COME AFTER THE AWAIT HAS RETURNED. The
		 *   chain's token holds the block's m_link, registered in await_suspend, and the
		 *   joiner's own waiter_stop_release, registered later at the park; libstdc++
		 *   walks that list in reverse registration order, so the joiner's release
		 *   resumes the chain FIRST, this function takes its verdict from !waited, and
		 *   only then does m_link fire cancel() and let the children run on and elect.
		 *   winner() therefore still reads no_winner where the flag is read. That test
		 *   observes the half that is observable -- both children resumed released and
		 *   ran the statement after their co_await, so both reached their election,
		 *   store-then-elect being straight-line code in when_any_child -- and it
		 *   cannot tell "no winner" from "winner discarded". It is the witness for the
		 *   premise, not for this ordering.
		 * - A TOKEN ALREADY STOPPED WHEN await_suspend RUNS does not produce one either.
		 *   link() fires cancel() inline, before the fan-out, so the flag is set first
		 *   and a child with no suspension point can complete and elect during the
		 *   fan-out -- but the park then emplaces its callback on an already-stopped
		 *   token, the claim finds the node, m_released is set and the verdict is again
		 *   !waited.
		 * - WHAT WOULD PRODUCE ONE is a stop that lands after this chain was resumed
		 *   SATISFIED and before this line reads the flag. Single-threaded that window
		 *   does not exist: await_resume runs in the same resumption that await_suspend
		 *   returned from. It is a cross-thread window, and nothing in the suite races
		 *   for it.
		 *
		 * So the discarding of an elected winner is ARRANGED BUT UNWITNESSED. Witnessing
		 * it needs either that cross-thread race -- a child driving the counter to zero
		 * on one thread while another requests the chain's stop, repeated until the
		 * interleaving lands -- or the reordering named in the second bullet below,
		 * which makes cancel() run before the joiner is claimed. Neither is in the suite.
		 *
		 * DO NOT delete or reorder the cancelled() test as dead code, and do not read
		 * the measurement below as saying it is. What it says is narrower:
		 *
		 * - Moving the cancelled() test below the winner check, so that an elected
		 *   winner outranks the flag, fails nothing in the suite: 0 of 200 repeats in
		 *   debug and 0 of 20 under each of AddressSanitizer and ThreadSanitizer, for
		 *   each of the five WhenAnyTest tests run alone, plus one whole-suite run of all
		 *   788 tests green in each of the three presets. That zero is the three bullets
		 *   above restated as a measurement rather than a second finding: the FIRST half
		 *   of the test returns the error on every path there is, so the second half is
		 *   never the deciding one. There is no guard of record for this ordering, which
		 *   is the state of it rather than an omission. The reason it still holds here is
		 *   detail::when_all_awaiter::await_resume's, unchanged.
		 * - Nothing in [stopcallback] fixes that order, which is why the flag is stored
		 *   ahead of the children's stop in combinator_state::cancel() and read ahead of
		 *   the election here. when_all's await_resume note measures the reordering that
		 *   makes its own flag test the deciding half -- registering the link after the
		 *   join park -- and that figure is when_all's. It was NOT repeated here, so do
		 *   not read it as a number for this component.
		 *
		 * A stop that races a completion is therefore reported as a cancellation, and a
		 * fully assembled winner is discarded with it. That is deliberate: a caller that
		 * cancelled a race is told the race was cancelled, not handed a result it may no
		 * longer have anywhere to put.
		 *
		 * May rethrow the winner's exception, which result_store::value() does from
		 * inside the assembly. **A cancelled race rethrows nothing**, and that follows
		 * from the order above rather than being a separate rule: the flag test returns
		 * before the assembly runs, so a winner that completed by throwing has its
		 * exception discarded with its slot exactly as a winner that completed with a
		 * value has. The cancellation is the only thing the caller is told.
		 */
		[[nodiscard]] std::expected<Result, std::error_code> await_resume()
		{
			// m_join is engaged by await_suspend, which always runs first: await_ready is
			// unconditionally false, so there is no path to a resumption without it.
			// Asserted rather than left implicit because clang-tidy-19's
			// bugprone-unchecked-optional-access cannot see across the two functions.
			assert(m_join.has_value() && "resuming an alt when_any that never suspended");

			const alt::wait_result waited = m_join.value().await_resume();

			// The flag first, and see above.
			if(!waited || m_state->shared().cancelled())
			{
				return std::expected<Result, std::error_code>{std::unexpect, wait_released_error()};
			}

			// Satisfied and not cancelled, so every child retired and at least one of
			// them retired by completing rather than by being stopped -- only cancel()
			// stops a child without a winner, and that sets the flag tested above. The
			// block is still alive because this awaiter has not been destroyed.
			//
			// An EMPTY RANGE is the one way past that argument: zero children, the
			// counter drained by the starter alone, nothing elected. It is what the
			// assert below is for, and it has no caller -- no overload builds a range
			// race yet -- so it must be answered by a run-time early return in
			// await_suspend, ahead of the allocation. An argument-list race over no
			// awaitables is not this case and never reaches here: it fails to compile in
			// when_any_result_at. See await_suspend, which separates the two.
			const std::size_t winner = m_state->election().winner();
			assert(winner != combinator_election::no_winner && "an alt when_any was satisfied with no child elected");

			return assemble_when_any_result<Result>(m_state->slots(), winner);
		}

	private:
		/** True for the range form, whose storage is the range itself and not a tuple. */
		static constexpr bool is_range_form = std::ranges::range<Storage>;

		/** How many children this race fans out. */
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
				// The range is iterated here and nowhere else, once. Slot i and election
				// position i both belong to the i-th element, so the index the election
				// reports is an index into the caller's range.
				//
				// alt::start's cancellation_handle is discarded here and in start_child,
				// deliberately: nothing cancels a single child, so the only thing the
				// handle could do is stop one of them on its own, which the children's
				// shared stop source exists to do for all of them at once. The children are
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
					// place of the child's by-value parameter, so the child's frame owns it
					// and the caller's range is left holding moved-from elements.
					static_cast<void>(alt::start(when_any_child<element_type>(element_type(std::ranges::iter_move(it)),
					                                                          index,
					                                                          &m_state->slots()[index],
					                                                          &m_state->election(),
					                                                          &m_state->shared()),
					                             options));
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

		/** Raises the count for child @p I and starts it at position @p I. */
		template<std::size_t I>
		void start_child(const start_options& options)
		{
			// The type the argument tuple stored: a value for an rvalue argument, moved
			// into the child's frame, and T& for an lvalue one.
			using child_type = std::tuple_element_t<I, Storage>;

			m_state->shared().add_ref();

			static_cast<void>(alt::start(when_any_child<child_type>(std::forward<child_type>(std::get<I>(*m_storage)),
			                                                        I,
			                                                        &std::get<I>(m_state->slots()),
			                                                        &m_state->election(),
			                                                        &m_state->shared()),
			                             options));
		}

		/** The caller's awaitables or range, owned by the awaitable object this came from. */
		Storage* m_storage;

		/** The shared block, from await_suspend until this awaiter is destroyed. */
		when_any_state<Slots>* m_state = nullptr;

		/** This chain's place in the block's completion counter. The only joiner on it. */
		std::optional<completion_counter::join_awaiter> m_join;
	};

}

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
	 */
	inline void on_combinator_child_complete(void* state) noexcept
	{
		auto* const shared = static_cast<combinator_state*>(state);

		// complete() may reach zero and resume the awaiting chain on this thread,
		// whose body may drop its own reference. release() must follow it, and
		// nothing may be read from shared after release(): this may be the last one.
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
		 * @brief Drops the awaiter's reference to the shared block.
		 *
		 * DELICATE. This is reference 1, the one the block is constructed with, and
		 * where it is dropped is the whole of two invariants.
		 *
		 * The awaiter holds it for as long as the awaiting chain is parked, so the block
		 * can never reach zero references with a joiner still on the counter: the
		 * counter is destroyed sixth of the block's seven members, and
		 * ~completion_counter resumes every still-parked joiner, whose body would then
		 * run against five already-destroyed members -- among them m_cancelled, which
		 * await_resume reads. The joiner is always off the
		 * counter before this runs -- claimed by the zero transition, claimed by the
		 * chain's cancellation, or never linked at all -- because this destructor runs
		 * only at the end of the co_await full-expression, after await_resume.
		 *
		 * It is also dropped after await_resume has finished reading the slots, which is
		 * why the release is here and not there.
		 *
		 * What it does NOT make unreachable is the block's last reference going while a
		 * cancel() is still unwinding. The chain is released before the children are
		 * stopped, so this destructor runs first and a child's completion hook then
		 * drops the last reference from inside combinator_state::cancel(). That is the
		 * ordinary cancellation path, and what makes it survivable is the counted copy
		 * of the stop state cancel() holds -- not anything here. Read that note before
		 * changing either.
		 */
		~when_all_awaiter()
		{
			if(m_state != nullptr)
			{
				m_state->shared().release();
			}
		}

		/**
		 * Unconditionally false, so the block is allocated and the children started in
		 * await_suspend, which is the only place that has the awaiting handle. A
		 * when_all with no children therefore costs one allocation and enters a
		 * suspension point it returns from without parking.
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
			// debug, 0/200 TSan, 0 across the suite -- while deleting it fails 20/20. See
			// the combinator_state constructor for why the count stays anyway.
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
	 * an rvalue carries no such requirement: it is moved into its child's frame, which
	 * owns it. The lvalue case is not made safe by copying, because alt::task is not
	 * copyable, which is the point of the distinction.
	 *
	 * Awaiting this costs one allocation, including when there are no arguments at all,
	 * for which the result is an empty tuple rather than a cancellation.
	 *
	 * The children inherit the awaiting chain's home scheduler and share one stop
	 * source: nothing cancels a single child on its own.
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
	 * empty vector, not a cancellation, at the cost of one allocation.
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

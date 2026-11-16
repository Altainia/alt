# `when_all` (combinators stage A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `alt::when_all`, which awaits several awaitables concurrently and yields every result positionally, with cancellation of the awaiting chain bounded and non-blocking.

**Architecture:** `when_all` is a bespoke awaiter, not a coroutine. On suspension it allocates one refcounted block holding the result slots, a `detail::completion_counter` and one `std::stop_source` for the child set, then starts each awaitable inside its own `alt::cancellable_task` — so every child is its own cancellation root and the combinator never owns a child frame. The awaiting chain parks on the block's counter via `completion_counter::join()`; a stop on the chain's token marks the block released and requests the children's stop, and the chain is then free to die while the children drain into state it no longer owns.

**Tech Stack:** C++23 coroutines, `std::stop_token`/`std::stop_callback`, `std::expected`, GoogleTest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-14-combinators-design.md` (revision 2, with the same-day correction to decision 5)

## Global Constraints

- Namespace `alt`; engine internals in `alt::detail`. No other nested namespaces.
- `snake_case` for everything except template typenames, which are `PascalCase`.
- Private and protected member variables are prefixed `m_`.
- Declare variables `const` unless mutation is required.
- `std::expected` for recoverable errors, not exceptions or error codes.
- Doxygen `/** */` on every public function, class, struct, enum and union; update an existing comment whose meaning a change affects.
- Header-only component: a single `.hpp` in `include/alt/`. A header no caller would include without a public component goes in `include/alt/detail/`.
- A new test file goes in `tests/` as `test_<component>.cpp` and must be added to the `alt_tests` sources list in `tests/CMakeLists.txt`.
- The released error is always `std::errc::operation_canceled`, built only through `detail::wait_released_error()`.
- **Invariant (A):** the combinator never owns or destroys a child frame. It only ever requests stops. Children are self-owning `cancellable_task`s. This forbids holding children as `alt::task`s the combinator destroys.
- **Invariant (B):** the claim election cannot free an already-claimed node. A loser whose node a satisfier already won loses the CAS in `waiter_list::claim`, so its `waiter_stop_release` resumes and frees nothing.
- **Invariant (C):** the combinator never *destroys* a child. It may resume the awaiting chain while children are alive; (A) is what makes that safe for the sibling hazard.
- The awaiting chain's result is `std::expected<Results, std::error_code>`; cancellation **is** an observable result, because `cancellation_checked::await_resume` is a bare delegation and the resumed parent's body runs on with no further check.
- Children inherit the awaiting chain's `home` (`cancellation_context::home`), so a child holding thread-affine state is destroyed on the thread `start_options::home` promises.
- One `std::stop_source` for the whole child set. Nothing in stage A cancels one child individually.
- `tests/.clang-tidy` carries the suite's suppression list — **append to it, never rewrite it.**
- Every quality gate in `CLAUDE.md` must pass: GCC, Clang, optimized, ASan+UBSan, TSan, `clang-tidy-19` (pinned, with `clang-19`/`clang++-19` and `CMAKE_BUILD_TYPE=Debug`), cppcheck, and the coverage preset. Eight gates. A sweep that omits one is not a green branch.

## Review Focus

Five things the spec implies that the obvious tests miss, most likely to bite first:

1. **A child awaitable whose `await_suspend` throws.** The child has no `on_exception`, so an escaped exception reaches `cancellable_task_promise::unhandled_exception` and calls `std::terminate`. The child's `try` must enclose the whole `co_await`, not just the resumption. → test in Task 3.
2. **`when_all` awaited from a promise that relays no cancellation context** — a plain coroutine, or `sync_wait`. There is then no token to link and no `home`; the children must still start, complete and assemble, and the combinator is simply uncancellable. → test in Task 3.
3. **Every child already satisfied when the `co_await` is reached.** The counter hits zero as the starter retires, the `join` link is refused, and `await_suspend` must return `false` rather than parking a waiter nobody will ever resume. → test in Task 3.
4. **An empty range in the vector form.** Zero children: the block is still allocated, the starter still retires, and the result is an empty `std::vector`, not a cancellation. The block must still be released. → test in Task 4.
5. **A stop requested after the wait was satisfied but before the block's last reference goes.** The link callback then runs over a block whose awaiter reference has already been dropped; only the children's references are keeping it alive. → test in Task 5.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `include/alt/detail/combinator_state.hpp` | **New.** `detail::combinator_state`: the non-template refcounted block — refcount, `completion_counter`, children's `std::stop_source`, the released flag, and the link onto the awaiting chain's token. All of stage A's concurrency lives here, in a class testable without a single coroutine. |
| `include/alt/detail/combinator_results.hpp` | **New.** The compile-time mapping from a child's await result to its stored and returned type, the slot containers, and result assembly. No concurrency. |
| `include/alt/when_all.hpp` | **New.** Public: `alt::when_all` (variadic and range), the awaitable/awaiter pair, the child wrapper coroutine, and the typed state that bolts slots onto `combinator_state`. |
| `include/alt/async_latch.hpp` | **Modify** (`:58-77`). Replace the "nothing lets one waiter own a sibling's frame" argument with the claim-election argument, now that a combinator exists. |
| `tests/test_combinator_state.cpp` | **New.** Task 1. |
| `tests/test_combinator_results.cpp` | **New.** Task 2. |
| `tests/test_when_all.cpp` | **New.** Tasks 3-5. |
| `tests/CMakeLists.txt` | **Modify.** Register the three test files. |
| `docs/when_all.md` | **New.** Task 6. |
| `README.md` | **Modify** (Components table). Task 6. |

Three headers rather than one: the refcounted block is the only part with a concurrency argument to make, and keeping it non-template means a reviewer can read it, and a test can drive it, without instantiating a combinator. `when_any` in stage B reuses both `detail/` headers unchanged.

---

### Task 1: The refcounted shared block

**Files:**
- Create: `include/alt/detail/combinator_state.hpp`
- Create: `tests/test_combinator_state.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_combinator_state.cpp` to the `alt_tests` sources list)

**Interfaces:**
- Consumes: `detail::completion_counter` (`add`, `complete`, `outstanding`, `join`) from `include/alt/detail/completion_counter.hpp`; `detail::stop_forwarder` as the precedent for a callback holding a reference-counted copy rather than a pointer.
- Produces, all in `namespace alt::detail`:

```cpp
class combinator_state
{
public:
    combinator_state(std::size_t children, void* owner, void (*destroy)(void* owner) noexcept);

    void add_ref() noexcept;
    void release() noexcept;

    [[nodiscard]] completion_counter& counter() noexcept;
    [[nodiscard]] std::stop_token children_token() const noexcept;

    void cancel() noexcept;
    [[nodiscard]] bool cancelled() const noexcept;

    void link(std::stop_token token);

    combinator_state(const combinator_state&)            = delete;
    combinator_state& operator=(const combinator_state&) = delete;
    combinator_state(combinator_state&&)                 = delete;
    combinator_state& operator=(combinator_state&&)      = delete;

private:
    std::atomic<std::size_t> m_references{1};
    completion_counter       m_counter;
    std::stop_source         m_children;
    std::atomic<bool>        m_cancelled{false};
    void*                    m_owner;
    void                   (*m_destroy)(void* owner) noexcept;
    std::optional<std::stop_callback<combinator_stop_forwarder>> m_link;
};
```

Three things about that declaration are load-bearing and must be commented in the header:

- The constructor sets `m_counter.add(children + 1)`. The `+ 1` is the **starter's**: the party fanning the children out holds one outstanding item until every child has been started, so a child completing synchronously during the fan-out cannot drive the count to zero and resume a parent that has not finished suspending. This is `task_group::spawn`'s "raise the count before the task starts" rule generalized to N starts.
- `m_references` starts at **1**, the awaiter's. One `add_ref()` per child, before that child is started.
- **`m_link` is declared last, so it is destroyed first.** `std::stop_callback`'s destructor blocks until an invocation already running on another thread returns, so destroying it ahead of the other members is what guarantees no `cancel()` is in flight over them. This is `wait_awaiter`'s reason for declaring `m_stop` after `m_node`, and unlike `cancellable_task_promise` the hazard here is **not** removed structurally — the callback really does point into this object — so the order is load-bearing and nothing in the language checks it.

`combinator_stop_forwarder` is a small struct in the same header holding `combinator_state* target` with `void operator()() const noexcept { target->cancel(); }`. It points at the enclosing block rather than holding a copy of anything, which is safe only because of the declaration order above.

The erased `void* owner` + `void (*destroy)(void*) noexcept` pair rather than a virtual destructor: it is the shape `alt::work_item`, `alt::exception_handler` and `alt::completion_handler` already use, it keeps `combinator_state` a concrete class a test can drive on its own, and it lets the typed state own the block by composition instead of inheritance.

- [ ] **Step 1: Write the failing tests**

In `tests/test_combinator_state.cpp`, fixture `CombinatorStateTest`. A helper records destruction:

```cpp
struct deleter_probe
{
    int calls = 0;
    static void destroy(void* owner) noexcept { ++static_cast<deleter_probe*>(owner)->calls; }
};
```

```cpp
TEST_F(CombinatorStateTest, ConstructionLeavesTheStarterOutstanding)
// state over 3 children -> counter().outstanding() == 4

TEST_F(CombinatorStateTest, RetiringTheStarterWithNoChildrenReachesZero)
// state over 0 children -> complete() once -> outstanding() == 0

TEST_F(CombinatorStateTest, TheLastReleaseRunsTheErasedDeleterExactlyOnce)
// add_ref() twice, release() three times -> probe.calls == 1

TEST_F(CombinatorStateTest, AReleaseThatIsNotTheLastRunsNothing)
// add_ref(), release() -> probe.calls == 0

TEST_F(CombinatorStateTest, CancelMarksTheWaitReleasedBeforeStoppingTheChildren)
// Register a std::stop_callback on children_token() whose body reads cancelled()
// and stores it. cancel(). Assert the stored value is true: the flag must be
// visible to anything the children's stop reaches, because a cancelled child
// still calls complete() and may drive the count to zero.

TEST_F(CombinatorStateTest, CancelIsIdempotent)
// Count invocations of a callback on children_token(). cancel() twice -> 1.

TEST_F(CombinatorStateTest, CancelledIsFalseUntilCancelIsCalled)

TEST_F(CombinatorStateTest, LinkingAnAlreadyStoppedTokenCancelsImmediately)
// stop_source stopped first, then link(token) -> cancelled() is already true

TEST_F(CombinatorStateTest, LinkingPropagatesALaterStop)
// link(token), then request_stop() -> cancelled() and children_token().stop_requested()

TEST_F(CombinatorStateTest, LinkingAnUnstoppableTokenIsANoOp)
// link(std::stop_token{}) -> cancelled() stays false, nothing registered

TEST_F(CombinatorStateTest, ARaceBetweenAStopAndTheLastReleaseIsClean)
// Two threads, alt_test::rendezvous: one calls source.request_stop(), the other
// calls release() as the last reference. Assert probe.calls == 1 and no
// sanitizer report. This is the test the m_link declaration order exists for.
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --preset debug && cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — `alt/detail/combinator_state.hpp: No such file or directory`.

- [ ] **Step 3: Implement `combinator_state` in `include/alt/detail/combinator_state.hpp`**

Signatures as in the Interfaces block. Two bodies the signature does not determine:

```cpp
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

void release() noexcept
{
    if(m_references.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Copied out before the call: m_destroy frees *this.
        const auto destroy = m_destroy;
        void* const owner  = m_owner;
        destroy(owner);
    }
}
```

`cancelled()` loads with `std::memory_order_acquire`, pairing with that release.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CombinatorStateTest`
Expected: PASS, all tests.

- [ ] **Step 5: Mutation-check the two ordering assertions**

The suite must fail when the order is wrong. For each, apply the mutation, run the named test 200 times, record the failure count, revert.

| Mutation | Test that must fail |
|---|---|
| Swap the two statements in `cancel()` | `CancelMarksTheWaitReleasedBeforeStoppingTheChildren` |
| Move `m_link` to the first member declaration | `ARaceBetweenAStopAndTheLastReleaseIsClean` (under TSan and ASan) |

Record each count in the task report, including a zero. A zero means the test does not guard what it claims to; restructure the test, do not soften the claim.

- [ ] **Step 6: Commit**

```bash
git add include/alt/detail/combinator_state.hpp tests/test_combinator_state.cpp tests/CMakeLists.txt
git commit -m "Add the refcounted shared block the combinators fan out through"
```

---

### Task 2: Result mapping, slots and assembly

**Files:**
- Create: `include/alt/detail/combinator_results.hpp`
- Create: `tests/test_combinator_results.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `alt::await_result_t`, `alt::awaitable` from `<alt/awaitable.hpp>`; `detail::result_store<T>` from `<alt/detail/coroutine.hpp>`.
- Produces, all in `namespace alt::detail`:

```cpp
/** How a child's await result is stored and handed back. */
template<typename R> struct combinator_value               { using type = std::remove_cvref_t<R>; };
template<>           struct combinator_value<void>         { using type = std::monostate; };
template<typename T> struct combinator_value<T&>           { using type = std::reference_wrapper<T>; };

template<typename R> using combinator_value_t = typename combinator_value<R>::type;

/** The value type position i of a when_all over A contributes. */
template<typename A> using when_all_value_t = combinator_value_t<alt::await_result_t<A>>;

/** The slot position i writes into. */
template<typename A> using when_all_slot_t = result_store<when_all_value_t<A>>;

/** Builds the slot container. A tuple ignores n; a vector is sized by it. */
template<typename Slots> [[nodiscard]] Slots make_combinator_slots(std::size_t n);

/** Moves every slot's value into Results, rethrowing the lowest-indexed stored exception. */
template<typename Results, typename Slots>
[[nodiscard]] Results assemble_combinator_results(Slots&& slots);
```

Three mapping decisions, each to be stated in the header:

- **`void` becomes `std::monostate`,** which the spec fixes, and the slot is `result_store<std::monostate>` rather than `result_store<void>`. That removes the only `if constexpr` from assembly: every slot has a `value()` returning something, so one expression covers every position.
- **An lvalue-reference result becomes `std::reference_wrapper<T>`,** following `detail::completion_value` in `task_completion_source.hpp` — `std::expected<T&, E>` is ill-formed, and `Results` travels inside a `std::expected`. The referenced object must outlive the await.
- **An rvalue-reference result (`T&&`, what awaiting an `alt::task<T>` rvalue yields) becomes a value,** by `remove_cvref_t` in the primary template, so the result is moved out of the dying child frame rather than referenced into it. This is the whole reason the slots live off the frame.

`assemble_combinator_results` is a braced-init-list over the slots, whose left-to-right evaluation order is what makes "the lowest-indexed throwing child is the one rethrown" true rather than incidental.

- [ ] **Step 1: Write the failing tests**

In `tests/test_combinator_results.cpp`. The mapping is compile-time, so most of this is `static_assert`; the assembly tests are runtime.

```cpp
// Mapping, as static_asserts against alt::task<T>, alt::task<void>, alt::task<int&>
// and alt::async_event:
static_assert(std::same_as<when_all_value_t<alt::task<int>>, int>);
static_assert(std::same_as<when_all_value_t<alt::task<void>>, std::monostate>);
static_assert(std::same_as<when_all_value_t<alt::task<int&>>, std::reference_wrapper<int>>);
static_assert(std::same_as<when_all_value_t<alt::task<std::string>>, std::string>);
// alt::async_event yields alt::wait_result, a value type:
static_assert(std::same_as<when_all_value_t<alt::async_event&>, alt::wait_result>);

TEST(CombinatorResultsTest, MakeSlotsDefaultConstructsATuple)
TEST(CombinatorResultsTest, MakeSlotsSizesAVector)           // n == 4 -> size() == 4
TEST(CombinatorResultsTest, MakeSlotsBuildsAnEmptyVectorForZero)

TEST(CombinatorResultsTest, AssemblyMovesEveryTupleSlotIntoPosition)
// slots: result_store<int>{1}, result_store<std::string>{"two"}, result_store<std::monostate>
// -> std::tuple<int, std::string, std::monostate>{1, "two", {}}

TEST(CombinatorResultsTest, AssemblyMovesRatherThanCopies)
// A move-counting type: exactly one move per position, zero copies.

TEST(CombinatorResultsTest, AssemblyRethrowsAStoredException)
// one slot set_exception(make_exception_ptr(std::runtime_error{"child"}))
// -> EXPECT_THROW(..., std::runtime_error) and the what() is "child"

TEST(CombinatorResultsTest, TheLowestIndexedThrowingSlotIsTheOneRethrown)
// positions 1 and 2 both hold exceptions, with distinct what() strings;
// the one thrown is position 1's.

TEST(CombinatorResultsTest, AssemblyFillsAVectorInOrder)
TEST(CombinatorResultsTest, AssemblyOfAnEmptyVectorYieldsAnEmptyVector)

TEST(CombinatorResultsTest, AReferenceSlotAssemblesToAWrapperAroundTheSameObject)
// int x = 7; slot.set_value(std::ref(x));
// -> &std::get<0>(results).get() == &x
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — `alt/detail/combinator_results.hpp: No such file or directory`.

- [ ] **Step 3: Implement the mapping, `make_combinator_slots` and `assemble_combinator_results`**

In `include/alt/detail/combinator_results.hpp`, signatures as in the Interfaces block. `make_combinator_slots` is two overloads selected by whether `Slots` is a `std::tuple` specialization; the tuple one asserts nothing about `n` beyond a debug check that it equals `std::tuple_size_v<Slots>`.

`assemble_combinator_results` for the tuple case expands over an index sequence into a braced-init-list of `std::move(std::get<I>(slots)).value()`; for the vector case it reserves `slots.size()` and `push_back`s each `std::move(slot).value()` in order.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R CombinatorResultsTest`
Expected: PASS, all tests.

- [ ] **Step 5: Mutation-check the ordering assertion**

Replace the braced-init-list with a `std::make_tuple` call whose arguments are evaluated in an unspecified order, or reverse the index expansion, and confirm `TheLowestIndexedThrowingSlotIsTheOneRethrown` fails. Record the count.

- [ ] **Step 6: Commit**

```bash
git add include/alt/detail/combinator_results.hpp tests/test_combinator_results.cpp tests/CMakeLists.txt
git commit -m "Add the combinator result mapping, slots and assembly"
```

---

### Task 3: The child wrapper, the awaiter, and variadic `when_all`

**Files:**
- Create: `include/alt/when_all.hpp`
- Create: `tests/test_when_all.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `detail::combinator_state`, `detail::combinator_stop_forwarder`; Task 2's `when_all_value_t`, `when_all_slot_t`, `make_combinator_slots`, `assemble_combinator_results`; `alt::start`, `alt::start_options`, `alt::cancellable_task`; `detail::completion_counter::join_awaiter`; `detail::relays_cancellation`, `detail::wait_released_error` from `<alt/detail/async_wait.hpp>`.
- Produces:

```cpp
namespace alt::detail
{
    /** The block plus the slots. Owns the block by composition, not inheritance. */
    template<typename Slots>
    class when_all_state
    {
    public:
        explicit when_all_state(std::size_t children);

        [[nodiscard]] combinator_state& shared() noexcept;
        [[nodiscard]] Slots&            slots() noexcept;

    private:
        static void destroy(void* owner) noexcept;

        Slots            m_slots;
        combinator_state m_shared;   // declared after m_slots: see the note below
    };

    /** Runs one child of a combinator as its own cancellation root. */
    template<typename A, typename Slot>
    cancellable_task when_all_child(A awaitable, Slot* slot, combinator_state* state);

    /** The completion hook every child carries. @p state is the combinator_state. */
    void on_combinator_child_complete(void* state) noexcept;

    template<typename Results, typename Slots>
    class when_all_awaiter { /* await_ready, await_suspend, await_resume */ };

    template<typename... Awaitables>
    class [[nodiscard]] when_all_awaitable { /* operator co_await() && */ };
}

namespace alt
{
    /**
     * Awaits every argument concurrently. Yields
     * std::expected<std::tuple<detail::when_all_value_t<Awaitables>...>, std::error_code>.
     */
    template<typename... Awaitables>
        requires (awaitable<Awaitables> && ...)
    [[nodiscard]] auto when_all(Awaitables&&... awaitables);
}
```

`m_shared` is declared **after** `m_slots` so the block — and with it `m_link`, whose destructor blocks on an in-flight `cancel()` — is destroyed first, before the slots it guards are gone.

The child takes its awaitable **by value in its own frame**, with `A` the type the awaitable tuple stored: a value for an rvalue argument (moved into the child frame, which then owns it) and `T&` for an lvalue argument. That gives `when_all` one documented precondition, in the family's existing shape: **an awaitable passed as an lvalue, and everything it refers to, must outlive the children** — which with a `home` scheduler means outliving its drain. An awaitable passed as an rvalue carries no such requirement, because the child owns it.

- [ ] **Step 1: Write the failing tests**

In `tests/test_when_all.cpp`, fixture `WhenAllTest`, `SetUp` calling `alt_test::reset_coroutine_test_state()`. Drive the awaits through `alt::sync_wait` for the uncancelled cases and through `alt::start`/`alt::cancellable_task` for the cancellation cases, as `tests/test_task_group.cpp` does.

```cpp
TEST_F(WhenAllTest, TwoReadyChildrenYieldBothResults)
// when_all(make_value_task(1), make_value_task(std::string{"two"}))
// -> result.has_value(), std::get<0> == 1, std::get<1> == "two"

TEST_F(WhenAllTest, ResultsAreInArgumentOrderNotCompletionOrder)
// two task_completion_source<int>, set in reverse order from the test body
// -> std::get<0> is the first argument's value

TEST_F(WhenAllTest, AVoidChildOccupiesItsPositionAsMonostate)
// when_all(make_void_task(), make_value_task(5))
// -> std::tuple_element_t<0, ...> is std::monostate, std::get<1> == 5

TEST_F(WhenAllTest, AReferenceChildYieldsAWrapperAroundTheSameObject)

TEST_F(WhenAllTest, NoChildrenCompletesWithAnEmptyTuple)
// when_all() -> has_value(), std::tuple_size_v == 0, and no suspension

TEST_F(WhenAllTest, EveryChildAlreadySatisfiedCompletesWithoutSuspending)   // Review Focus 3
// Children that complete synchronously; assert through a flag set between the
// co_await and the next statement that the chain was never parked, and that
// results still assemble.

TEST_F(WhenAllTest, AChildThatThrewRethrowsAtTheAwait)
// EXPECT_THROW on the co_await; the other child's result is discarded

TEST_F(WhenAllTest, AnAwaitSuspendThatThrowsIsCaughtByTheChildRatherThanTerminating)  // Review Focus 1
// A test awaitable whose await_suspend throws. The process must survive and the
// exception must surface at the parent's co_await.

TEST_F(WhenAllTest, WhenAllInAPromiseWithNoCancellationContextStillCompletes)  // Review Focus 2
// Awaited from a plain coroutine driven by sync_wait: no token linked, no home,
// results assemble.

TEST_F(WhenAllTest, CancellingTheAwaitingChainReportsOperationCanceled)
// cancellable_task parked in when_all over two task_completion_source children;
// handle.request_stop() -> the co_await yields !result and
// result.error() == std::errc::operation_canceled

TEST_F(WhenAllTest, CancellingTheAwaitingChainRequestsEveryChildsStop)
// Each child parks on its own async_event; after request_stop() every child's
// frame is gone (alt_test::live_frames == 0)

TEST_F(WhenAllTest, TheParentsBodyRunsOnAfterACancelledWait)
// The point of decision 5: set a flag in the statement after the co_await and
// assert it was set, so the test pins that the result -- not a torn-down frame --
// is what tells the body it was cancelled.

TEST_F(WhenAllTest, AChildCompletingOnAnotherThreadDuringTheFanOutDoesNotResumeEarly)
// N children, one satisfied from a second thread with alt_test::rendezvous timed
// against the fan-out. The starter count is what this guards.
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset debug 2>&1 | tail -20`
Expected: FAIL to compile — `alt/when_all.hpp: No such file or directory`.

- [ ] **Step 3: Implement `when_all_child` in `include/alt/when_all.hpp`**

```cpp
template<typename A, typename Slot>
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
```

The `state` parameter is unused in the body on purpose: the counter is retired by the completion hook at frame destruction, which is the only point both the normal and the cancelled path pass through. Keep the parameter so the hook's `state` pointer and the child's lifetime are visibly the same thing, and say so in a comment.

`on_combinator_child_complete` does exactly two calls, in this order, and reads nothing afterwards:

```cpp
inline void on_combinator_child_complete(void* state) noexcept
{
    auto* const shared = static_cast<combinator_state*>(state);

    // complete() may reach zero and resume the awaiting chain on this thread,
    // whose body may drop its own reference. release() must follow it, and
    // nothing may be read from shared after release(): this may be the last one.
    shared->counter().complete();
    shared->release();
}
```

- [ ] **Step 4: Implement `when_all_awaiter::await_suspend`**

`await_ready()` returns `false` unconditionally. `await_suspend` is where the order is the whole design:

```cpp
template<typename Promise>
bool await_suspend(std::coroutine_handle<Promise> h)
{
    m_state = new when_all_state<Slots>{child_count};

    start_options child_options{};
    child_options.on_completion = {.invoke = &on_combinator_child_complete,
                                   .state  = &m_state->shared()};

    if constexpr(relays_cancellation<Promise>)
    {
        if(cancellation_context* const context = h.promise().cancellation_context_pointer();
           context != nullptr)
        {
            child_options.home = context->home;

            // Before any child starts. An already-stopped token fires this callback
            // from inside link(), which marks the block released and stops the child
            // source, so children started below die at their first suspension point
            // rather than running work the chain will never read.
            m_state->shared().link(context->token);
        }
    }

    child_options.token = m_state->shared().children_token();

    // One add_ref per child, before that child is started, for the same reason
    // task_group::spawn raises its count before start: a body with no suspension
    // point completes inside start() and fires its hook there.
    // No rollback if start throws, exactly as task_group::spawn records.
    for_each_child([&](auto&& awaitable, auto* slot) {
        m_state->shared().add_ref();
        start(when_all_child(std::forward<decltype(awaitable)>(awaitable), slot,
                             &m_state->shared()),
              child_options);
    });

    // Retire the starter. Until this returns, no child can have driven the count
    // to zero, so nothing can have resumed this frame, which has not finished
    // suspending. Nothing resumes here either: no joiner is parked yet.
    m_state->shared().counter().complete();

    // Park on the counter. join_awaiter registers its own waiter_stop_release on
    // this chain's token and re-reads outstanding() under the list's mutex, so a
    // count that already reached zero refuses the link and this returns false.
    m_join.emplace(m_state->shared().counter());
    return m_join->await_suspend(h);
}
```

Two conventions this relies on and must cite in comments: `completion_counter::join()` is reused as-is — the spec left that open — because the resumption election between "the last child retired" and "the chain was cancelled" is exactly what `waiter_list::claim` already decides, and a bespoke single-joiner election would be that protocol rewritten without `waiter_list`'s tests. The cost is one mutex and one node per combinator for a queue that holds at most one waiter, which is cheaper than the proof.

And the *verdict* does not come from the counter: a cancelled child still fires its hook, so the children's stop can drive the count to zero and resume this chain **satisfied**. `combinator_state::cancel()` setting its flag before requesting the children's stop is what makes `await_resume` able to tell the two apart.

- [ ] **Step 5: Implement `when_all_awaiter::await_resume` and the destructor**

```cpp
[[nodiscard]] std::expected<Results, std::error_code> await_resume()
{
    const alt::wait_result waited = m_join->await_resume();

    if(!waited || m_state->shared().cancelled())
    {
        return std::expected<Results, std::error_code>{std::unexpect, wait_released_error()};
    }

    // Satisfied and not cancelled, so every child's body ran to its end and filled
    // its slot: the only thing that stops a child body is cancel(), and that sets
    // the flag tested above. May rethrow a child's exception.
    return assemble_combinator_results<Results>(std::move(m_state->slots()));
}

~when_all_awaiter()
{
    if(m_state != nullptr)
    {
        m_state->shared().release();
    }
}
```

The move constructor transfers `m_state` and leaves the source null, and asserts `!m_join.has_value()`: the awaiter is moved exactly once, into `cancellation_checked`'s storage, before any await member runs. `wait_awaiter`'s own move constructor carries the same assert for the same reason.

- [ ] **Step 6: Implement `when_all_awaitable` and `alt::when_all`**

`when_all_awaitable` holds `std::tuple<Awaitables...> m_awaitables` and nothing else; its `operator co_await() &&` returns a `when_all_awaiter` holding a pointer to that tuple. The awaitable is stored by value inside `detail::cancellation_checked_awaitable`, which lives for the whole full-expression, so the pointer is stable — the same arrangement `detail::expected_awaitable`/`expected_awaiter` uses.

`Slots` is `std::tuple<when_all_slot_t<Awaitables>...>`; `Results` is `std::tuple<when_all_value_t<Awaitables>...>`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R WhenAllTest`
Expected: PASS, all tests.

- [ ] **Step 8: Run the whole suite and the sanitizers**

Run:
```bash
ctest --preset debug --output-on-failure
cmake --preset debug-asan && cmake --build --preset debug-asan && ctest --preset debug-asan --output-on-failure
cmake --preset debug-tsan && cmake --build --preset debug-tsan && ctest --preset debug-tsan --output-on-failure
```
Expected: PASS on all three; report every failure by name, including pre-existing ones.

- [ ] **Step 9: Mutation-check the three ordering claims**

| Mutation | Test that must fail |
|---|---|
| Move `counter().complete()` (the starter) above the child-start loop | `AChildCompletingOnAnotherThreadDuringTheFanOutDoesNotResumeEarly` |
| Move `shared().link(context->token)` below the child-start loop | `CancellingTheAwaitingChainRequestsEveryChildsStop` |
| Drop `|| m_state->shared().cancelled()` from `await_resume` | `CancellingTheAwaitingChainReportsOperationCanceled` |

Run each 200 times in the debug preset and under TSan; record every count, including zeros.

- [ ] **Step 10: Commit**

```bash
git add include/alt/when_all.hpp tests/test_when_all.cpp tests/CMakeLists.txt
git commit -m "Add alt::when_all over a fixed argument list"
```

---

### Task 4: The range form

**Files:**
- Modify: `include/alt/when_all.hpp`
- Modify: `tests/test_when_all.cpp`

**Interfaces:**
- Consumes: everything Task 3 produced.
- Produces:

```cpp
namespace alt
{
    /**
     * Awaits every element of @p range concurrently. Yields
     * std::expected<std::vector<V>, std::error_code>, where V is the element's
     * value type by the same mapping the argument-list form uses.
     */
    template<std::ranges::sized_range R>
        requires awaitable<std::ranges::range_reference_t<R>>
    [[nodiscard]] auto when_all(R&& range);
}
```

`sized_range` rather than `input_range`: the child count must be known before the first child starts, because the starter count is `N + 1`. The spec's non-goal on infinite and streaming ranges already excludes what this rules out, and `docs/when_all.md` says so.

No ambiguity with the variadic overload: that one requires each argument to satisfy `alt::awaitable`, which a range of awaitables does not.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(WhenAllTest, ARangeOfTasksYieldsAVectorOfResults)
// std::vector<alt::task<int>> of three -> vector{1, 2, 3} in range order

TEST_F(WhenAllTest, ARangeOfVoidTasksYieldsAVectorOfMonostate)
// the vector form's answer to a void child: the element type is uniform

TEST_F(WhenAllTest, AnEmptyRangeYieldsAnEmptyVector)   // Review Focus 4
// and the block is released: no leak under ASan

TEST_F(WhenAllTest, ARangeChildThatThrewRethrowsAtTheAwait)
TEST_F(WhenAllTest, CancellingAChainParkedOnARangeFormStopsEveryChild)
TEST_F(WhenAllTest, ARangeIsIteratedOnceAndItsElementsAreMovedIntoTheChildren)
// a move-counting awaitable: one move per element, no copies
```

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --preset debug --output-on-failure -R WhenAllTest.ARangeOf`
Expected: FAIL to compile — no matching `when_all` overload for a `std::vector<alt::task<int>>`.

- [ ] **Step 3: Implement the range overload**

`Slots` is `std::vector<when_all_slot_t<std::ranges::range_reference_t<R>>>`, sized by `std::ranges::size(range)`; `Results` is the matching `std::vector`. The awaitable holds the range by the same forwarding rule the variadic form uses, and `await_suspend`'s fan-out iterates it once, forwarding each element into its child.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --preset debug --output-on-failure -R WhenAllTest`
Expected: PASS, all tests.

- [ ] **Step 5: Commit**

```bash
git add include/alt/when_all.hpp tests/test_when_all.cpp
git commit -m "Add the range form of alt::when_all"
```

---

### Task 5: The two paths that broke revision 1

**Files:**
- Modify: `tests/test_when_all.cpp`
- Modify: `include/alt/when_all.hpp`, `include/alt/detail/combinator_state.hpp` (only if a test forces a fix)

**Interfaces:** consumes everything above; produces no new API.

The spec requires a test for each of the two paths whose existence falsified revision 1's central guarantee, plus a direct test that a cancelled combinator leaves no child running. These are the tests the whole design exists to pass, so they are a task of their own rather than a footnote to Task 3.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(WhenAllTest, ASatisfierThatBeatsTheStopStillDrainsIntoTheSharedBlock)
// Revision 1's path (a). A child parked on a task_completion_source<int>.
// Two threads meeting at alt_test::rendezvous: one sets the source, the other
// requests the chain's stop. waiter_list::claim decides which wins; whichever
// does, the child writes its slot into a block the chain may already have
// abandoned. Assert: no sanitizer report under ASan and TSan, alt_test::live_frames
// returns to 0, and the parent reported either outcome without reading an
// unfilled slot. Repeat 200 times in-process.

TEST_F(WhenAllTest, AHomeSchedulerDefersTheParentsDestructionPastItsChildrens)
// Revision 1's path (b), single-threaded and deterministic. alt::manual_scheduler
// as the chain's home. Park children on async_events, request_stop(), then
// drain(). The parent root's destruction is posted and runs before the children's
// posted destructions, so the children's complete() calls land after the frame
// that used to own the counter is gone. Assert: live_frames == 0 after the drain,
// the block's erased deleter ran exactly once, and no use-after-free.

TEST_F(WhenAllTest, CancellingTheChainWhileEveryChildIsStoppedReportsCanceledNotSatisfied)
// The ordering bug combinator_state::cancel() exists to prevent: the children's
// stop drives the counter to zero, which resumes the chain through the satisfied
// path. The flag is what makes the verdict right.

TEST_F(WhenAllTest, NoChildIsLeftRunningAfterTheChainIsCancelled)
// The spec's direct requirement. Children parked on separate async_events;
// after request_stop() and a drain, live_frames == 0 and setting each event
// afterwards resumes nothing.

TEST_F(WhenAllTest, AStopAfterTheWaitWasSatisfiedRunsOverABlockTheAwaiterHasLeft)  // Review Focus 5
// Satisfy every child, let the parent resume and drop its reference, then request
// the chain's stop so the link callback runs with only children's references (or
// none) holding the block. Assert no sanitizer report.

TEST_F(WhenAllTest, ANestedWhenAllInsideAChildIsItsOwnRoot)
// A child whose body awaits its own when_all. Cancelling the outer chain must
// tear down both levels and leave live_frames at 0.
```

- [ ] **Step 2: Run them to verify they fail or pass, and say which**

Run: `ctest --preset debug --output-on-failure -R WhenAllTest` and the same under `debug-asan` and `debug-tsan`.

These are regression tests for a design, so some may pass on the first run. **Record for each test whether it passed immediately.** A test that passed immediately has not been watched to fail, so it does not yet prove anything: Step 3 is what makes it a test.

- [ ] **Step 3: Mutation-check every test in this task**

For each test above, apply a mutation that breaks the property it names, confirm the test fails, record the count over 200 repeats, and revert:

| Property | Mutation |
|---|---|
| The block outlives both the chain and the children | Have `when_all_awaiter::~when_all_awaiter` `delete` the state instead of calling `release()` |
| Children are their own roots | Pass `context->token` as the child's token instead of `children_token()`, and relay `context` into the child |
| The released flag precedes the children's stop | Swap the two statements in `combinator_state::cancel()` |
| The hook retires before it releases | Swap the two calls in `on_combinator_child_complete` |
| Children inherit `home` | Drop `child_options.home = context->home` |

Record every number, including zeros. A zero means the test cannot reach the path it claims to test — restructure the test and say so in the report; do not reword the claim to match the measurement.

- [ ] **Step 4: Fix whatever the tests force, then re-run all eight gates**

Run the full `CLAUDE.md` sweep: debug, debug-asan, debug-tsan, release-tests, the pinned `clang-tidy-19` configure-and-build, the cppcheck target, `clang-format` on changed files, and the coverage preset with its `coverage` target. Capture each preset's pass/fail summary line in full — a truncated capture has twice reported a gate it never read.

- [ ] **Step 5: Commit**

```bash
git add tests/test_when_all.cpp include/alt/when_all.hpp include/alt/detail/combinator_state.hpp
git commit -m "Pin the two paths that falsified the combinators spec's first revision"
```

---

### Task 6: Record the claim-election argument and document `when_all`

**Files:**
- Modify: `include/alt/async_latch.hpp:58-77`
- Modify: `include/alt/when_all.hpp` (class documentation)
- Create: `docs/when_all.md`
- Modify: `README.md` (Components table)

**Interfaces:** documentation only; no API change.

- [ ] **Step 1: Replace the `async_latch` note's argument**

The note currently says `count_down()` may resume a multi-node chain because "nothing in the library lets one waiter own a sibling's frame, because there is no `when_all` or race combinator and a task is lazy and owns only the child it awaits", and names the four-component revisit it would force. A combinator now exists, so that sentence is false as written, and the predicted revisit did **not** become necessary. Replace it with the reason it did not, which is the claim election:

- `alt::when_all` exists, and it does not create the hazard, because no combinator ever owns or destroys a child frame — invariant (A). Children are self-owning `cancellable_task`s and the combinator only requests stops.
- A loser whose node a satisfier already won loses the CAS in `waiter_list::claim`, so its `waiter_stop_release` resumes and frees nothing — invariant (B). The only way a child frame dies is the child itself running, which happens only when the walk reaches it.
- Name the four components the old note named — `async_event::set`, this `count_down`, `async_semaphore::release` and the four destructors with their `~waiter_list` backstop — and say that what protects them is now (A) and (B) rather than the absence of a combinator, so a change to `claim` or `resume_claimed` is what would force the revisit, not the arrival of a combinator.
- Point at `include/alt/when_all.hpp`, so the next reader of either file finds the other.

- [ ] **Step 2: Write the class documentation on `when_all`**

Cover, in the register the other components' headers use:

- What it yields, and that cancellation travels in the `std::expected` as `std::errc::operation_canceled` because the resumed body runs on with no further check.
- The lvalue precondition from Task 3: an awaitable passed as an lvalue, and everything it refers to, must outlive the children — with a `home` scheduler, its drain. An rvalue awaitable is owned by its child and carries no such requirement.
- That each child is its own cancellation root, with invariants (A), (B) and (C) stated, and a pointer to `async_latch.hpp`'s note.
- That children inherit the chain's `home`, and that the shared block is what makes that safe.
- That one allocation happens per `when_all`, including the zero-child case.
- That a child's exception is rethrown at the await, lowest position first, and the other children's results are then discarded.
- That the range form requires a sized range.

- [ ] **Step 3: Write `docs/when_all.md` and add the README row**

`docs/when_all.md` in the shape of `docs/task_group.md` (177 lines) and `docs/task_completion_source.md` (191): what it is for, both forms with worked examples, the result type and how to test it, cancellation and teardown, the preconditions, and the resource-loss note deferred to stage B's `when_any`.

README row, in the Components table after `task_group.hpp`:

```
| `<alt/when_all.hpp>` | Awaits several awaitables concurrently and yields every result: `when_all` | [docs/when_all.md](docs/when_all.md) |
```

- [ ] **Step 4: Verify the build and the full sweep**

Run: the eight gates again — a Doxygen comment change can still fail `clang-tidy` and `clang-format`.

- [ ] **Step 5: Commit**

```bash
git add include/alt/async_latch.hpp include/alt/when_all.hpp docs/when_all.md README.md
git commit -m "Record the claim-election argument and document alt::when_all"
```

---

## Not in this plan

- **`when_any`**, winner election, cancel-the-losers and the loser resource-loss documentation. Stage B, per the spec's staging.
- **The version bump.** `CLAUDE.md` makes it a standalone commit on the branch touching all six files, and it has not been discussed for this branch. Ask before Task 1 or after Task 6; do not bundle it into a feature commit.

## Self-Review

**1. Spec coverage.** Every section of the spec maps to a task:

| Spec | Task |
|---|---|
| Decision 1, invariants (A)(B)(C) | Global Constraints; realized in 3; recorded in 6 |
| Decision 1's `async_latch` note update | 6 |
| Decision 2, refcounted state off the frame | 1, and its two falsifying paths in 5 |
| Decision 3, a bespoke awaiter | 3 |
| Decision 4, one stop source and inherited `home` | 1 (source), 3 (`home`) |
| Decision 5, `expected<Results, error_code>` | 3 (`await_resume`), tested in 3 and 5 |
| `when_all` positional results, `void` as `monostate` | 2, 3, 4 |
| A child's exception rethrown at assembly | 2, 3 |
| Builder question: shared block layout and refcounting | 1 |
| Builder question: `completion_counter` as-is or a single-joiner path | 3, Step 4 — resolved as-is, with the reason |
| Builder question: `void` in the `vector` form | 2, 4 — `std::vector<std::monostate>`, since a range's element type is uniform |
| Builder question: the child wrapper's shape | 3 |
| Builder question: test strategy, both revision-1 paths, a mutation check per race or ordering test | 5, plus mutation steps in 1, 2 and 3 |

Nothing in the spec is unassigned. Decisions 6, 7 and 8's `when_any` half is explicitly out of scope above.

**2. Step scan.** Four steps carry code bodies: `cancel()`/`release()` (Task 1), the child body (Task 3), `await_suspend` and `await_resume` (Task 3). Each is an ordering the signature and tests do not determine, which is the stated exception. Every other step is a signature, a test name with its assertions, or a command with its expected output. No step says "handle edge cases" or names a type no task defines.

**3. Type consistency.** Checked across tasks: `combinator_state` (1) is used by name in 3 and 5; `when_all_value_t`/`when_all_slot_t`/`make_combinator_slots`/`assemble_combinator_results` (2) are spelled identically in 3 and 4; `on_combinator_child_complete` (3) matches `alt::completion_handler`'s `void (*)(void*) noexcept`; `children_token()` is used in 3 and mutated in 5 under that name. `combinator_value_t` is the general mapping and `when_all_value_t` the awaitable-facing alias — both are defined in Task 2 and neither is used before it.

**4. Review Focus.** Five uncovered input classes identified, each pinned to a task: 1 and 2 and 3 to Task 3, 4 to Task 4, 5 to Task 5. A sixth candidate — the same awaitable passed twice — is a precondition violation rather than a behavior, and is documented in Task 6 instead of tested.

**5. Proportion.** Measured, not estimated: the spec is 288 lines and this plan is 879, a ratio of 3.1. That is at the edge of the "several times longer is a transcript" threshold rather than comfortably under it, so the check is worth stating precisely. Where the length sits: four code blocks, all of them orderings the signatures and tests do not determine; roughly 230 lines of test names with their assertions, which is the part a plan cannot delegate because the assertion *is* the decision; and six mutation tables. What is **not** here is any function body the signature already determines, and no test is written out in full. An earlier draft of this line asserted a 2:1 ratio from memory and was wrong in the flattering direction, which is the failure mode this project has recorded before.

**One thing I changed from my own reading of the spec while writing this.** The spec left open whether `completion_counter` could be used as-is. It can, but not for the reason that looked obvious: a cancelled child still fires its completion hook, so the children's stop can drive the counter to zero and resume the awaiting chain through the **satisfied** path with every slot unfilled. The counter cannot be the verdict. That is why `combinator_state::cancel()` sets its flag before requesting the children's stop, why `await_resume` tests `cancelled()` as well as the wait result, and why both have a mutation check. Without that ordering, reusing `completion_counter` would be a silent read of unfilled slots on the most ordinary cancellation path there is.

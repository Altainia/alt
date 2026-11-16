#include <gtest/gtest.h>

#include <alt/cancellable_task.hpp>
#include <alt/detail/async_wait.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "coroutine_test_support.hpp"

namespace
{
	using alt::detail::wait_awaiter;
	using alt::detail::waiter_list;
	using alt::detail::waiter_state;
	using alt_test::rendezvous;
	using alt_test::spin_limit;

	/** The one error every released alt wait reports. */
	constexpr std::errc released_error = std::errc::operation_canceled;

	/** The handle type an alt::cancellable_task's frame is named by. */
	using root_handle = std::coroutine_handle<alt::cancellable_task::promise_type>;

	/**
	 * Whether this build still has the assert the first test below observes.
	 *
	 * detail::waiter_list carries its liveness sentinel in every configuration and
	 * only the checks on it are assert-guarded, so under NDEBUG a stop callback
	 * reaching a destroyed list returns claim_result::not_linked and nothing
	 * observable happens -- the refused-link leak is still there and still a defect,
	 * but no test in this file can see it. That is a property of the build, so the
	 * test skips and says so rather than passing vacuously.
	 */
#if defined(NDEBUG)
	constexpr bool asserts_are_compiled_in = false;
#else
	constexpr bool asserts_are_compiled_in = true;
#endif

	/**
	 * Whether ThreadSanitizer is instrumenting this build. Nested rather than written
	 * as one condition because the two compilers spell it differently and GCC before
	 * 14 does not have __has_feature at all, where a single `defined(__has_feature) &&
	 * __has_feature(...)` would not short-circuit in the preprocessor.
	 */
#if defined(__SANITIZE_THREAD__)
	constexpr bool thread_sanitizer_is_active = true;
#elif defined(__has_feature)
#	if __has_feature(thread_sanitizer)
	constexpr bool thread_sanitizer_is_active = true;
#	else
	constexpr bool thread_sanitizer_is_active = false;
#	endif
#else
	constexpr bool thread_sanitizer_is_active = false;
#endif

	/** Why the exit test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "waiter_list's liveness precondition is an assert, and NDEBUG compiles it away, so a stop "
	  "callback left behind on a destroyed list does nothing this test can observe. The release "
	  "build therefore has no coverage of this fix at all, which is the same gap waiter_list's own "
	  "death tests record.";

	/** Why the exit test is skipped under ThreadSanitizer. */
	constexpr std::string_view skipped_under_thread_sanitizer =
	  "a GoogleTest death or exit test forks, and ThreadSanitizer does not support fork, so the "
	  "child would die of the tool rather than report its own exit status. What this test pins is "
	  "single-threaded by construction, so the debug and AddressSanitizer presets both run it; the "
	  "contended test at the bottom of this file is the one ThreadSanitizer is here for.";

	/**
	 * @brief Sets GoogleTest's death test style for one scope and puts back what was
	 *        there.
	 *
	 * GTEST_FLAG_SET writes a process-global flag and GoogleTest offers nothing scoped
	 * of its own, so a death or exit test that sets it leaves it set for everything
	 * after it in the same process.
	 *
	 * Deliberately a second copy of the holder tests/test_waiter_list.cpp carries,
	 * rather than a shared one: the only header both files already include is
	 * coroutine_test_support.hpp, which does not depend on GoogleTest, and this class
	 * cannot be put there without making every test translation unit's support header
	 * depend on it. Copying the shape is the smaller cost; what must not be copied is a
	 * bare GTEST_FLAG_SET, which leaks the flag into every test that runs afterwards.
	 */
	class scoped_death_test_style
	{
	public:
		explicit scoped_death_test_style(const std::string& style): m_previous(GTEST_FLAG_GET(death_test_style))
		{
			GTEST_FLAG_SET(death_test_style, style);
		}

		~scoped_death_test_style()
		{
			GTEST_FLAG_SET(death_test_style, m_previous);
		}

		scoped_death_test_style(const scoped_death_test_style&)            = delete;
		scoped_death_test_style& operator=(const scoped_death_test_style&) = delete;
		scoped_death_test_style(scoped_death_test_style&&)                 = delete;
		scoped_death_test_style& operator=(scoped_death_test_style&&)      = delete;

	private:
		/** What the flag held before this object changed it. */
		std::string m_previous;
	};

	/** Spins until the predicate holds, yielding once the spin budget is spent. */
	template<typename Predicate>
	void spin_until(Predicate predicate)
	{
		for(int spins = 0; !predicate();)
		{
			if(spins < spin_limit)
			{
				++spins;
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	/**
	 * @brief Captures the typed handle of the alt::cancellable_task awaiting it and
	 *        leaves that frame suspended for good.
	 *
	 * A detail::wait_awaiter only registers a stop callback when its awaiting promise
	 * relays a cancellation context whose token can be stopped, so these tests cannot
	 * drive await_suspend with std::noop_coroutine() the way the hand-driven semaphore
	 * tests do. alt::cancellable_task's promise is the one in the library that supplies
	 * such a context, and alt::start is what fills its token in, so a real started root
	 * is the handle to drive with. The frame is parked here and never resumed: every
	 * test below destroys it by hand at the end.
	 */
	struct capture_root_handle
	{
		root_handle* out;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		bool await_suspend(root_handle h) const noexcept
		{
			*out = h;
			return true;
		}

		void await_resume() const noexcept
		{}
	};

	/** A root that parks forever, so a test can drive an awaiter with its handle. */
	alt::cancellable_task parked_root(root_handle* out)
	{
		co_await capture_root_handle{out};
	}

	/**
	 * @brief The smallest detail::wait_awaiter there is.
	 *
	 * Supplies the two members the base asks of a Derived and nothing else, because
	 * these tests are about the base rather than about any one primitive: a ready check
	 * the test drives by hand, and a list the test owns. Deriving is also what grants
	 * access to the base's private members, wait_awaiter befriending its Derived, so
	 * the two observers at the bottom read state that has no public accessor and did
	 * not grow one for a test.
	 *
	 * The ready check is consulted at most twice per wait -- once by await_ready and
	 * once as the condition on the link, under the list's mutex -- which is what the
	 * condition hook exploits. Installing the hook between those two calls is the only
	 * way a test gets to run code on the awaiting thread after the stop callback has
	 * been registered and before the link has been refused.
	 */
	class probe_awaiter: public wait_awaiter<probe_awaiter>
	{
	public:
		explicit probe_awaiter(waiter_list& list) noexcept: m_list(&list)
		{}

		/**
		 * Makes every ready check from here on report the wait over, which is how a test
		 * refuses the link from inside the condition.
		 *
		 * A member rather than a bool the test owns and the probe points at, which is
		 * what this was first written as: clang-analyzer-deadcode.DeadStores cannot see
		 * the read through the pointer and reports the test's `satisfied = true` as a
		 * dead store, and suppressing a check that is right about what it can see is
		 * worse than owning the flag here.
		 */
		void satisfy_before_the_link() noexcept
		{
			m_satisfied = true;
		}

		/**
		 * Runs @p hook from inside link()'s condition, with the list's mutex held. It
		 * must therefore not touch the list, exactly as the condition itself must not.
		 */
		void run_this_inside_the_condition(std::function<void()> hook)
		{
			m_hook = std::move(hook);
		}

		[[nodiscard]] bool ready() const
		{
			if(m_hook)
			{
				m_hook();
			}

			return m_satisfied;
		}

		[[nodiscard]] waiter_list& list() const noexcept
		{
			return *m_list;
		}

		/** Whether a std::stop_callback is still held. The state the fix is about. */
		[[nodiscard]] bool holds_a_stop_callback() const noexcept
		{
			return m_stop.has_value();
		}

		/** Who owns this waiter's resumption, as the claim election last left it. */
		[[nodiscard]] waiter_state node_state() const noexcept
		{
			return m_node.state.load(std::memory_order_acquire);
		}

	private:
		waiter_list*          m_list;
		bool                  m_satisfied = false;
		std::function<void()> m_hook;
	};
}

// The hazard this file exists for, end to end: an awaiter whose link was refused, a
// primitive destroyed while that awaiter is still alive, and only then a stop.
//
// EXPECT_EXIT rather than EXPECT_DEATH, because what is pinned here is the ABSENCE of
// an abort. Run inline, the mutation this test guards would abort the whole test
// binary and take every test scheduled after it with it, which is the failure mode
// that hid stage A's per-test attribution. Forking keeps the finding attributable: the
// child aborts, the parent reports one failed expectation and carries on.
//
// The parent builds the awaiter and the list too, and never reaches the three
// statements in the child's block, so nothing in the parent is left holding a callback
// over a destroyed list whichever way the expectation goes.
//
// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_EXIT's own expansion is what measures over the threshold of 25, not this body. Measured with the pinned clang-tidy-19 at threshold 0: this body scores 55, the contended test at the bottom of this file scores 24 unsuppressed, and the already-stopped-token test between them scores 0 -- the difference being the macro, which tests/test_waiter_list.cpp measured as counting only once some `if` precedes it in the same function. The two skip guards here are that `if`, and they cannot move, GTEST_SKIP only skipping the function it is written in. What this body actually contains is two skip guards, four preconditions and one expectation.
TEST(AsyncWaitTest, ARefusedLinkLeavesNoCallbackBehindToClaimADeadList)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << skipped_under_thread_sanitizer;
	}

	// The child re-executes this binary rather than forking a process that has already
	// run every test ahead of this one. GoogleTest recommends this style for a
	// multi-threaded program, and this suite is one. Scoped so the flag does not
	// outlive this test; see scoped_death_test_style.
	const scoped_death_test_style style{"threadsafe"};

	// Declared before the awaiter, so that the awaiter and the callback it may be
	// holding are destroyed first.
	std::optional<waiter_list> list{std::in_place};

	// Named while the optional is still engaged, as tests/test_waiter_list.cpp's death
	// tests name theirs: _GLIBCXX_ASSERTIONS aborts inside <optional> on operator-> of
	// a reset optional, and a stop callback holds a pointer into the block anyway
	// rather than an optional.
	waiter_list* const storage = &*list;

	root_handle root{};
	auto        stop = alt::start(parked_root(&root));
	ASSERT_TRUE(root);

	// The wait is not over at the ready check and is over at the link, which is the
	// ordinary way a link is refused with a stop callback already registered.
	probe_awaiter awaiter{*storage};

	ASSERT_FALSE(awaiter.await_ready());
	awaiter.satisfy_before_the_link();

	ASSERT_FALSE(awaiter.await_suspend(root));
	ASSERT_TRUE(awaiter.await_resume().has_value());

	EXPECT_EXIT(
	  {
		  // The primitive dies with the awaiter still alive. Its storage is destroyed
		  // rather than freed, so waiter_list's sentinel reads dead_pattern and the
		  // precondition below is reachable deterministically.
		  list.reset();

		  // Without the reset in await_suspend's refused-link tail this runs the
		  // still-registered waiter_stop_release, which claims a destroyed list and
		  // aborts on "claiming an alt waiter on a destroyed list". With it, no callback
		  // is registered and this does nothing at all.
		  stop.request_stop();

		  // std::_Exit rather than exit: the root frame is still suspended and
		  // deliberately not destroyed here, so running atexit handlers would hand
		  // AddressSanitizer's leak check a frame this test never meant to reclaim and
		  // turn a clean exit into code 23. The parent destroys it below.
		  std::_Exit(0);
	  },
	  ::testing::ExitedWithCode(0),
	  "");

	root.destroy();
}

TEST(AsyncWaitTest, ARefusedLinkOnAnAlreadyStoppedTokenLeavesNothingRegistered)
{
	// The same end state by the other route into the refused-link tail. A
	// std::stop_callback constructed over a token that has already been stopped invokes
	// its callback in the constructor and is not registered with the stop state at all
	// -- [stopcallback.cons] requires both -- so the reset here runs against an
	// invocation that has already completed, and no later stop can reach this awaiter
	// however long it lives. That is why this route cannot be made to abort the way the
	// test above does, and why the observable is the awaiter's own state: after the
	// refusal it must hold nothing.
	//
	// Which also makes this the test that pins the reset being a no-op where there is
	// nothing to deregister, rather than something that has to be reached around a
	// branch.
	waiter_list list;

	root_handle root{};
	auto        stop = alt::start(parked_root(&root));
	ASSERT_TRUE(root);

	// Never satisfied: this link is refused by the election, not by the condition.
	probe_awaiter awaiter{list};

	ASSERT_FALSE(awaiter.await_ready());

	stop.request_stop();

	EXPECT_FALSE(awaiter.await_suspend(root));

	// The inline invocation won the node, so link() refused on the election rather than
	// on the condition, and the wait is the released one it was before.
	EXPECT_EQ(awaiter.node_state(), waiter_state::claimed);
	EXPECT_FALSE(awaiter.holds_a_stop_callback());

	const auto result = awaiter.await_resume();
	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error(), released_error);

	root.destroy();
}

// A stop callback of this awaiter's own, genuinely in flight on another thread, while
// the refused-link tail runs on this one.
//
// WHAT THIS TEST OBSERVES, AND WHAT IT DOES NOT. The reset is safe only because
// ~std::stop_callback blocks for an invocation already running on another thread, and
// the honest statement of this test is that it pins the OUTCOME of that and not the
// ORDERING. It shows that the tail deregisters, and completes rather than deadlocking,
// with an invocation of the same callback demonstrably in flight elsewhere. It does not
// show that m_stop.reset() waited for that invocation rather than arriving a moment
// after it finished on its own.
//
// Nothing short of instrumenting the library closes that gap, and the construction that
// would is worth recording so the next reader does not re-derive it. The callback's only
// blocking point is the list's mutex, so pinning the ordering means holding that mutex
// across the reset, and the only party that can hold it is a link() condition or a
// detach_*() action. This thread's own condition is the one hook it has after the
// callback is registered, and link() drops the mutex before the tail begins. A third
// thread queued on the mutex would do it -- the callback would stay in flight for as
// long as that thread held it, and a reset that waited would provably return after it
// -- but whether that thread or the callback wins the mutex when link() drops it is a
// std::mutex handoff, not something a rendezvous decides. A test that asserted the
// ordering would therefore be asserting a coin flip, and one that looped until it saw
// the informative interleaving would be a gate that can fail on a loaded or single-core
// runner. So the ordering is argued from ~std::stop_callback's specification and left
// unasserted, rather than claimed by a test name.
//
// Not looped. The rendezvous makes the registration and the in-flight claim
// deterministic rather than probabilistic, so repeats would only vary the mutex handoff
// the test does not assert on. Its value under ThreadSanitizer is in running at all.
//
// This body measures 24 against readability-function-cognitive-complexity's threshold of
// 25 with the pinned clang-tidy-19, so it carries no suppression and one more branch
// would need one.
TEST(AsyncWaitTest, ARefusedLinkDeregistersWhileAStopCallbackRunsElsewhere)
{
	// Declared first, so it outlives the awaiter and anything the awaiter's callback is
	// still doing to it.
	waiter_list list;

	root_handle root{};
	auto        stop = alt::start(parked_root(&root));
	ASSERT_TRUE(root);

	probe_awaiter awaiter{list};

	ASSERT_FALSE(awaiter.await_ready());

	// Set after the ready check and before the suspension, so the link is refused by
	// the condition rather than by the election. The election is still held when
	// link() tests it, the stop not having been requested yet, so the condition really
	// does run -- which is the whole hook this test hangs on.
	awaiter.satisfy_before_the_link();

	std::atomic<std::size_t> arrived{0};

	{
		std::jthread stopper{[&] {
			rendezvous(arrived, 2);
			stop.request_stop();
		}};

		awaiter.run_this_inside_the_condition([&] {
			// Reached with the list's mutex held, and after await_suspend has registered
			// the callback over a token that was not yet stopped. Releasing the stopper
			// here is what makes the registration ordering deterministic instead of a
			// race: it cannot run until this thread arrives.
			rendezvous(arrived, 2);

			// claim() moves the node to claimed without the mutex and only then reaches
			// for the mutex this thread is holding, so observing the state word is
			// observing an invocation that has started and cannot yet finish. No
			// deadlock: this spin waits for a store made without the lock.
			spin_until([&] { return awaiter.node_state() == waiter_state::claimed; });
		});

		EXPECT_FALSE(awaiter.await_suspend(root));
	}

	// The callback has finished by now, the stopper having been joined, so this reads
	// the tail's work rather than a race with it: nothing is left registered.
	EXPECT_FALSE(awaiter.holds_a_stop_callback());

	// And the verdict is the condition's, not the callback's. The callback won a node
	// that was on no list and resumed nothing, which is rule 4: a condition that
	// refused because the primitive was satisfied is never reported cancelled, however
	// the state word reads afterwards.
	//
	// The state word itself is deliberately NOT asserted here. The in-hook spin above
	// already waited for exactly waiter_state::claimed, and nothing on the not_linked
	// path rewrites it -- await_suspend's tail writes m_stop, m_parked and m_released
	// and never the node -- so an EXPECT_EQ on it could not fail once this line is
	// reached. The failure mode it looks like it would guard is a callback that never
	// claims the node, and that one hangs in the spin above and is reported by ctest's
	// 60-second timeout instead. An assertion was here and was removed for being
	// decorative rather than wrong; do not put it back without a way to make it fail.
	EXPECT_TRUE(awaiter.await_resume().has_value());

	root.destroy();
}

#include <gtest/gtest.h>

#include <alt/detail/waiter_list.hpp>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

namespace
{
	using alt::detail::claim_result;
	using alt::detail::resume_claimed;
	using alt::detail::waiter_list;
	using alt::detail::waiter_node;
	using alt::detail::waiter_state;

	/** How many times the two contended tests repeat their race. */
	constexpr int race_repeats = 100;

	/**
	 * How many nodes one repeat of the election test contends. Each node is a
	 * separate two-thread race, so the repeats multiply rather than just repeat.
	 */
	constexpr std::size_t contended_nodes = 32;

	// The build-detection constants, the death test style holder and the lead sentence of
	// the ThreadSanitizer skip message are shared from tests/death_test_support.hpp,
	// which records why they live there. The assert skip message below stays local,
	// naming this file's own assert, and so does the ThreadSanitizer message's second
	// sentence, which says something true only here.
	using alt_test::asserts_are_compiled_in;
	using alt_test::scoped_death_test_style;
	using alt_test::thread_sanitizer_forks_death_tests;
	using alt_test::thread_sanitizer_is_active;

	/** Why a death test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "the liveness precondition is an assert, and NDEBUG compiles it away, so this build has no "
	  "guard for a death test to observe. That is a real gap rather than a configuration detail: a "
	  "release build gets no liveness check at all, which waiter_list's own notes state.";

	/**
	 * What this file adds to the shared ThreadSanitizer skip reason, streamed after
	 * thread_sanitizer_forks_death_tests.
	 *
	 * Local because it is true only here: the shared lead states why any forked death
	 * test cannot run under the tool, and this says why losing this particular one
	 * costs nothing.
	 */
	constexpr std::string_view this_test_is_single_threaded =
	  "What this test pins is single-threaded by construction and nothing about it is "
	  "sanitizer-specific, so the debug and AddressSanitizer presets both run it.";

	// The two-party rendezvous both contended tests below are built around lives in
	// coroutine_test_support.hpp, because alt::async_event's tests need the same one.
	using alt_test::rendezvous;

	/**
	 * @brief Holds one object in storage that outlives it, so the death tests can reach
	 *        an object that has been destroyed but whose bytes have not been handed back.
	 *
	 * Deliberately not std::optional, which did this job until MSVC: its standard library
	 * annotates std::optional for AddressSanitizer and poisons the storage of an empty
	 * one, so under MSVC's ASan a call through a pointer into a reset optional is
	 * reported as use-after-poison before the sentinel's assert can run. Raw aligned
	 * storage with std::destroy_at ends the object's lifetime the same way, leaves the
	 * sentinel's dead_pattern in place, and carries no library's annotations.
	 *
	 * get() works before and after destroy(), which is the point: a stop callback that
	 * outlived its block holds exactly such a pointer. It also sidesteps the libstdc++
	 * hazard the optional had, where _GLIBCXX_ASSERTIONS made operator-> on a reset
	 * optional abort inside <optional> before the call under test was reached.
	 */
	template<typename ValueType>
	class destroyable_in_place
	{
	public:
		destroyable_in_place(): m_object{std::construct_at(reinterpret_cast<ValueType*>(m_storage.data()))}
		{}

		~destroyable_in_place()
		{
			if(m_alive)
			{
				std::destroy_at(m_object);
			}
		}

		destroyable_in_place(const destroyable_in_place&)            = delete;
		destroyable_in_place& operator=(const destroyable_in_place&) = delete;
		destroyable_in_place(destroyable_in_place&&)                 = delete;
		destroyable_in_place& operator=(destroyable_in_place&&)      = delete;

		/** The object's address, valid to hold, though not to use, after destroy(). */
		[[nodiscard]] ValueType* get() const noexcept
		{
			return m_object;
		}

		/** Ends the object's lifetime and leaves its storage intact. */
		void destroy() noexcept
		{
			std::destroy_at(m_object);
			m_alive = false;
		}

	private:
		alignas(ValueType) std::array<std::byte, sizeof(ValueType)> m_storage{};
		ValueType* m_object;
		bool       m_alive = true;
	};

	/**
	 * A real, self-destroying coroutine. Its final_suspend never suspends, so the
	 * frame is freed as soon as the body finishes, which is what a resumption from
	 * resume_claimed triggers.
	 */
	struct test_waiter
	{
		struct promise_type
		{
			test_waiter get_return_object() noexcept
			{
				return test_waiter{std::coroutine_handle<promise_type>::from_promise(*this)};
			}

			std::suspend_never initial_suspend() noexcept
			{
				return {};
			}

			std::suspend_never final_suspend() noexcept
			{
				return {};
			}

			void return_void() noexcept
			{}

			void unhandled_exception()
			{
				std::terminate();
			}
		};

		std::coroutine_handle<promise_type> handle;
	};

	/**
	 * Counts its own resumptions in a coroutine whose waiter_node lives in its own
	 * frame, exactly as a real awaiter's node will. Resuming it therefore frees the
	 * storage the node occupies, which is what makes resume_claimed's
	 * read-next-before-resume rule observable rather than theoretical.
	 */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): out_node and resumptions are owned by the calling test, which resumes this coroutine itself and never leaves the scope holding them until it has, so the references cannot dangle.
	test_waiter make_counting_waiter(waiter_node*& out_node, int& resumptions, int tag)
	{
		const alt_test::frame_tracker tracker{tag};
		waiter_node                   node;
		out_node = &node;
		co_await alt_test::park{};
		++resumptions;
	}

	/** As above, but its resumption also destroys the list it was waiting on. */
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): all three referents are owned by the calling test, which resumes this coroutine itself and never leaves the scope holding them until it has, so the references cannot dangle.
	test_waiter make_list_destroying_waiter(waiter_node*& out_node, int& resumptions, std::unique_ptr<waiter_list>& owner)
	{
		waiter_node node;
		out_node = &node;
		co_await alt_test::park{};
		++resumptions;
		owner.reset();
	}

	/**
	 * Starts a counting waiter and links its node into the list. The handle comes
	 * from alt_test::park, which stores it as the coroutine suspends.
	 */
	void link_counting_waiter(waiter_list& list, int& resumptions, int tag)
	{
		waiter_node* node = nullptr;
		make_counting_waiter(node, resumptions, tag);
		node->handle = alt_test::parked;
		ASSERT_TRUE(list.link(*node));
	}

	/**
	 * What a semaphore does with its permit once detach_one has reported: nothing
	 * when it was handed to a waiter, back into the count when it was not.
	 */
	void count_back_unless_handed_over(bool detached, int& counted_back)
	{
		if(!detached)
		{
			++counted_back;
		}
	}

	/** Counts the nodes a detached chain holds, without resuming any of them. */
	std::size_t chain_length(const waiter_node* chain)
	{
		std::size_t count = 0;
		for(const waiter_node* node = chain; node != nullptr; node = node->next)
		{
			++count;
		}
		return count;
	}

	/** Links a whole batch of nodes, each with a resumable handle. */
	template<std::size_t Count>
	void link_all(waiter_list& list, std::array<waiter_node, Count>& nodes)
	{
		for(waiter_node& node: nodes)
		{
			node.handle = std::noop_coroutine();
			ASSERT_TRUE(list.link(node));
		}
	}

	/** One thread's side of the election: contend for every node, counting its wins. */
	void contend_for_every_node(waiter_list& list, std::array<waiter_node, contended_nodes>& nodes, std::array<std::atomic<int>, contended_nodes>& winners, std::atomic<std::size_t>& arrived)
	{
		for(std::size_t index = 0; index < contended_nodes; ++index)
		{
			// Rendezvous per node rather than once per thread, so every node is a
			// genuine race instead of only the first one.
			rendezvous(arrived, (index + 1) * 2);

			if(list.claim(nodes[index]) == claim_result::detached)
			{
				winners[index].fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	/** Checks the election: one winner per node, and every node left claimed. */
	void expect_one_winner_per_node(const std::array<waiter_node, contended_nodes>& nodes, const std::array<std::atomic<int>, contended_nodes>& winners)
	{
		for(std::size_t index = 0; index < contended_nodes; ++index)
		{
			ASSERT_EQ(winners[index].load(std::memory_order_relaxed), 1) << "node " << index;
			ASSERT_EQ(nodes[index].state.load(std::memory_order_acquire), waiter_state::claimed) << "node " << index;
		}
	}

	/** A satisfier releasing everyone. Returns how many waiters it resumed. */
	std::size_t detach_all_and_resume(waiter_list& list)
	{
		waiter_node* const chain = list.detach_all();
		const std::size_t  won   = chain_length(chain);
		resume_claimed(chain, true);
		return won;
	}

	/** A stop callback working node by node. Returns how many waiters it resumed. */
	template<std::size_t Count>
	std::size_t claim_and_resume_each(waiter_list& list, std::array<waiter_node, Count>& nodes)
	{
		std::size_t resumed = 0;

		for(waiter_node& node: nodes)
		{
			if(list.claim(node) == claim_result::detached)
			{
				resume_claimed(&node, false);
				++resumed;
			}
		}

		return resumed;
	}

	/** Checks that every node in a batch ended up done. */
	template<std::size_t Count>
	void expect_every_node_done(const std::array<waiter_node, Count>& nodes)
	{
		std::size_t index = 0;

		for(const waiter_node& node: nodes)
		{
			ASSERT_EQ(node.state.load(std::memory_order_acquire), waiter_state::done) << "node " << index;
			++index;
		}
	}
}

TEST(WaiterList, LinkingThenDetachingReturnsTheNodesInFifoOrder)
{
	waiter_node first{.handle = std::noop_coroutine()};
	waiter_node second{.handle = std::noop_coroutine()};
	waiter_node third{.handle = std::noop_coroutine()};

	waiter_list list;
	EXPECT_TRUE(list.empty());

	ASSERT_TRUE(list.link(first));
	ASSERT_TRUE(list.link(second));
	ASSERT_TRUE(list.link(third));
	EXPECT_EQ(list.size(), 3U);
	EXPECT_FALSE(list.empty());

	waiter_node* const chain = list.detach_all();

	ASSERT_EQ(chain, &first);
	ASSERT_EQ(chain->next, &second);
	ASSERT_EQ(chain->next->next, &third);
	EXPECT_EQ(chain->next->next->next, nullptr);

	EXPECT_EQ(list.size(), 0U);
	EXPECT_TRUE(list.empty());
}

TEST(WaiterList, ClaimElectsExactlyOneWinner)
{
	// The election is the whole correctness argument: a satisfier and a stop
	// callback both reaching for one node must not both resume it, because the
	// second resumption would run a frame the first one already destroyed.
	//
	// A load followed by a store would let both sides see linked and both win, and
	// that window is only nanoseconds wide, so the test is built to hit it: the two
	// threads rendezvous immediately before every single claim, and every repeat
	// contends a fresh batch of nodes rather than just one.
	//
	// No repeat calls alt_test::reset_coroutine_test_state(): the nodes here are bare
	// waiter_nodes with no coroutine behind them, so there is none of that state to
	// clear.
	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		waiter_list                                   list;
		std::array<waiter_node, contended_nodes>      nodes{};
		std::array<std::atomic<int>, contended_nodes> winners{};
		std::atomic<std::size_t>                      arrived{0};

		link_all(list, nodes);

		{
			const auto contend = [&] { contend_for_every_node(list, nodes, winners, arrived); };

			std::jthread one{contend};
			std::jthread two{contend};
		}

		expect_one_winner_per_node(nodes, winners);

		// An assertion inside that helper only returns from the helper, so stop the
		// repeats here instead of reporting the same failure a hundred times over.
		ASSERT_FALSE(HasFailure()) << "repeat " << repeat;
		EXPECT_EQ(list.size(), 0U);
	}
}

TEST(WaiterList, ADetachedNodeIsAlreadyClaimedSoClaimLoses)
{
	waiter_node node{.handle = std::noop_coroutine()};

	waiter_list list;
	ASSERT_TRUE(list.link(node));

	waiter_node* const chain = list.detach_all();
	ASSERT_EQ(chain, &node);
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::claimed);

	// The satisfier already owns this resumption, so a late claim must lose.
	EXPECT_EQ(list.claim(node), claim_result::lost);
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::claimed);
}

TEST(WaiterList, ClaimingANodeThatWasNeverLinkedWinsButDetachesNothing)
{
	// A node is winnable from the moment it exists, which is before its waiter has
	// linked it. A stop callback claiming it then owns the resumption and must not
	// perform one: nobody is parked on a node that is on no list, and the waiter it
	// belongs to is still inside its own await_suspend. Reporting the win without a
	// detach is what lets that waiter resume itself instead.
	waiter_node node{.handle = std::noop_coroutine()};
	waiter_list list;

	EXPECT_EQ(list.claim(node), claim_result::not_linked);
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::claimed);

	// Still a one-shot election: a second claimer loses whatever the first achieved.
	EXPECT_EQ(list.claim(node), claim_result::lost);
}

TEST(WaiterList, LinkingIsRefusedForANodeAClaimerHasAlreadyWon)
{
	// The other half of the window above. Linking a node whose election is already
	// lost would park a waiter nobody will ever resume, because the winner found
	// nothing to unlink and resumed nothing. Refusing the link is what hands the
	// resumption back to the waiter itself.
	waiter_node node{.handle = std::noop_coroutine()};
	waiter_list list;

	ASSERT_EQ(list.claim(node), claim_result::not_linked);

	int conditions_checked = 0;
	EXPECT_FALSE(list.link(node, [&] {
		++conditions_checked;
		return true;
	}));

	// The election is settled before the caller's condition is asked, so a node
	// already won is refused whatever the condition would have said.
	EXPECT_EQ(conditions_checked, 0);
	EXPECT_EQ(list.size(), 0U);
	EXPECT_TRUE(list.empty());
}

TEST(WaiterList, LinkingIsRefusedWhenTheCallersConditionSaysTheWaitIsOver)
{
	// What makes a primitive's satisfied flag and its waiting queue one decision
	// rather than two. The condition is evaluated under the same mutex the
	// insertion takes, so a satisfier that ran between a waiter's own ready check
	// and this call cannot leave that waiter parked on something already satisfied.
	waiter_node node{.handle = std::noop_coroutine()};
	waiter_list list;

	EXPECT_FALSE(list.link(node, [] { return false; }));
	EXPECT_EQ(list.size(), 0U);

	// Untouched by the refusal: the node is still winnable and still off every
	// list, so its waiter owns it outright and reports its own outcome.
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::linked);
	EXPECT_FALSE(node.on_list);

	// And the same node links when the condition holds, so the refusal above was
	// the condition's doing rather than anything about the node.
	EXPECT_TRUE(list.link(node, [] { return true; }));
	EXPECT_EQ(list.size(), 1U);
	EXPECT_TRUE(node.on_list);
}

TEST(WaiterList, ClaimUnlinksSoSizeDrops)
{
	waiter_node first{.handle = std::noop_coroutine()};
	waiter_node second{.handle = std::noop_coroutine()};

	waiter_list list;
	ASSERT_TRUE(list.link(first));
	ASSERT_TRUE(list.link(second));

	ASSERT_EQ(list.claim(first), claim_result::detached);
	EXPECT_EQ(list.size(), 1U);

	// The winner's node comes back as a chain of one, ready for resume_claimed.
	EXPECT_EQ(first.prev, nullptr);
	EXPECT_EQ(first.next, nullptr);

	// A second claim of the same node loses: the first caller owns the resumption.
	EXPECT_EQ(list.claim(first), claim_result::lost);
	EXPECT_EQ(list.size(), 1U);

	// The node still linked is untouched and still detachable.
	waiter_node* const chain = list.detach_all();
	EXPECT_EQ(chain, &second);
	EXPECT_EQ(list.size(), 0U);
}

TEST(WaiterList, DetachingSkipsANodeAnotherPartyHasAlreadyClaimed)
{
	// The one window the list cannot close: a claim that has won the election but
	// has not yet taken the mutex leaves its node linked for that moment. Whoever
	// detaches in that moment has to step over the node rather than steal it or
	// stop at it. Claiming the node by hand here is what makes that window, which
	// is nanoseconds wide in a real race, deterministically testable.
	waiter_node taken{.handle = std::noop_coroutine()};
	waiter_node available{.handle = std::noop_coroutine()};

	waiter_list list;
	ASSERT_TRUE(list.link(taken));
	ASSERT_TRUE(list.link(available));

	taken.state.store(waiter_state::claimed, std::memory_order_release);

	// Stepping over it rather than stopping at it is what keeps one waiter being
	// cancelled from hiding every waiter behind it.
	waiter_node* const chain = list.detach_one();
	ASSERT_EQ(chain, &available);
	EXPECT_EQ(list.detach_one(), nullptr);

	// And detach_all leaves it linked, because its owner is the only party that may
	// unlink it. So a detach does not always empty the list.
	EXPECT_EQ(list.detach_all(), nullptr);
	EXPECT_EQ(list.size(), 1U);
	EXPECT_FALSE(list.empty());
}

TEST(WaiterList, DetachAllRunsTheCallersActionEvenWithNobodyWaiting)
{
	// A satisfier's state change and its walk are one critical section, and that
	// state change is not conditional on finding anyone: an event with no waiters
	// still has to become set, or the next waiter to arrive parks forever.
	waiter_list list;
	int         actions = 0;

	const auto mark_satisfied = [&] { ++actions; };

	// Hoisted out of the assertion macros throughout: a lambda argument inside one
	// counts against the test body's cognitive complexity for no reader's benefit.
	const waiter_node* const nothing_waiting = list.detach_all(mark_satisfied);
	EXPECT_EQ(nothing_waiting, nullptr);
	EXPECT_EQ(actions, 1);

	waiter_node node{.handle = std::noop_coroutine()};
	ASSERT_TRUE(list.link(node));

	const waiter_node* const detached = list.detach_all(mark_satisfied);
	EXPECT_EQ(detached, &node);
	EXPECT_EQ(actions, 2);
	EXPECT_EQ(list.size(), 0U);
}

TEST(WaiterList, DetachOneTellsTheCallersActionWhenItFoundNobody)
{
	// The semaphore's shape: the permit goes to a waiter if there was one and back
	// into the count if there was not, and that decision has to be made in the same
	// critical section as the search, or a waiter can park on a permit that has
	// already been given away.
	waiter_list list;
	int         counted_back = 0;

	const auto take_back_unless_handed_over = [&](bool detached) { count_back_unless_handed_over(detached, counted_back); };

	// Results are hoisted out of the assertion macros throughout: a lambda argument
	// inside one counts against the test body's cognitive complexity for no reader's
	// benefit.
	const waiter_node* const from_an_empty_list = list.detach_one(take_back_unless_handed_over);
	EXPECT_EQ(from_an_empty_list, nullptr);
	EXPECT_EQ(counted_back, 1);

	waiter_node taken{.handle = std::noop_coroutine()};
	ASSERT_TRUE(list.link(taken));
	taken.state.store(waiter_state::claimed, std::memory_order_release);

	// Linked, but already won by another party, so there is still nobody this caller
	// can hand anything to. This is the interleaving size() and empty() cannot see,
	// and the reason the answer comes from the search itself rather than from a count.
	const waiter_node* const from_a_list_of_one_already_won = list.detach_one(take_back_unless_handed_over);
	EXPECT_EQ(from_a_list_of_one_already_won, nullptr);
	EXPECT_EQ(counted_back, 2);
	EXPECT_EQ(list.size(), 1U);
}

TEST(WaiterList, DetachOneTellsTheCallersActionWhenItHandedTheWaiterOver)
{
	waiter_list list;
	int         counted_back = 0;

	const auto take_back_unless_handed_over = [&](bool detached) { count_back_unless_handed_over(detached, counted_back); };

	waiter_node available{.handle = std::noop_coroutine()};
	ASSERT_TRUE(list.link(available));

	const waiter_node* const handed_over = list.detach_one(take_back_unless_handed_over);
	EXPECT_EQ(handed_over, &available);

	// Nothing counted back, because the waiter got it.
	EXPECT_EQ(counted_back, 0);
	EXPECT_EQ(list.size(), 0U);
}

TEST(WaiterList, ResumeClaimedWritesSatisfiedAndMarksDone)
{
	waiter_node satisfied_node{.handle = std::noop_coroutine()};
	waiter_node unsatisfied_node{.handle = std::noop_coroutine()};

	waiter_list list;
	ASSERT_TRUE(list.link(satisfied_node));
	ASSERT_TRUE(list.link(unsatisfied_node));

	waiter_node* const satisfied_chain = list.detach_one();
	ASSERT_EQ(satisfied_chain, &satisfied_node);
	resume_claimed(satisfied_chain, true);

	EXPECT_TRUE(satisfied_node.satisfied);
	EXPECT_EQ(satisfied_node.state.load(std::memory_order_acquire), waiter_state::done);

	// The flag is written from the argument, not hardcoded: a waiter released
	// without the primitive being satisfied has to be able to tell.
	waiter_node* const unsatisfied_chain = list.detach_one();
	ASSERT_EQ(unsatisfied_chain, &unsatisfied_node);
	resume_claimed(unsatisfied_chain, false);

	EXPECT_FALSE(unsatisfied_node.satisfied);
	EXPECT_EQ(unsatisfied_node.state.load(std::memory_order_acquire), waiter_state::done);

	EXPECT_EQ(list.detach_one(), nullptr);
}

TEST(WaiterList, ResumeClaimedResumesEveryHandleOnce)
{
	alt_test::reset_coroutine_test_state();

	int first_resumptions  = 0;
	int second_resumptions = 0;
	int third_resumptions  = 0;

	waiter_list list;
	link_counting_waiter(list, first_resumptions, 1);
	link_counting_waiter(list, second_resumptions, 2);
	link_counting_waiter(list, third_resumptions, 3);

	ASSERT_EQ(alt_test::live_frames, 3);
	ASSERT_EQ(list.size(), 3U);

	waiter_node* const chain = list.detach_all();
	ASSERT_EQ(chain_length(chain), 3U);

	resume_claimed(chain, true);

	// Each node's storage is gone by now, so nothing is read back through first,
	// second or third: the counters and the frame tracker are the evidence.
	EXPECT_EQ(first_resumptions, 1);
	EXPECT_EQ(second_resumptions, 1);
	EXPECT_EQ(third_resumptions, 1);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_EQ(alt_test::destruction_order, (std::vector<int>{1, 2, 3}));
}

TEST(WaiterList, TheWalkTakesNoLockSoAWaiterMayDestroyTheListMidWalk)
{
	alt_test::reset_coroutine_test_state();

	auto list = std::make_unique<waiter_list>();

	int          destroyer_resumptions = 0;
	waiter_node* destroyer             = nullptr;
	make_list_destroying_waiter(destroyer, destroyer_resumptions, list);
	destroyer->handle = alt_test::parked;
	ASSERT_TRUE(list->link(*destroyer));

	int second_resumptions = 0;
	int third_resumptions  = 0;
	link_counting_waiter(*list, second_resumptions, 2);
	link_counting_waiter(*list, third_resumptions, 3);

	waiter_node* const chain = list->detach_all();
	ASSERT_EQ(chain_length(chain), 3U);

	// The first resumption destroys the list from inside the walk. Because the walk
	// holds no lock and already has the rest of the chain in locals, the remaining
	// two still resume.
	resume_claimed(chain, true);

	EXPECT_EQ(destroyer_resumptions, 1);
	EXPECT_EQ(second_resumptions, 1);
	EXPECT_EQ(third_resumptions, 1);
	EXPECT_EQ(list, nullptr);
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(WaiterList, DestroyingTheListWithLinkedNodesLeavesNoneLinked)
{
	// The nodes outlive the list here on purpose, so their final state is still
	// readable. A real waiter's node lives in its frame and is gone by then.
	waiter_node first{.handle = std::noop_coroutine()};
	waiter_node second{.handle = std::noop_coroutine()};

	{
		waiter_list list;
		ASSERT_TRUE(list.link(first));
		ASSERT_TRUE(list.link(second));
		ASSERT_EQ(list.size(), 2U);
	}

	EXPECT_EQ(first.state.load(std::memory_order_acquire), waiter_state::done);
	EXPECT_EQ(second.state.load(std::memory_order_acquire), waiter_state::done);

	// Released, not satisfied: the thing they were waiting for never happened.
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
}

// The suite's first EXPECT_DEATH cases -- tests/test_async_wait.cpp's EXPECT_EXIT is a
// death test too, and landed one commit ahead of the third case below -- and they are
// here because waiter_list is the one place in the library where the mistake they catch
// is reachable while no tool is looking at it.
//
// What they pin. Every public member of waiter_list that locks m_mutex asserts the
// list's liveness sentinel before it locks -- the destructor is the exception, being the
// member that stores the dead pattern rather than checking it -- so entering one after
// ~waiter_list has run aborts instead of locking a std::mutex whose object no longer
// exists. Three cases over two entry points:
//
// - claim and link rather than just one of them, because they are reached by different
//   parties -- a stop callback and a waiter -- so a guard on only one would leave the
//   other silent.
// - claim a second time, with the node already won by somebody else. That claim never
//   reaches the lock at all: it loses the election and returns. It aborts anyway,
//   because the assert sits ahead of the election, and the third test is what keeps it
//   there. A lost claim is the one shape of late arrival that has no other symptom,
//   which is what makes it worth a case of its own rather than a sentence.
//
// The asserts on size(), empty(), detach_all and detach_one are deliberately untested:
// they take the same mutex and so touch the same storage, and guarding only the entry
// points a test happens to reach would make the guard's coverage a function of this
// suite rather than of the hazard.
//
// Why an assert and not a tool, and the scope of that "not". Absent the sentinel, the
// hazard's only contact with the freed list is std::scoped_lock over its std::mutex,
// which is glibc's pthread_mutex_lock. Neither sanitizer instruments glibc, libstdc++'s
// std::mutex destructor is trivial so no pthread_mutex_destroy is intercepted, and
// ThreadSanitizer drops its synchronization variables on free. There was no instrumented
// access anywhere on that path, which is why nothing but a sentinel the library owns
// could report it. See WhenAllTest.AStopRacingTheEndOfAnAwaitWhoseJoinerNeverLinked,
// whose note measured exactly that blindness, and waiter_list's own class notes, which
// say what the sentinel changed: alive()'s four-byte load is instrumented, so on a
// genuinely FREED list AddressSanitizer now reports the use-after-free at that load
// rather than seeing nothing. Both statements are needed, because a reader who takes
// only the first will read a green ASan run as exoneration and a reader who takes only
// the second will expect these three tests to produce an ASan report, which they cannot.
//
// What they do NOT reach, stated rather than left for a reader to assume:
//
// - A claim running CONCURRENTLY with ~waiter_list, rather than after it. The sentinel is
//   stored before the destructor drains the list precisely so that interleaving is
//   caught, but no rendezvous a test can write pins a point inside a destructor body
//   that takes the same mutex the claim is about to take, so it is not deterministically
//   reachable from here. Uncovered, not covered.
// - Anything under NDEBUG, where assert compiles the check away and a release build
//   therefore has no guard. The sentinel member is still there -- making it conditional
//   would change the class layout between translation units built with different flags
//   -- but nothing reads it. All three death tests skip there and say so.
// - The freed-and-returned window, as opposed to the destroyed-but-not-yet-freed one.
//   destroyable_in_place::destroy() is what makes the latter reachable: it ends the list's
//   lifetime and leaves its storage intact, so the sentinel's bytes still read
//   dead_pattern. Once the storage is handed back and reused it reads neither pattern,
//   which is why waiter_list::alive() tests positively for alive_pattern; what no test
//   can do is land a call in that window on purpose. All three use stack storage,
//   so nothing here is heap-freed and AddressSanitizer has nothing to report either --
//   which is the point of the previous paragraph, not a gap in it: on the freed window
//   the tool is the instrument, and on this one the sentinel is.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, and clang-tidy-19 counts it only when some `if` precedes it in the same function. Measured with the pinned tool at threshold 0: the macro alone in a test body is 0, the macro alone in a named helper is 37, and one skip guard ahead of it is 38 -- so hoisting the macro into a helper moves the finding rather than removing it, and the guard cannot move because GTEST_SKIP only skips the function it is written in. What this body actually contains is two skip guards and one expectation.
TEST(WaiterListDeathTest, ClaimingAWaiterOnADestroyedListAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << this_test_is_single_threaded;
	}

	// The child re-executes this binary rather than forking a process that has already
	// run every test ahead of this one. GoogleTest recommends this style for a
	// multi-threaded program, and this suite is one, even though these death tests are
	// single-threaded by construction. Scoped so the flag does not outlive this test;
	// see scoped_death_test_style.
	const scoped_death_test_style style{"threadsafe"};

	destroyable_in_place<waiter_list> list;
	waiter_node                       node;

	// Holding the address past the destruction is the faithful model: a stop callback
	// holds a pointer into the block. See destroyable_in_place for why the storage is
	// not a std::optional.
	waiter_list* const storage = list.get();

	list.destroy();

	// Calling through that pointer is formally undefined, and deliberately so: it is
	// the only way to name the destroyed object's storage, which is exactly what a
	// stop callback that outlived the block it points into does.
	EXPECT_DEATH(static_cast<void>(storage->claim(node)), "claiming an alt waiter on a destroyed list");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body. See the test above, whose suppression carries the measurement.
TEST(WaiterListDeathTest, LinkingAWaiterOnADestroyedListAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << this_test_is_single_threaded;
	}

	const scoped_death_test_style style{"threadsafe"};

	destroyable_in_place<waiter_list> list;

	// Given a resumable handle so that the liveness assert is the only precondition this
	// call can trip. link's older asserts carry their own messages, and the regex below
	// is what keeps the test specific to the sentinel rather than to whichever assert
	// happens to come first.
	waiter_node node{.handle = std::noop_coroutine()};

	waiter_list* const storage = list.get();

	list.destroy();

	EXPECT_DEATH(static_cast<void>(storage->link(node)), "linking an alt waiter on a destroyed list");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, and clang-tidy-19 counts it only when some `if` precedes it in the same function. See the two tests above, whose suppressions carry the measurement.
TEST(WaiterListDeathTest, AClaimThatWouldLoseTheElectionOnADestroyedListAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << thread_sanitizer_forks_death_tests << ' ' << this_test_is_single_threaded;
	}

	const scoped_death_test_style style{"threadsafe"};

	destroyable_in_place<waiter_list> list;

	// Already won by another party, so try_claim's compare-exchange from linked fails
	// and this claim would return lost having touched nothing at all: not the mutex,
	// not the links, not this node again. The abort therefore comes from where the
	// assert sits -- ahead of the election, not after it -- rather than from anything
	// the call goes on to do, and that is what this case adds to the two above.
	// Measured: with the assert moved below try_claim, this test fails 20 of 20 in debug
	// and 20 of 20 under AddressSanitizer while the two above pass 20 of 20 in both;
	// ThreadSanitizer skips all three.
	//
	// It is the shape a late claim takes on a waiter that was resumed rather than
	// cancelled: a stop callback arriving after the satisfier has already won the node
	// reads the state word, loses, and returns. Every other symptom of a dead list is
	// downstream of the lock it then never takes, which is why a lost claim is the one
	// that would otherwise be silent. detail::wait_awaiter's linked path is where that
	// matters; see detail::when_all_awaiter's destructor.
	waiter_node node;
	node.state.store(waiter_state::done, std::memory_order_relaxed);

	waiter_list* const storage = list.get();

	list.destroy();

	EXPECT_DEATH(static_cast<void>(storage->claim(node)), "claiming an alt waiter on a destroyed list");
}

TEST(WaiterList, AConcurrentSatisfierAndStopDoNotBothResume)
{
	// A satisfier detaching the whole list while a stop callback claims nodes one at
	// a time. Every node must be resumed exactly once in total, by whichever of the
	// two won it: twice would run a destroyed frame, and never would leave a
	// coroutine suspended forever.
	//
	// This test does catch a non-atomic election, and what makes it do so is the
	// two-party rendezvous, not the order the two sides work in. Both sides block on
	// the same counter and the second arrival releases both, so their first reaches
	// for the head node's state word -- detach_all's claim of it against the stop
	// side's -- land close enough together to collide. Close enough is not together:
	// the window is one-sided, because the satisfier has to take m_mutex and read
	// m_head and node->next before its first claim while the stop side claims
	// straight away, so the two arrivals are offset by roughly a lock plus a cache
	// miss.
	//
	// Those are measurements, not arguments. With claim's compare-exchange broken
	// into a load and a store, this test detected it in 200 of 200 runs of the whole
	// test, each run being race_repeats races. Swapping the rendezvous for the
	// one-sided start flag an earlier draft used drops that to 2 detections in 100
	// runs. That comparison is what identifies the rendezvous as the load-bearing
	// part, rather than anything about which node either side reaches for first.
	//
	// A detector built on a race is never certain whatever a sample shows, so treat
	// those figures as what they are: one machine, one compiler, one sample each. A
	// single green run of this test is weak evidence on its own.
	//
	// None of which is why the test is worth keeping day to day. The standing guards
	// are the exactly-once resumption count and the m_size check at the bottom of
	// the loop, which hold whatever the interleaving turns out to be, plus
	// ThreadSanitizer running all of it. A double win surfaces through different ones
	// of those assertions from run to run -- a resumption count of five, or a node
	// left claimed and never resumed because a double unlink corrupted the links --
	// so it is the set of them that does the catching, not any one line.
	//
	// ThreadSanitizer earns its place in that set: both threads are alive for the whole
	// race and a double win races on a frame the first resumption destroyed, which is an
	// instrumented access. Do not generalize that to waiter_list's lifetime
	// preconditions. A call that reaches a destroyed list touches it through
	// nothing but the lock, which neither sanitizer instruments, so no run of this test
	// says anything about that -- see the three WaiterListDeathTest cases above and
	// waiter_list's own class notes.
	//
	// No repeat calls alt_test::reset_coroutine_test_state(): the nodes here are bare
	// waiter_nodes with no coroutine behind them, so there is none of that state to
	// clear.
	constexpr std::size_t node_count = 4;

	for(int repeat = 0; repeat < race_repeats; ++repeat)
	{
		waiter_list                         list;
		std::array<waiter_node, node_count> nodes{};
		std::atomic<std::size_t>            arrived{0};
		std::atomic<std::size_t>            resumptions{0};

		link_all(list, nodes);

		{
			std::jthread satisfier{[&] {
				rendezvous(arrived, 2);
				resumptions.fetch_add(detach_all_and_resume(list), std::memory_order_relaxed);
			}};

			std::jthread stopper{[&] {
				rendezvous(arrived, 2);
				resumptions.fetch_add(claim_and_resume_each(list, nodes), std::memory_order_relaxed);
			}};
		}

		expect_every_node_done(nodes);
		ASSERT_FALSE(HasFailure()) << "repeat " << repeat;
		ASSERT_EQ(resumptions.load(std::memory_order_relaxed), node_count) << "repeat " << repeat;
		EXPECT_EQ(list.size(), 0U);
	}
}

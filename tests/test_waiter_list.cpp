#include <gtest/gtest.h>

#include <alt/detail/waiter_list.hpp>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"

namespace
{
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

	/** Spins before a waiting party starts yielding. See rendezvous below. */
	constexpr int spin_limit = 100000;

	/**
	 * @brief Waits until @c released parties have arrived, spinning.
	 *
	 * A two-party rendezvous that leaves both sides running within nanoseconds of
	 * each other, which is what makes the claim election genuinely contended. A
	 * condition variable or a sleep would hand the race to whichever side woke
	 * first and the window under test would never be hit. The yield is only a
	 * backstop for a machine with fewer cores than threads, where an unbounded spin
	 * could starve the party being waited for.
	 */
	void rendezvous(std::atomic<std::size_t>& arrived, std::size_t released)
	{
		arrived.fetch_add(1, std::memory_order_acq_rel);

		for(int spins = 0; arrived.load(std::memory_order_acquire) < released;)
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
		list.link(*node);
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
			list.link(node);
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

			if(list.claim(nodes[index]))
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
			if(list.claim(node))
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

	list.link(first);
	list.link(second);
	list.link(third);
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
	list.link(node);

	waiter_node* const chain = list.detach_all();
	ASSERT_EQ(chain, &node);
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::claimed);

	// The satisfier already owns this resumption, so a late claim must lose.
	EXPECT_FALSE(list.claim(node));
	EXPECT_EQ(node.state.load(std::memory_order_acquire), waiter_state::claimed);
}

TEST(WaiterList, ClaimUnlinksSoSizeDrops)
{
	waiter_node first{.handle = std::noop_coroutine()};
	waiter_node second{.handle = std::noop_coroutine()};

	waiter_list list;
	list.link(first);
	list.link(second);

	ASSERT_TRUE(list.claim(first));
	EXPECT_EQ(list.size(), 1U);

	// The winner's node comes back as a chain of one, ready for resume_claimed.
	EXPECT_EQ(first.prev, nullptr);
	EXPECT_EQ(first.next, nullptr);

	// A second claim of the same node loses: the first caller owns the resumption.
	EXPECT_FALSE(list.claim(first));
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
	list.link(taken);
	list.link(available);

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

TEST(WaiterList, ResumeClaimedWritesSatisfiedAndMarksDone)
{
	waiter_node satisfied_node{.handle = std::noop_coroutine()};
	waiter_node unsatisfied_node{.handle = std::noop_coroutine()};

	waiter_list list;
	list.link(satisfied_node);
	list.link(unsatisfied_node);

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
	list->link(*destroyer);

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
		list.link(first);
		list.link(second);
		ASSERT_EQ(list.size(), 2U);
	}

	EXPECT_EQ(first.state.load(std::memory_order_acquire), waiter_state::done);
	EXPECT_EQ(second.state.load(std::memory_order_acquire), waiter_state::done);

	// Released, not satisfied: the thing they were waiting for never happened.
	EXPECT_FALSE(first.satisfied);
	EXPECT_FALSE(second.satisfied);
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

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace alt::detail
{

	/**
	 * @brief Which party, if any, owns a waiter's resumption.
	 *
	 * The three states are visited in one direction only: linked -> claimed ->
	 * done. There is no way back, which is what makes a single compare-exchange
	 * enough to elect one resumer for the lifetime of a waiter.
	 */
	enum class waiter_state : std::uint8_t
	{
		/** On a waiter_list and unclaimed. Anyone may still win this node. */
		linked,

		/** Won by exactly one party, which now owns the resumption. */
		claimed,

		/** The owner has written the result; the handle has been resumed, or is about to be. */
		done
	};

	/**
	 * @brief What a claim attempt achieved.
	 *
	 * The election has two winning outcomes rather than one, because a node is
	 * winnable from the moment it exists and that is before its waiter has linked
	 * it. Winning therefore says who owns the resumption; whether the node was
	 * actually parked says whether there is a resumption to perform.
	 */
	enum class claim_result : std::uint8_t
	{
		/** Another party already owns this resumption. The node must not be touched again. */
		lost,

		/** Won, and the node came off the list. The caller must resume it, exactly once. */
		detached,

		/**
		 * Won, but the node was on no list, so nobody is parked on it. Resuming it
		 * would run a coroutine still inside its own await_suspend, so the caller
		 * resumes nothing and touches nothing: the waiter's own frame will see its
		 * link refused and resume itself. See waiter_list::link.
		 */
		not_linked
	};

	/**
	 * @brief One suspended waiter's place in a waiter_list.
	 *
	 * A node lives inside an awaiter, which lives inside a coroutine frame, so the
	 * node's storage disappears when its handle is resumed and the coroutine
	 * finishes. Nothing here is reference counted and nothing is heap allocated:
	 * the waiter owns its own node and the list only borrows it.
	 *
	 * The links, @c on_list and the handle are written by the waiting coroutine
	 * before it is linked, and afterwards only under the list's mutex. Two fields
	 * are touched without it: @c state, because the claim protocol has to work from a stop
	 * callback that cannot take the mutex, and @c satisfied, which only the party
	 * that won the claim writes, at a point where the node is already off the list
	 * and reachable from nothing else.
	 */
	struct waiter_node
	{
		/** Previous node in link order, or null at the head. Guarded by the list's mutex. */
		waiter_node* prev{nullptr};

		/** Next node in link order, or null at the tail. Also the link of a detached chain. */
		waiter_node* next{nullptr};

		/**
		 * Whether this node is currently on a list. Guarded by the list's mutex, like
		 * the links themselves, and not derivable from them: the sole node of a list
		 * has null links too. This is the one thing that tells a winning claimer
		 * whether anybody is actually parked here, which is why waiter_list::claim
		 * can report a win that detached nothing.
		 */
		bool on_list{false};

		/**
		 * The waiter to resume. Default constructed empty, and must hold a resumable
		 * handle before the node is linked, which waiter_list::link asserts. An empty
		 * handle is perfectly detectable; what nothing downstream can do is supply the
		 * missing one, which is why the check belongs at the link and not at the
		 * resume.
		 */
		std::coroutine_handle<> handle;

		/** Who owns this resumption. The one field that changes without the mutex. */
		std::atomic<waiter_state> state{waiter_state::linked};

		/**
		 * Whether the wait ended because the primitive was satisfied, as opposed to
		 * cancelled or abandoned. Written by the owner of the resumption just before
		 * the handle is resumed, and read by the waiter once it runs again.
		 */
		bool satisfied{false};
	};

	/**
	 * @brief Resumes every waiter in a detached chain, taking no lock.
	 *
	 * DELICATE. Read this before changing it.
	 *
	 * The chain comes from waiter_list::detach_all, waiter_list::detach_one, or a
	 * winning waiter_list::claim. Every node in it is already @c claimed and
	 * already off the list, so this function is the sole owner of each node it
	 * walks and needs no mutual exclusion to walk them.
	 *
	 * Holding no lock is the point, not an optimization:
	 *
	 * - A resumed waiter may re-await the same primitive on this thread, or destroy
	 *   it outright. Either would deadlock or reenter if the list's mutex were held
	 *   across the resume, and a node still on the list could be resumed twice.
	 * - Nodes in flight here are reachable only from this walk. Anything that locks
	 *   the list cannot see them, which is exactly what makes the window between
	 *   detaching and resuming safe.
	 *
	 * Every read of a node happens before that node's handle is resumed, and the
	 * next node and the handle are copied into locals first. This is not style:
	 * resuming a waiter runs its body to completion, which destroys the frame the
	 * node lives in, so @c node is dangling the instant @c resume() returns.
	 * Reading @c node->next after the resume is a use-after-free that ends the walk
	 * on garbage. The store of @c done is likewise the point at which the node is
	 * published as finished to anyone still watching it, so no field of the node is
	 * read after it.
	 *
	 * The copy only protects the node being resumed. The rule the rest of the chain
	 * needs is the caller's to keep: no resumption may destroy a *later* node in the
	 * same chain. @c following is a raw pointer into the next waiter's frame, held
	 * live across the resume and dereferenced after it, so a waiter whose
	 * continuation frees another waiter still queued behind it in this chain leaves
	 * this walk reading freed storage. The shape to watch for is a resumed
	 * continuation that destroys something owning another waiter's frame, such as a
	 * task holding a sibling that is waiting on the same primitive; a primitive that
	 * can do that must resume its waiters one detach at a time rather than from one
	 * multi-node chain. A chain of one, from waiter_list::claim or
	 * waiter_list::detach_one, cannot hit this at all.
	 *
	 * Marked noexcept deliberately. A waiter's resumption throwing past this walk
	 * would abandon the rest of the chain suspended forever, so terminating is
	 * preferable to leaking the remaining waiters.
	 *
	 * Every node in the chain must carry a resumable handle, because resuming an
	 * empty one is undefined behavior. waiter_list::link asserts that, which puts
	 * the check at the point where the mistake is made rather than here, where the
	 * node has already been detached and its owner has nothing left to do but
	 * resume it.
	 *
	 * @param chain     The first node of a chain linked through @c next, or null.
	 * @param satisfied What to record in each node before resuming it.
	 */
	inline void resume_claimed(waiter_node* chain, bool satisfied) noexcept
	{
		waiter_node* node = chain;

		while(node != nullptr)
		{
			// Both copies happen before anything is published or resumed. After the
			// resume there is no node left to read them from.
			waiter_node* const            following = node->next;
			const std::coroutine_handle<> handle    = node->handle;

			node->satisfied = satisfied;
			node->state.store(waiter_state::done, std::memory_order_release);

			handle.resume();

			node = following;
		}
	}

	/**
	 * @brief The waiting queue every alt async primitive is built on.
	 *
	 * Holds the links, the mutex and the claim protocol, so the protocol exists
	 * once rather than once per primitive. alt::async_event, alt::async_latch,
	 * alt::async_semaphore and alt::async_mutex differ in when they satisfy their
	 * waiters, not in how they resume them.
	 *
	 * The mutex guards the links and the size, and nothing else. In particular it
	 * is never held across a resumption, because a resumed waiter may re-await or
	 * destroy the primitive on the resuming thread.
	 *
	 * The claim protocol is the correctness argument:
	 *
	 * - Every node starts @c linked. Exactly one party moves it to @c claimed, with
	 *   one compare_exchange_strong, and that party owns the resumption. A satisfier
	 *   taking the whole list and a cancellation callback taking one node are
	 *   therefore mutually exclusive rather than racing: one of them loses and must
	 *   not touch the node again.
	 * - A claimed node is off the list before its handle is resumed, so a waiter
	 *   that re-awaits the same primitive while it runs cannot find its old node.
	 * - Between being detached and being resumed a node is reachable only from the
	 *   owner's local chain. That window is invisible to this list, and resuming
	 *   there is what resume_claimed is for.
	 * - A node is winnable before its waiter has linked it, so winning the election
	 *   and having a waiter to resume are two different things. claim() reports them
	 *   separately and link() refuses a node whose election is already lost, which
	 *   between them give exactly one resumption on either side of that window: the
	 *   winner resumes nothing it did not unlink, and the waiter whose link was
	 *   refused resumes itself.
	 *
	 * Two preconditions the list cannot enforce and the owner must:
	 *
	 * - A node's handle must be resumable before the node is linked, and the node's
	 *   storage must outlive every claim attempt on it. A losing claimer still reads
	 *   the node's state word, so the waiter's frame may not be destroyed while a
	 *   claim on it is in flight. For a cancellation callback that guarantee comes
	 *   from std::stop_callback's destructor, which waits for a callback already
	 *   running.
	 * - No claim may be in flight when the list is destroyed. A claim that has won
	 *   the election but not yet taken the mutex leaves its node linked for that
	 *   moment, and a list destroyed in that window would race the unlink.
	 */
	class waiter_list
	{
	public:
		/** Constructs an empty list with no waiters. */
		waiter_list() = default;

		/**
		 * @brief Releases every waiter still linked, unsatisfied.
		 *
		 * A backstop rather than the expected path: a primitive normally resumes its
		 * waiters before it dies. Releasing them beats leaking them, because a waiter
		 * nobody resumes stays suspended forever and its frame is never freed.
		 *
		 * The waiters are detached first and resumed after the mutex is released, so
		 * a resumption may not touch this list: it is being destroyed. Each waiter
		 * sees @c satisfied false, which is how it tells this apart from the
		 * primitive actually completing.
		 *
		 * This is also the one multi-node resumption the list performs on its own
		 * behalf, so resume_claimed's chain rule binds here as much as anywhere: a
		 * waiter released by this destructor may not destroy another waiter still
		 * queued behind it in the same chain. A primitive whose waiters can destroy
		 * one another must therefore drain its list with detach_one before it reaches
		 * this destructor, rather than leaving the release to the backstop.
		 */
		~waiter_list()
		{
			resume_claimed(detach_all(), false);
		}

		/**
		 * Neither copyable nor movable: it owns a mutex, and the linked nodes hold
		 * pointers back into it that no copy or move could fix up.
		 */
		waiter_list(const waiter_list&)            = delete;
		waiter_list& operator=(const waiter_list&) = delete;
		waiter_list(waiter_list&&)                 = delete;
		waiter_list& operator=(waiter_list&&)      = delete;

		/**
		 * @brief Adds a waiter to the back of the list, if it is still waiting at all.
		 *
		 * Link order is resume order: detach_all returns the nodes oldest first and
		 * detach_one takes the oldest claimable node, so the primitives above this one
		 * are fair without doing anything to be fair.
		 *
		 * The node must not already be on a list and must carry a resumable handle.
		 * The handle is asserted, because a node is winnable the instant it is linked
		 * and from then on nobody downstream can tell an empty handle from a resumable
		 * one.
		 *
		 * Two conditions are then checked together under the mutex, and the node is
		 * inserted only if both hold:
		 *
		 * - The node is still @c linked. A node already won is refused, because its
		 *   winner saw claim_result::not_linked and resumed nothing: parking it now
		 *   would suspend a waiter with no party left to wake it.
		 * - @c still_waiting returns true. This is where a primitive's own state joins
		 *   the decision. A waiter checks whether the primitive is satisfied before it
		 *   suspends, and a satisfier that lands between that check and this call has
		 *   already walked the list and found nothing. Asking again here, under the
		 *   same mutex the satisfier's detach takes, is what makes those two steps one
		 *   decision instead of a lost wakeup.
		 *
		 * @param still_waiting Evaluated with the list's mutex held, so it must not
		 *        take a lock of its own, touch this list, or resume anything. Reading
		 *        the primitive's own atomic flag is the intended shape.
		 *
		 * @return True when the node is on the list, and from that instant any party
		 *         may win it, so the caller must read nothing from the waiter's frame
		 *         afterwards. False when nothing was inserted; the two causes are told
		 *         apart by the node's @c state, which the caller may safely read
		 *         because a node on no list has no resumer but its own frame: still
		 *         @c linked means @c still_waiting refused, and anything else means the
		 *         election was lost.
		 */
		template<typename Condition>
		[[nodiscard]] bool link(waiter_node& node, Condition still_waiting)
		{
			// The cheapest place to catch the most likely mistake above this header:
			// linking a waiter before its handle has been stored. Checked here rather
			// than in resume_claimed, where the node is already detached and in flight
			// and whoever owns it has nothing left to do but resume it.
			assert(node.handle && "linking an alt waiter with no handle to resume");
			assert(!node.on_list && "linking an alt waiter that is already on a list");

			const std::scoped_lock lock{m_mutex};

			// The election first, so a node already won is refused whatever the
			// condition would have said. A lost election means some other party owns
			// this waiter's resumption, and that outranks the primitive's state.
			if(node.state.load(std::memory_order_acquire) != waiter_state::linked)
			{
				return false;
			}

			if(!still_waiting())
			{
				return false;
			}

			node.prev    = m_tail;
			node.next    = nullptr;
			node.on_list = true;

			if(m_tail != nullptr)
			{
				m_tail->next = &node;
			}
			else
			{
				m_head = &node;
			}

			m_tail = &node;
			++m_size;
			return true;
		}

		/** Links a waiter whose primitive has no state of its own to consult. */
		[[nodiscard]] bool link(waiter_node& node)
		{
			return link(node, [] { return true; });
		}

		/**
		 * @brief Tries to win the right to resume one node.
		 *
		 * This is the single election. One compare_exchange_strong from @c linked to
		 * @c claimed decides it, so a satisfier and a cancellation callback reaching
		 * for the same waiter cannot both win. A load followed by a store would let
		 * both see @c linked and resume the same frame twice.
		 *
		 * Only the winner takes the mutex, which is what lets this be called from a
		 * cancellation callback that must not block on the primitive's lock in the
		 * common losing case.
		 *
		 * Winning is not the same as having someone to resume. A node is winnable
		 * before its waiter has linked it, and a winner that finds the node on no
		 * list must resume nothing: the waiter is still inside its own await_suspend,
		 * its link will be refused, and it resumes itself. The membership check is
		 * made under the mutex, because that is the only place it is authoritative.
		 *
		 * @return claim_result::detached when this caller won and the node came off
		 *         the list, with both links cleared, so it can be passed straight to
		 *         resume_claimed as a chain of one. claim_result::not_linked when this
		 *         caller won a node that was on no list: nothing to resume, and
		 *         nothing in the node may be written, because its waiter reads its own
		 *         fields on its own thread. claim_result::lost when another party
		 *         already owns it, and then the node must not be touched again.
		 */
		claim_result claim(waiter_node& node)
		{
			if(!try_claim(node))
			{
				return claim_result::lost;
			}

			const std::scoped_lock lock{m_mutex};

			if(!node.on_list)
			{
				return claim_result::not_linked;
			}

			unlink_locked(node);
			return claim_result::detached;
		}

		/**
		 * @brief Claims and detaches every node this caller can win.
		 *
		 * What a satisfier that releases everyone calls: an event being set, a latch
		 * reaching zero, a primitive being destroyed.
		 *
		 * A node another party has already claimed but not yet unlinked is left where
		 * it is, because that party owns it and will unlink it when it takes the
		 * mutex. So this does not always leave the list empty, and size() may still be
		 * non-zero on return.
		 *
		 * @return The oldest node won, linked to the rest through @c next, or null.
		 *         Every node in the chain is @c claimed and off the list. Passing a
		 *         multi-node chain to resume_claimed carries that function's rule with
		 *         it: no waiter's resumption may destroy a later node in the chain. A
		 *         primitive whose waiters can destroy one another must release them
		 *         with detach_one instead, one chain of one at a time. That works for a
		 *         reason worth making explicit: a node still on the list can be
		 *         withdrawn by its own owner's claim, so a waiter about to be destroyed
		 *         can still be taken off the list first, while a node already sitting
		 *         in a chain is beyond rescue: nothing can take it back out before the
		 *         walk reaches it.
		 */
		waiter_node* detach_all() noexcept
		{
			const std::scoped_lock lock{m_mutex};

			waiter_node* chain_head = nullptr;
			waiter_node* chain_tail = nullptr;
			waiter_node* node       = m_head;

			while(node != nullptr)
			{
				// Read the successor before unlinking: unlinking clears the node's links.
				waiter_node* const following = node->next;

				if(try_claim(*node))
				{
					unlink_locked(*node);

					// Linked forwards only. The chain's nodes die one at a time as they are
					// resumed, so a prev pointer here would be a pointer to freed storage.
					if(chain_tail != nullptr)
					{
						chain_tail->next = node;
					}
					else
					{
						chain_head = node;
					}

					chain_tail = node;
				}

				node = following;
			}

			return chain_head;
		}

		/**
		 * @brief Claims and detaches the oldest node this caller can win.
		 *
		 * What a satisfier that releases one waiter calls: a semaphore being released,
		 * a mutex being unlocked. Nodes another party has already claimed are skipped
		 * rather than stopping the search, so a waiter being cancelled does not hide
		 * the waiters behind it.
		 *
		 * @return A chain of exactly one @c claimed, unlinked node, or null when the
		 *         list holds nothing this caller can win.
		 */
		waiter_node* detach_one() noexcept
		{
			const std::scoped_lock lock{m_mutex};

			for(waiter_node* node = m_head; node != nullptr; node = node->next)
			{
				if(try_claim(*node))
				{
					unlink_locked(*node);
					return node;
				}
			}

			return nullptr;
		}

		/**
		 * How many nodes are linked. This counts a node that is already claimed but
		 * whose winner has not yet reached the mutex, so it is an upper bound on how
		 * many waiters are still winnable, not an exact count of them.
		 */
		[[nodiscard]] std::size_t size() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_size;
		}

		/**
		 * True when no node is linked. This carries the same caveat as size(): a node
		 * whose claim another party has won but not yet unlinked still counts as
		 * linked, so a false result does not promise that detach_one() will return
		 * anything.
		 *
		 * A caller releasing one waiter must therefore act on what detach_one()
		 * returns, not on this. Testing this first and assuming a waiter will be there
		 * drops the release on exactly that interleaving, which for a semaphore means
		 * a permit that is never handed to anyone.
		 */
		[[nodiscard]] bool empty() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_size == 0;
		}

	private:
		/**
		 * The election itself, in one place so there is exactly one copy of it.
		 *
		 * Acquire-release, so the claim orders what happens on either side of it: a
		 * winner's later writes to the node cannot be hoisted above it, and a party
		 * that observes someone else's claim also observes whatever that claimer
		 * published before claiming.
		 *
		 * What this word does not do is hand over the node's contents. A waiter's
		 * handle and links are written before link() takes m_mutex, and every reader
		 * of them reaches them through that same mutex: claim()'s winner locks before
		 * it unlinks, and a detached chain was assembled under the lock before anyone
		 * walked it. The handle handoff is therefore synchronized by m_mutex, not by
		 * this CAS, so winning the election alone does not entitle a caller to read
		 * the node's handle.
		 */
		static bool try_claim(waiter_node& node) noexcept
		{
			waiter_state expected = waiter_state::linked;
			return node.state.compare_exchange_strong(expected, waiter_state::claimed, std::memory_order_acq_rel, std::memory_order_acquire);
		}

		/** Removes a linked node and clears its links. The mutex must be held. */
		void unlink_locked(waiter_node& node) noexcept
		{
			if(node.prev != nullptr)
			{
				node.prev->next = node.next;
			}
			else
			{
				m_head = node.next;
			}

			if(node.next != nullptr)
			{
				node.next->prev = node.prev;
			}
			else
			{
				m_tail = node.prev;
			}

			node.prev    = nullptr;
			node.next    = nullptr;
			node.on_list = false;
			--m_size;
		}

		mutable std::mutex m_mutex;
		waiter_node*       m_head = nullptr;
		waiter_node*       m_tail = nullptr;
		std::size_t        m_size = 0;
	};

}

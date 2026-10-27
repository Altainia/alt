#include <alt/async_semaphore.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>

namespace alt
{

	async_semaphore::async_semaphore(std::ptrdiff_t initial): m_permits(initial)
	{
		assert(initial >= 0 && "constructing an alt::async_semaphore with a negative permit count");
	}

	async_semaphore::~async_semaphore()
	{
		// Released here rather than left to m_waiters' own destructor, for the reason
		// async_event's destructor gives: a released waiter may await this semaphore
		// again, and that re-await must land in a list that has not finished its walk.
		//
		// Like release(), this resumes a multi-node chain, so resume_claimed's chain rule
		// binds here: a waiter released by this destructor may not destroy another waiter
		// still queued behind it. That is sound for the same reason it is sound in
		// async_latch, whose count_down records the argument and the condition under
		// which it must be revisited.
		detail::resume_claimed(m_waiters.detach_all(), false);
	}

	void async_semaphore::release(std::ptrdiff_t n)
	{
		assert(n >= 0 && "releasing a negative number of permits to an alt::async_semaphore");

		// DELICATE. Read this before changing it.
		//
		// Where each permit goes is decided inside detach_one's critical section, and the
		// deposit of whatever is left over is made there too. That is the whole reason
		// this is a loop of detach_one(action) rather than a detach followed by an
		// increment: a waiter reaches this queue only through link(), which takes the
		// same mutex, so there is no instant at which the count has been raised and the
		// queue has already been walked past. Split the two -- detach, then raise the
		// count outside the section -- and a waiter whose link lands in the gap reads the
		// count as zero under that mutex, parks, and stays there with its permit sitting
		// in the count. waiter_list::detach_one documents that lost wakeup with this
		// function as its worked example.
		//
		// Every waiter won is strung onto a local chain and the whole chain is resumed
		// once, after the loop, rather than one waiter per iteration. That is what makes
		// the resumption the last statement of this function, as it is in
		// async_event::set and async_latch::count_down, and it is the only reason a
		// waiter resumed from here may destroy the semaphore: resuming inside the loop
		// would bring control back to read m_waiters afterwards. Nothing about the
		// decision above changes, because the chain is assembled from nodes that are
		// already claimed and already off the list -- unlink_locked clears both their
		// links -- so holding them across the next detach_one is the window
		// resume_claimed documents as safe.
		//
		// The resumption stays outside the mutex, because a resumed waiter runs its body:
		// it may re-await this semaphore, take whatever lock it likes, or destroy it.
		// Resuming a multi-node chain does carry resume_claimed's chain rule, so a waiter
		// resumed here may not destroy another waiter of this semaphore -- which the class
		// documentation already forbids for the reason the list gives, that nothing
		// withdraws a parked node from the queue.
		//
		// What this costs, and it is a real cost: a waiter resumed by one of these
		// permits is resumed after the last of them has been placed, so it can no longer
		// be handed the next one. It re-awaits into a count that already holds the
		// leftovers, where an unrelated try_acquire could take one first. A permit's
		// route is not observable from outside the semaphore, so no caller can tell; what
		// a caller could tell is a destroyed semaphore.
		//
		// The waiter the permit goes to is deliberately not handed to the action:
		// resuming it under the mutex is the one thing that must not happen there.
		detail::waiter_node* chain_head = nullptr;
		detail::waiter_node* chain_tail = nullptr;
		std::ptrdiff_t       remaining  = n;

		while(remaining > 0)
		{
			detail::waiter_node* const won = m_waiters.detach_one([this, &remaining](const bool detached) noexcept {
				if(detached)
				{
					// Handed straight to that waiter, so this permit never enters the count and
					// nothing else can take it on the way.
					--remaining;
					return;
				}

				// No waiter this caller can win, so every permit still in hand belongs in the
				// count. Nodes another party has already claimed are the ones that do not
				// count as winnable, and their waiters are on their way out unsatisfied.
				m_permits.fetch_add(remaining, std::memory_order_release);
				remaining = 0;
			});

			if(won == nullptr)
			{
				break;
			}

			// Linked forwards only, like detach_all's own walk: the chain's nodes die one
			// at a time as they are resumed, so a prev pointer here would point at freed
			// storage.
			if(chain_tail != nullptr)
			{
				chain_tail->next = won;
			}
			else
			{
				chain_head = won;
			}

			chain_tail = won;
		}

		detail::resume_claimed(chain_head, true);
	}

	bool async_semaphore::try_acquire() noexcept
	{
		// A compare-exchange loop rather than a load and a store, because several callers
		// may reach for the same permit at once and only one may have it. Lowered only
		// from a value seen above zero, so the count never goes negative and a caller
		// that gets nothing has taken nothing.
		std::ptrdiff_t current = m_permits.load(std::memory_order_acquire);

		while(current > 0)
		{
			// On failure current is reloaded, and the loop re-decides against it.
			if(m_permits.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return true;
			}
		}

		return false;
	}

	std::ptrdiff_t async_semaphore::available() const noexcept
	{
		return m_permits.load(std::memory_order_acquire);
	}

}

#include <algorithm>
#include <alt/async_latch.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>

namespace alt
{

	async_latch::async_latch(std::ptrdiff_t count): m_count(count)
	{
		assert(count >= 0 && "constructing an alt::async_latch with a negative count");
	}

	async_latch::~async_latch()
	{
		// Released here rather than left to m_waiters' own destructor, for the reason
		// async_event's destructor gives: a released waiter may await this latch again,
		// and that re-await has to land in a list that has not finished its walk.
		//
		// Defensive rather than promised, exactly as it is there. Awaiting a primitive
		// from inside a waiter its destructor released is unsupported on all four of
		// them, so this call buys that the unsupported case leaks nothing rather than
		// that any depth of it works.
		detail::resume_claimed(m_waiters.detach_all(), false);
	}

	void async_latch::count_down(std::ptrdiff_t n)
	{
		assert(n >= 0 && "counting an alt::async_latch down by a negative amount");

		// DELICATE. Read this before changing it.
		//
		// Publishing zero and electing the walker are the same operation: the
		// compare-exchange below. The detach_all call is control-dependent on its
		// result, so there is no pair of statements whose order has to be kept.
		//
		// What would reintroduce the question is splitting that compare-exchange into
		// a load, a decision and a store. ConcurrentCountDownsReleaseTheWaiterExactlyOnce
		// rejects that split; do not make it without that test still passing.
		//
		// A waiter decides whether to park by reading m_count as the condition on its
		// link, under the list's mutex, and detach_all takes that same mutex. So a
		// waiter either links before the walk and is found by it, or links after the
		// walk and, the zero having been published before the walk began, sees zero and
		// does not park. Publish after the walk, or between the walk and the
		// resumptions, and a waiter arriving in the gap sees a nonzero count, parks on
		// a queue nobody will walk again, and waits forever. See waiter_list::detach_all.
		//
		// The action overload of detach_all cannot serve here: it always walks, and
		// this call must release nobody until the count reaches zero.
		//
		// The compare-exchange is also what elects the releaser. Of any number of
		// concurrent calls, exactly one steps the count from nonzero to zero and so
		// exactly one goes on to the walk; the rest see zero, or a count that is still
		// above it, and return.
		std::ptrdiff_t current = m_count.load(std::memory_order_acquire);

		while(current != 0 && n != 0)
		{
			const std::ptrdiff_t lowered = std::max<std::ptrdiff_t>(current - n, 0);

			// On failure current is reloaded, and the loop re-decides against it.
			if(!m_count.compare_exchange_weak(current, lowered, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				continue;
			}

			if(lowered == 0)
			{
				// Outside the mutex, and last, because resuming a waiter runs its body: it
				// may re-await this latch, destroy it outright, or take whatever lock it
				// likes. Nothing may be read from *this after this call, for the same
				// reason.
				detail::resume_claimed(m_waiters.detach_all(), true);
			}

			return;
		}
	}

	bool async_latch::try_wait() const noexcept
	{
		return m_count.load(std::memory_order_acquire) == 0;
	}

}

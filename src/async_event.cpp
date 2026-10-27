#include <alt/async_event.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>

namespace alt
{

	async_event::async_event(bool set) noexcept: m_set(set)
	{}

	async_event::~async_event()
	{
		// Released here rather than left to m_waiters' own destructor, which would do
		// the same thing one step later, because one step later is too late for a
		// waiter that awaits this event again from inside the walk. Its continuation
		// may perfectly well do that: the event still exists while this body runs, so
		// the re-await parks in a list that is still whole, and the member's destructor
		// then releases that round. So the list's destructor may well have waiters
		// left; AWaiterThatReAwaitsWhileTheEventIsBeingDestroyedIsStillReleased is the
		// case where it has exactly one. Leave the release to the member instead and
		// that re-await lands in a list whose walk has already finished, parked on an
		// event that will never walk again.
		detail::resume_claimed(m_waiters.detach_all(), false);
	}

	void async_event::set()
	{
		// DELICATE. Read this before changing it.
		//
		// Two statements, and only the boundary between them is load-bearing.
		//
		// Marking the event set and taking its waiting queue are one critical section,
		// not two steps whose order has to be argued about. A waiter decides whether to
		// park by reading this same flag as the condition on its link, under this same
		// mutex, so it either links before this section and is detached by it, or links
		// after it and finds the event already set and does not park at all. There is
		// no third interleaving, and that is the point: an earlier version of this
		// function published the flag and then detached, which was correct only because
		// the two statements were in that order. They are no longer in any order.
		detail::waiter_node* const chain = m_waiters.detach_all([this] { m_set.store(true, std::memory_order_release); });

		// The resumption is outside the mutex, and last, because resuming a waiter runs
		// its body: it may re-await this event, destroy it outright, or take whatever
		// lock it likes. Nothing may be read from *this after this call, for the same
		// reason.
		detail::resume_claimed(chain, true);
	}

	void async_event::reset() noexcept
	{
		m_set.store(false, std::memory_order_release);
	}

	bool async_event::is_set() const noexcept
	{
		return m_set.load(std::memory_order_acquire);
	}

}

#include <alt/async_event.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>

namespace alt
{

	async_event::async_event(bool set) noexcept: m_set(set)
	{}

	async_event::~async_event()
	{
		// Released explicitly rather than left to m_waiters' own destructor, so that
		// the release happens here, where the released-not-satisfied contract is
		// documented, and cannot be lost to a later change in the member order or in
		// waiter_list's backstop. The list's destructor then finds nothing to do.
		detail::resume_claimed(m_waiters.detach_all(), false);
	}

	void async_event::set()
	{
		// DELICATE. Read this before changing it.
		//
		// Three statements, and the order of all three is load-bearing.
		//
		// The store comes first, because a waiter that arrives from here on must not
		// park: the detach below has already walked the list and would not see it.
		// What makes that watertight is where the waiter reads this flag a second
		// time, as the condition on its link, under the same mutex detach_all takes.
		// Either the waiter's link runs first, and this detach finds its node, or
		// this detach runs first, and the waiter's link sees the flag set and refuses
		// to park. Store after detach instead and that reasoning collapses: a waiter
		// could read the flag unset under the mutex, link, and never be resumed.
		m_set.store(true, std::memory_order_release);

		detail::waiter_node* const chain = m_waiters.detach_all();

		// Last, and outside the list's mutex, because resuming a waiter runs its body:
		// it may re-await this event, destroy it outright, or take whatever lock it
		// likes. Nothing may be read from *this after this call for the same reason.
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

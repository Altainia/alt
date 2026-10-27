#include <alt/async_mutex.hpp>
#include <alt/detail/waiter_list.hpp>
#include <atomic>
#include <expected>
#include <system_error>
#include <utility>

namespace alt
{

	namespace
	{
		/**
		 * @brief Which mutex this thread is already draining a hand-off chain for, and
		 *        whether that chain's lock has come back to it.
		 *
		 * Per-thread rather than per-mutex, exactly as detail::trampoline_state is, and
		 * for the same reason: the thing being bounded is one thread's stack. A mutex is
		 * identified by address only while a drain for it is live on this thread's
		 * stack, and async_mutex forbids a resumed waiter from destroying this mutex or
		 * any mutex whose lock is in transit on this thread -- the saved outer_mutex of
		 * a nested drain being the second kind -- so the pointer can never name a
		 * different object than the one it was stored for. It is only ever compared
		 * against, never dereferenced, which is the belt to that braces.
		 */
		struct handoff_state
		{
			/** The mutex whose chain this thread is draining, or null. */
			async_mutex* draining = nullptr;

			/** Whether the drained mutex's lock was released again on this thread. */
			bool handed_back = false;
		};

		handoff_state& this_thread_handoff() noexcept
		{
			static thread_local handoff_state state;
			return state;
		}
	}

	async_mutex_lock::~async_mutex_lock()
	{
		unlock();
	}

	async_mutex_lock& async_mutex_lock::operator=(async_mutex_lock&& other) noexcept
	{
		if(this == &other)
		{
			return *this;
		}

		// This guard's own lock goes first, because overwriting the pointer without
		// releasing it would leave that mutex locked with nothing left that could ever
		// release it. The release hands the lock to that mutex's next waiter and runs
		// that waiter's body on this thread, which is a lot to happen inside an
		// assignment and is why async_mutex_lock::unlock is where it is all documented.
		unlock();

		m_mutex = std::exchange(other.m_mutex, nullptr);
		return *this;
	}

	void async_mutex_lock::unlock() noexcept
	{
		// Idempotent, and the clearing happens before the release: the release resumes a
		// waiter whose body may reach this guard again, and must find it owning nothing.
		async_mutex* const mutex = std::exchange(m_mutex, nullptr);

		if(mutex != nullptr)
		{
			mutex->unlock();
		}
	}

	async_mutex::~async_mutex()
	{
		// Released here rather than left to m_waiters' own destructor, as in
		// async_event, async_latch and async_semaphore, whose reason is that a released
		// waiter may await the primitive again and that re-await has to land in a list
		// that has not finished its walk.
		//
		// That re-await is *chosen* to be unsupported -- here and on the other three,
		// where the explicit release is likewise a defense against the unsupported case
		// leaking rather than a promise that it works -- and the distinction matters
		// because nothing forces it. This destructor is one detach_all and one
		// resume_claimed: it never calls unlock(), so no waiter it releases builds a
		// guard and none of them re-enters the hand-off loop. The iterative drain has
		// no say here at all.
		//
		// What it would take to support it is the one durable objection, and it is not
		// what an earlier version of this comment said. A re-awaiting waiter would not
		// find the lock free and would not find a half-destroyed mutex: m_held is true
		// at this point, because waiters are only parked here if somebody took the
		// lock, and inside its own destructor body every member of this object is still
		// alive and member calls on it are perfectly well defined. So the ordinary case
		// is benign -- the re-await parks in a list that is still whole and ~waiter_list
		// releases it a second time, which is exactly the shape the other three fall
		// into and likewise do not promise.
		//
		// The objection is what the shape rests on. It is benign only while this mutex
		// is destroyed *locked*, and a mutex destroyed unlocked with waiters still
		// parked is not reachable through the public interface today but is not ruled
		// out by anything in the type either. There, the re-await is handed the lock
		// inside this destructor body, and nothing stops that guard being moved out to
		// outlive the body: a guard that outlives its mutex has no mutex to hand the
		// lock back to. Supporting re-await would mean promising that cannot happen, and
		// this class has nothing to promise it with. So the rule stays: a waiter
		// released here must not come back.
		//
		// Which is also why the explicit release stays rather than being left to
		// ~waiter_list. It is equivalent to that backstop only while the rule above
		// holds; the moment the choice is revisited, this call is what keeps a re-await
		// landing in a live list, and the equivalence goes away with it. Do not read
		// "removing it changes no test" as "removing it is free".
		//
		// This resumes a multi-node chain, so resume_claimed's chain rule binds here: a
		// waiter released by this destructor may not destroy another waiter still queued
		// behind it. That is sound for the same reason it is sound in async_latch, whose
		// class documentation records the argument and the condition under which it must
		// be revisited.
		detail::resume_claimed(m_waiters.detach_all(), false);
	}

	std::expected<async_mutex_lock, std::error_code> async_mutex::try_lock()
	{
		if(!try_acquire())
		{
			return std::expected<async_mutex_lock, std::error_code>{std::unexpect, std::make_error_code(std::errc::device_or_resource_busy)};
		}

		return std::expected<async_mutex_lock, std::error_code>{guard_for(*this)};
	}

	bool async_mutex::try_acquire() noexcept
	{
		// One exchange rather than a load and a store, because several callers may reach
		// for the same lock at once and only one may have it. A failing attempt writes
		// true over a true, which changes nothing: the lock stays with whoever has it,
		// and the caller has taken nothing.
		return !m_held.exchange(true, std::memory_order_acq_rel);
	}

	void async_mutex::unlock() noexcept
	{
		// DELICATE. Read this before changing it.
		//
		// Where the lock goes is decided inside detach_one's critical section, and
		// clearing m_held when there is nobody to give it to is decided there too. That
		// is the whole reason this is detach_one(action) rather than a detach followed by
		// a store: a waiter reaches this queue only through link(), whose condition runs
		// under the same mutex, so there is no instant at which the lock has been freed
		// and the queue has already been walked past. Split the two -- detach, then clear
		// m_held outside the section -- and a waiter whose link lands in the gap reads
		// m_held as true under that mutex, parks, and stays parked forever beside a lock
		// nobody holds. For async_semaphore the same mistake loses one permit; here it is
		// a deadlock, because no later release will ever come to wake that waiter.
		// waiter_list::detach_one documents the lost wakeup with async_semaphore::release
		// as its worked example.
		//
		// When a waiter is detached, m_held stays true and ownership passes straight to
		// that waiter. It is not cleared and re-contended for, which is what makes a
		// parked waiter impossible to overtake, and leaving it true across the hand-off
		// is also what stops an arriving locker from taking a lock already promised to
		// somebody.
		//
		// The chain drains iteratively, and that is the second thing here that is not a
		// matter of taste. A resumed waiter's guard dies at the end of its critical
		// section and calls this function again; resuming the next waiter from inside
		// that nested call would nest one stack frame per hand-off, so a queue of ten
		// thousand waiters would be ten thousand frames deep. Phase 3 rejected an
		// inline_scheduler on exactly this ground -- "the only difference would be that
		// it overflows the stack" -- and alt::trampoline_scheduler is the library's
		// existing answer to it. This uses the same device: a per-thread flag says a
		// drain is already running, a nested release only records that the lock came
		// back, and the loop below picks it up.
		//
		// A nested release deliberately leaves m_held true. The lock is in transit
		// between this loop and the waiter it will go to next, so an arriving locker
		// must see it as held and park; the loop's next detach_one is what gives it
		// away. Clearing it in the nested call instead would let an arrival take the
		// lock and then let this loop hand the same lock to a parked waiter.
		//
		// Accumulating the whole chain and resuming it once, as async_semaphore::release
		// does, is not available rather than merely not chosen: release() knows n up
		// front and can win every node before it resumes anybody, while this function
		// has one lock to give and cannot know there is a second hand-off to make until
		// the first waiter gives the lock back -- which happens inside the resumption.
		//
		// resume_claimed is the last statement of each iteration that touches *this*.
		// What follows it reads only this thread's own hand-off state, so a resumed
		// waiter may do anything its body likes up to and including releasing the lock
		// again. What it may not do is destroy this mutex: the loop reads m_waiters again
		// whenever the lock comes back, and unlike async_semaphore::release this
		// function cannot promise otherwise. The restriction costs nothing real, because
		// a waiter resumed here is inside the critical section and holds a guard it has
		// to release first, and releasing it is itself a call into this mutex.
		//
		// One visible consequence of leaving m_held true across a hand-off: a waiter
		// that releases the lock and immediately awaits it again does not get it
		// straight back. It finds m_held still true, parks at the back of the queue, and
		// is handed the lock by this loop's next iteration -- fairly, and at the cost of
		// one trip through the queue.
		//
		// The resumption stays outside the queue's mutex, as everywhere else in the
		// library: a resumed waiter runs its body, which may re-await this mutex or take
		// whatever lock it likes. Each iteration resumes a chain of exactly one, so
		// resume_claimed's chain rule has nothing to bite on here.
		handoff_state& state = this_thread_handoff();

		if(state.draining == this)
		{
			state.handed_back = true;
			return;
		}

		// Saved and restored rather than simply set, so that a waiter which locks and
		// unlocks a *different* mutex inside its critical section drains that one
		// without disturbing this drain's record of its own.
		async_mutex* const outer_mutex       = std::exchange(state.draining, this);
		const bool         outer_handed_back = std::exchange(state.handed_back, false);

		for(;;)
		{
			detail::waiter_node* const won = m_waiters.detach_one([this](const bool detached) noexcept {
				if(!detached)
				{
					// Nobody this caller can win, so the lock goes back to being free. Nodes
					// another party has already claimed are the ones that do not count as
					// winnable, and their waiters are on their way out without the lock.
					m_held.store(false, std::memory_order_release);
				}
			});

			if(won == nullptr)
			{
				break;
			}

			state.handed_back = false;
			detail::resume_claimed(won, true);

			if(!state.handed_back)
			{
				break;
			}
		}

		state.draining    = outer_mutex;
		state.handed_back = outer_handed_back;
	}

}

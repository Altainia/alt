#include <alt/run_loop.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>

namespace
{

	// The entry state packs the owning thread's identity into the high half of one
	// word and the nesting depth into the low half, which is what lets an entry be a
	// single compare-exchange. std::thread::id is not an integer and has no portable
	// numeric conversion, so it cannot be packed; a counter can.
	constexpr unsigned      depth_bits = 32;
	constexpr std::uint64_t depth_mask = (std::uint64_t{1} << depth_bits) - 1;
	constexpr std::uint64_t owner_mask = ~depth_mask;
	constexpr std::uint64_t unowned    = 0;

	/** The calling thread's identity, already shifted into the owner half. */
	[[nodiscard]] std::uint64_t this_thread_owner_bits() noexcept
	{
		static std::atomic<std::uint32_t> next_token{1};

		// Nonzero, so a packed word is zero exactly when the loop is unowned. Tokens
		// are handed out once and never reused, so a thread that exits cannot leave an
		// identity behind for a later thread to be mistaken for. They would repeat only
		// after 2^32 threads have been created in the process.
		thread_local const std::uint32_t token = next_token.fetch_add(1, std::memory_order_relaxed);

		return static_cast<std::uint64_t>(token) << depth_bits;
	}

}

namespace alt
{

	run_loop::entry_guard::entry_guard(run_loop& loop) noexcept: m_loop(&loop)
	{
		const std::uint64_t self = this_thread_owner_bits();

		// Entering is one atomic transition. Taking an unowned loop and nesting on one
		// this thread already owns are the same arithmetic, because an unowned word is
		// zero: stamp this thread's identity in and raise the depth by one.
		//
		// The exchange is what makes the check exact. The expected value is never
		// another thread's state, so ownership cannot be stolen; and a failure hands
		// back the word that was really there, so a second thread inside the loop is
		// observed rather than inferred from a load that might have raced a store.
		std::uint64_t observed = unowned;

		while(!m_loop->m_entry_state.compare_exchange_weak(observed,
		                                                   self | ((observed & depth_mask) + 1),
		                                                   std::memory_order_acq_rel,
		                                                   std::memory_order_acquire))
		{
			if(observed != unowned && (observed & owner_mask) != self)
			{
				// Nesting on one thread is legal and supported; two threads at once is
				// not. The word that failed the exchange carried another thread's
				// identity, so this is the violation itself and not a stale read of one.
				// Leave that thread's state untouched and report.
				assert(false && "two threads inside one alt::run_loop at once");
				return;
			}
		}

		m_entered = true;
	}

	run_loop::entry_guard::~entry_guard() noexcept
	{
		if(!m_entered)
		{
			return;
		}

		// While the word is owned, only the owning thread writes it: a competing guard
		// loses its exchange and returns without storing anything, and guards on this
		// thread are strictly nested. So a load and a store are enough here, and no
		// exchange is needed.
		//
		// The last exit clears owner and depth in that one store, which is what closes
		// the window the two-word version had: there was a moment where the depth read
		// zero while a stale owner was still visible, and a guard constructed on
		// another thread in that moment asserted against an owner that had already
		// left.
		const std::uint64_t observed = m_loop->m_entry_state.load(std::memory_order_relaxed);
		const std::uint64_t released = (observed & depth_mask) == 1 ? unowned : observed - 1;

		m_loop->m_entry_state.store(released, std::memory_order_release);
	}

	run_loop::run_loop(run_loop_options options) noexcept: m_options(options)
	{}

	run_loop::~run_loop()
	{
		// Drains rather than discards: queued items may be coroutine resumptions, or a
		// cancelled chain's teardown posted here as its home scheduler, and discarding
		// either leaks frames invisibly.
		drain();
	}

	void run_loop::post(work_item w)
	{
		const bool became_non_empty = m_queue.push(w);

		if(became_non_empty && m_options.on_wake.invoke != nullptr)
		{
			m_options.on_wake.invoke(m_options.on_wake.state);
		}
	}

	std::size_t run_loop::drain()
	{
		std::size_t ran = 0;
		while(run_one())
		{
			++ran;
		}
		return ran;
	}

	void run_loop::run()
	{
		const entry_guard guard{*this};

		while(!m_stopped.load(std::memory_order_acquire))
		{
			work_item next{};
			if(m_queue.pop_or_wait(next, &m_stopped))
			{
				next.invoke(next.state);
			}
		}
	}

	void run_loop::stop() noexcept
	{
		m_stopped.store(true, std::memory_order_release);
		m_queue.notify();
	}

	bool run_loop::stopped() const noexcept
	{
		return m_stopped.load(std::memory_order_acquire);
	}

	std::size_t run_loop::run_pending()
	{
		const entry_guard guard{*this};

		// Taken once. Anything posted during the drain waits for the next call, which
		// is what stops a self-posting item from freezing a host loop.
		std::size_t remaining = m_queue.size();
		std::size_t ran       = 0;

		while(remaining > 0 && run_one())
		{
			remaining -= 1;
			ran += 1;
		}

		return ran;
	}

	bool run_loop::run_one()
	{
		work_item next{};
		if(!m_queue.try_pop(next))
		{
			return false;
		}
		next.invoke(next.state);
		return true;
	}

	bool run_loop::run_one_or_wait(const std::atomic<bool>* stop)
	{
		const entry_guard guard{*this};

		work_item next{};
		if(!m_queue.pop_or_wait(next, stop))
		{
			return false;
		}
		next.invoke(next.state);
		return true;
	}

	void run_loop::notify_waiters()
	{
		m_queue.notify();
	}

	bool run_loop::empty() const
	{
		return m_queue.empty();
	}

}

#include <alt/run_loop.hpp>
#include <cassert>
#include <thread>

namespace alt
{

	run_loop::entry_guard::entry_guard(run_loop& loop) noexcept: m_loop(&loop)
	{
		const auto self = std::this_thread::get_id();

		if(m_loop->m_depth.load(std::memory_order_acquire) == 0)
		{
			m_loop->m_owner.store(self, std::memory_order_release);
		}
		else
		{
			// Nesting on one thread is legal and supported; two threads at once is not.
			assert(m_loop->m_owner.load(std::memory_order_acquire) == self &&
			       "two threads inside one alt::run_loop at once");
		}

		m_loop->m_depth.fetch_add(1, std::memory_order_acq_rel);
	}

	run_loop::entry_guard::~entry_guard() noexcept
	{
		if(m_loop->m_depth.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			m_loop->m_owner.store(std::thread::id{}, std::memory_order_release);
		}
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

#include <algorithm>
#include <alt/thread_pool.hpp>
#include <utility>

namespace alt
{

	std::expected<thread_pool, std::error_code> thread_pool::create(std::size_t threads)
	{
		const std::size_t requested = threads == 0 ? std::size_t{std::thread::hardware_concurrency()} : threads;
		const std::size_t count     = std::max(std::size_t{1}, requested);

		try
		{
			// Constructed in place: the pool is not movable, so it cannot be built here
			// and moved into the expected. In place is also why the caller's reference
			// into the expected has a stable address.
			return std::expected<thread_pool, std::error_code>{std::in_place, passkey{}, count};
		}
		catch(const std::system_error& failure)
		{
			return std::unexpected(failure.code());
		}
	}

	thread_pool::thread_pool(passkey /*unused*/, std::size_t threads)
	{
		m_threads.reserve(threads);

		try
		{
			for(std::size_t started = 0; started < threads; ++started)
			{
				m_threads.emplace_back([this] { worker(); });
			}
		}
		catch(...)
		{
			// DELICATE. The workers already started are parked in the queue, and
			// std::jthread's destructor joins. Without closing the queue first, a
			// partial failure would hang forever here instead of returning an error.
			m_queue.close();
			throw;
		}
	}

	thread_pool::~thread_pool()
	{
		// A draining close, not a discarding one: workers keep taking items and come
		// back only once the queue is empty, so joining them is what makes this wait
		// for the queued work.
		m_queue.close();
	}

	void thread_pool::post(work_item w)
	{
		m_queue.push(w);
	}

	std::size_t thread_pool::drain()
	{
		std::size_t ran = 0;
		work_item   next{};

		while(m_queue.try_pop(next))
		{
			next.invoke(next.state);
			ran += 1;
		}

		return ran;
	}

	std::size_t thread_pool::thread_count() const noexcept
	{
		return m_threads.size();
	}

	bool thread_pool::empty() const
	{
		return m_queue.empty();
	}

	void thread_pool::worker()
	{
		for(;;)
		{
			work_item next{};

			if(m_queue.pop_or_wait(next, nullptr))
			{
				next.invoke(next.state);
				continue;
			}

			// Came back without an item. Only a close means there will never be another;
			// anything else is spurious and we wait again. Since a closed queue still
			// hands out what it holds, reaching here means it is closed and empty.
			if(m_queue.is_closed())
			{
				return;
			}
		}
	}
}

#include <alt/io_backend.hpp>
#include <cassert>
#include <utility>

namespace alt
{

	std::expected<io_token, std::error_code> manual_io_backend::submit(const io_request& request,
	                                                                   io_completion_fn  on_done,
	                                                                   void*             state)
	{
		std::optional<io_completion> inline_result;
		io_token                     token;
		{
			const std::lock_guard lock(m_mutex);
			if(m_fail_next)
			{
				const std::error_code error = *m_fail_next;
				m_fail_next.reset();
				return std::unexpected(error);
			}

			m_entries.push_back(entry{.request = request, .on_done = on_done, .state = state});
			token = io_token(m_entries.size());
			++m_pending;
			inline_result = std::exchange(m_inline_next, std::nullopt);
		}

		if(inline_result)
		{
			complete(token.value() - 1, *inline_result);
		}
		return token;
	}

	void manual_io_backend::cancel(io_token token) noexcept
	{
		const std::lock_guard lock(m_mutex);
		const std::uint64_t   value = token.value();
		if(value != 0 && value <= m_entries.size())
		{
			m_entries[value - 1].cancelled = true;
		}
	}

	io_backend_kind manual_io_backend::kind() const noexcept
	{
		return io_backend_kind::manual;
	}

	std::size_t manual_io_backend::pending() const
	{
		const std::lock_guard lock(m_mutex);
		return m_pending;
	}

	void manual_io_backend::complete(std::size_t index, io_completion result)
	{
		io_completion_fn on_done = nullptr;
		void*            state   = nullptr;
		io_buffer        released;
		{
			const std::lock_guard lock(m_mutex);
			assert(index < m_entries.size() && "complete() of an index never submitted");
			assert(!m_entries[index].completed && "complete() of an operation already completed");
			entry& target    = m_entries[index];
			on_done          = target.on_done;
			state            = target.state;
			target.completed = true;
			// Drop the backend's copy of the buffer, as a real backend does.
			released = std::exchange(target.request.buffer, io_buffer{});
			--m_pending;
		}
		on_done(state, result);
		// `released` dies here, after the callback: the storage is alive for it.
	}

	void manual_io_backend::complete_inline_next(io_completion result)
	{
		const std::lock_guard lock(m_mutex);
		m_inline_next = result;
	}

	void manual_io_backend::fail_next_submission(std::error_code error)
	{
		const std::lock_guard lock(m_mutex);
		m_fail_next = error;
	}

	bool manual_io_backend::was_cancelled(std::size_t index) const
	{
		const std::lock_guard lock(m_mutex);
		assert(index < m_entries.size() && "was_cancelled() of an index never submitted");
		return m_entries[index].cancelled;
	}

	const io_request& manual_io_backend::request(std::size_t index) const
	{
		const std::lock_guard lock(m_mutex);
		assert(index < m_entries.size() && "request() of an index never submitted");
		return m_entries[index].request;
	}

} // namespace alt

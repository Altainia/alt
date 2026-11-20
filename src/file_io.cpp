#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/thread_pool.hpp>
#include <cassert>
#include <memory>
#include <new>
#include <utility> // std::in_place

namespace alt
{

	namespace
	{

		/**
		 * Resolves a requested kind to one this build provides.
		 *
		 * The probe's structure rather than its content: in 7a `automatic` always
		 * settles on the thread pool, and a later phase adds branches here ahead of that
		 * fallback instead of adding a mechanism.
		 */
		std::expected<io_backend_kind, std::error_code> probe(const io_backend_kind requested)
		{
			switch(requested)
			{
			case io_backend_kind::automatic:
			case io_backend_kind::thread_pool:
				return io_backend_kind::thread_pool;
			case io_backend_kind::manual:
				return std::unexpected(std::make_error_code(std::errc::invalid_argument));
			case io_backend_kind::io_uring:
			case io_backend_kind::iocp:
				return std::unexpected(std::make_error_code(std::errc::not_supported));
			}

			return std::unexpected(std::make_error_code(std::errc::invalid_argument));
		}

		/** The argument checks both create() overloads share. */
		std::error_code check_arguments(const scheduler_ref completion, const file_io_options& options)
		{
			if(options.max_in_flight == 0 || !completion)
			{
				return std::make_error_code(std::errc::invalid_argument);
			}

			return {};
		}

	}

	std::expected<file_io, std::error_code> file_io::create(thread_pool& workers, const scheduler_ref completion, const file_io_options& options)
	{
		if(const std::error_code bad = check_arguments(completion, options))
		{
			return std::unexpected(bad);
		}

		const std::expected<io_backend_kind, std::error_code> chosen = probe(options.backend);
		if(!chosen)
		{
			return std::unexpected(chosen.error());
		}

		try
		{
			std::unique_ptr<io_backend> owned = make_thread_pool_backend(workers);
			io_backend&                 seam  = *owned;
			assert(seam.kind() == *chosen && "the backend built is not the one the probe settled on");
			return std::expected<file_io, std::error_code>{std::in_place, passkey{}, std::move(owned), seam, workers, completion, options.max_in_flight};
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
		}
	}

	std::expected<file_io, std::error_code> file_io::create(thread_pool& workers, const scheduler_ref completion, io_backend& backend, const file_io_options& options)
	{
		if(const std::error_code bad = check_arguments(completion, options))
		{
			return std::unexpected(bad);
		}

		// `automatic` means "whatever was passed". Anything else that disagrees with the
		// backend is a caller who asked for one thing and would silently get another.
		if(options.backend != io_backend_kind::automatic && options.backend != backend.kind())
		{
			return std::unexpected(std::make_error_code(std::errc::invalid_argument));
		}

		try
		{
			return std::expected<file_io, std::error_code>{std::in_place, passkey{}, nullptr, backend, workers, completion, options.max_in_flight};
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
		}
	}

	file_io::file_io(passkey /*unused*/, std::unique_ptr<io_backend> owned, io_backend& backend, thread_pool& workers, const scheduler_ref completion, const std::size_t max_in_flight):
	  m_owned_backend(std::move(owned)),
	  m_shared(std::make_shared<detail::io_shared>(backend, completion, &workers, static_cast<std::ptrdiff_t>(max_in_flight))),
	  m_kind(backend.kind())
	{}

	file_io::~file_io()
	{
		// Violating the first precondition is not survivable: a parked operation's
		// frame holds this state alive, so nothing crashes here, but nothing will ever
		// release the permit it is waiting for either. Say so instead of hanging later.
		// parked_waiters() is an upper bound: an operation a cancel has claimed but not
		// yet unlinked still counts. That cannot abort a destroyer that has met the
		// precondition, because the claimant unlinks before it resumes the chain, so a
		// destroyer that waited for its chains never sees the overcount. One racing a
		// cancel on another thread has not outlived its operations, so the abort is
		// right there too.
		assert(m_shared->parked_waiters() == 0 && "destroying an alt::file_io with an operation parked on a backpressure slot");

		m_shared->stop_accepting();

		// Backend quiescence only. Resumptions the backend has already posted stay on
		// the completion scheduler, and they hold their own reference to m_shared.
		m_shared->wait_until_quiet();
	}

	io_backend_kind file_io::backend() const noexcept
	{
		return m_kind;
	}

}

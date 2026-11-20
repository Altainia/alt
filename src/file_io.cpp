#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/thread_pool.hpp>
#include <cassert>
#include <filesystem>
#include <memory>
#include <new>
#include <utility> // std::in_place
#include <vector>

namespace alt
{

	namespace
	{

		using detail::acquire_permit;
		using detail::io_awaiter;
		using detail::io_shared;

		// The shape every path operation shares (written out in each, as async_file.cpp does, rather
		// than behind a helper task: awaiting a task<io_completion> trips modernize-use-nodiscard
		// on task.hpp's await_resume):
		//   auto permit = co_await acquire_permit(owner);    // non-const: it is moved from below
		//   co_await io_awaiter{owner, request, nullptr, std::move(*permit)};
		// `in` and `out` point into `state`, which the request carries, so the operation's
		// block co-owns them. A coroutine frame would not do: the backend reads and writes
		// them on a worker thread, possibly after a cancelled frame is gone.

		/** The verdict for an operation whose chain was released before it could be submitted. */
		std::unexpected<std::error_code> cancelled()
		{
			return std::unexpected(std::make_error_code(std::errc::operation_canceled));
		}

		/** The result slot and the argument of a path operation that reports one value. */
		template<typename Result>
		struct path_query
		{
			std::filesystem::path path;
			Result                result;
		};

		task<std::expected<file_status, std::error_code>> stat_path(std::shared_ptr<io_shared> owner, std::filesystem::path path)
		{
			const auto query  = std::make_shared<path_query<file_status>>(path_query<file_status>{.path = std::move(path), .result = {}});
			auto       permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return cancelled();
			}
			const io_completion done = co_await io_awaiter{owner, io_request{.op = io_op::stat_path, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = &query->path, .out = &query->result, .state = query}, nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return query->result;
		}

		task<std::expected<std::vector<directory_entry>, std::error_code>> list_path(std::shared_ptr<io_shared> owner, std::filesystem::path path)
		{
			const auto query  = std::make_shared<path_query<std::vector<directory_entry>>>(path_query<std::vector<directory_entry>>{.path = std::move(path), .result = {}});
			auto       permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return cancelled();
			}
			const io_completion done = co_await io_awaiter{owner, io_request{.op = io_op::list_directory, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = &query->path, .out = &query->result, .state = query}, nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return std::move(query->result);
		}

		task<std::expected<void, std::error_code>> unlink_path(std::shared_ptr<io_shared> owner, std::filesystem::path path)
		{
			const auto target = std::make_shared<std::filesystem::path>(std::move(path));
			auto       permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return cancelled();
			}
			const io_completion done = co_await io_awaiter{owner, io_request{.op = io_op::unlink, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = target.get(), .out = nullptr, .state = target}, nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return std::expected<void, std::error_code>{};
		}

		task<std::expected<void, std::error_code>> rename_path(std::shared_ptr<io_shared> owner, std::filesystem::path from, std::filesystem::path to)
		{
			const auto args   = std::make_shared<io_rename_args>(io_rename_args{.from = std::move(from), .to = std::move(to)});
			auto       permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return cancelled();
			}
			const io_completion done = co_await io_awaiter{owner, io_request{.op = io_op::rename, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = args.get(), .out = nullptr, .state = args}, nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return std::expected<void, std::error_code>{};
		}

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

	task<std::expected<file_status, std::error_code>> file_io::stat(const std::filesystem::path& path) const
	{
		return stat_path(m_shared, path);
	}

	task<std::expected<void, std::error_code>> file_io::rename(const std::filesystem::path& from, const std::filesystem::path& to) const
	{
		return rename_path(m_shared, from, to);
	}

	task<std::expected<void, std::error_code>> file_io::unlink(const std::filesystem::path& path) const
	{
		return unlink_path(m_shared, path);
	}

	task<std::expected<std::vector<directory_entry>, std::error_code>> file_io::list_directory(const std::filesystem::path& path) const
	{
		return list_path(m_shared, path);
	}

}

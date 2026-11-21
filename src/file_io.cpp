#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/thread_pool.hpp>
#include <cassert>
#include <expected>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>
#include <utility> // std::in_place
#include <vector>

// Guarded inside: on every other platform it declares nothing.
#include "io_backend_iocp.hpp"

namespace alt
{

	namespace
	{

		using detail::acquire_permit;
		using detail::io_awaiter;
		using detail::io_shared;

		// The shape every path operation shares, written out in each rather than behind a helper:
		//   auto permit = co_await acquire_permit(owner);    // non-const: it is moved from below
		//   co_await io_awaiter{owner, request, nullptr, std::move(*permit)};
		// The two awaits cannot move into a function without that function being a coroutine,
		// and a coroutine helper returning task<io_completion> is a second frame per operation,
		// which the one-frame-per-operation cost model of
		// docs/superpowers/specs/2026-11-20-awaitable-file-io-design.md rules out. What does
		// not need a coroutine is factored: path_request builds the request and cancelled()
		// the verdict. The sequence is also written out once more, in file_io.hpp's
		// run_invocation, a header template that cannot reach these helpers.
		// `in` and `out` point into `state`, which the request carries, so the operation's
		// block co-owns them. A coroutine frame would not do: the backend reads and writes
		// them on a worker thread, possibly after a cancelled frame is gone.

		/** The verdict for an operation whose chain was released before it could be submitted. */
		std::unexpected<std::error_code> cancelled()
		{
			return std::unexpected(std::make_error_code(std::errc::operation_canceled));
		}

		/**
		 * The request for a path operation: no descriptor and no buffer, `in` and `out` pointing
		 * into `state`, which the operation's block co-owns for as long as a worker can touch them.
		 */
		io_request path_request(const io_op op, const void* const in, void* const out, std::shared_ptr<void> state)
		{
			return io_request{.op = op, .handle = invalid_native_handle, .buffer = {}, .offset = 0, .size = 0, .in = in, .out = out, .state = std::move(state)};
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
			const io_completion done = co_await io_awaiter{owner, path_request(io_op::stat_path, &query->path, &query->result, query), nullptr, std::move(*permit)};
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
			const io_completion done = co_await io_awaiter{owner, path_request(io_op::list_directory, &query->path, &query->result, query), nullptr, std::move(*permit)};
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
			const io_completion done = co_await io_awaiter{owner, path_request(io_op::unlink, target.get(), nullptr, target), nullptr, std::move(*permit)};
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
			const io_completion done = co_await io_awaiter{owner, path_request(io_op::rename, args.get(), nullptr, args), nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return std::expected<void, std::error_code>{};
		}

		using built_backend = std::expected<std::unique_ptr<io_backend>, std::error_code>;

#ifdef _WIN32
		/** Windows' `automatic`: IOCP, or the thread pool if the port or its thread cannot be created. */
		built_backend automatic_on_windows(thread_pool& workers)
		{
			built_backend iocp = detail::make_iocp_backend(workers);
			if(iocp)
			{
				return iocp;
			}
			return make_thread_pool_backend(workers);
		}
#endif

		/**
		 * The probe: resolves a requested kind to a backend this build provides, and builds it.
		 *
		 * Resolving and building are one step because on Windows whether `automatic` gets IOCP
		 * depends on whether building it succeeds. It falls back to the thread pool when the
		 * port or its thread cannot be created, which file_io::backend() then reports; an
		 * explicit `iocp` request fails with that error instead. A later backend adds its branch
		 * ahead of the fallback in the same way.
		 *
		 * May throw std::bad_alloc, which create() reports.
		 */
		built_backend probe(thread_pool& workers, const io_backend_kind requested)
		{
			// Each platform-specific case returns ahead of the shared one below it; elsewhere
			// its label is empty and it falls into that one.
			switch(requested)
			{
			case io_backend_kind::automatic:
#ifdef _WIN32
				return automatic_on_windows(workers);
#endif
			case io_backend_kind::thread_pool:
				return make_thread_pool_backend(workers);
			case io_backend_kind::iocp:
#ifdef _WIN32
				return detail::make_iocp_backend(workers);
#endif
			case io_backend_kind::io_uring:
				return std::unexpected(std::make_error_code(std::errc::not_supported));
			case io_backend_kind::manual:
				return std::unexpected(std::make_error_code(std::errc::invalid_argument));
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

		try
		{
			built_backend built = probe(workers, options.backend);
			if(!built)
			{
				return std::unexpected(built.error());
			}

			std::unique_ptr<io_backend> owned = std::move(*built);
			io_backend&                 seam  = *owned;
			assert(seam.kind() != io_backend_kind::automatic && (options.backend == io_backend_kind::automatic || seam.kind() == options.backend) && "the probe built a backend other than the one requested");

			// IOCP transfers only on handles its own open prepared, overlapped and associated
			// with its port, so an adopted file's operations go to a thread-pool backend on the
			// same pool, which serves any handle. Every other backend serves adopted files itself.
			std::unique_ptr<io_backend> adopted      = seam.kind() == io_backend_kind::iocp ? make_thread_pool_backend(workers) : nullptr;
			io_backend&                 adopted_seam = adopted ? *adopted : seam;
			return std::expected<file_io, std::error_code>{std::in_place, passkey{}, std::move(owned), std::move(adopted), seam, adopted_seam, completion, options.max_in_flight};
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
		}
	}

	std::expected<file_io, std::error_code> file_io::create(const scheduler_ref completion, io_backend& backend, const file_io_options& options)
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
			// The caller's backend serves adopted files too: there is no second one to route to.
			return std::expected<file_io, std::error_code>{std::in_place, passkey{}, nullptr, nullptr, backend, backend, completion, options.max_in_flight};
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
		}
	}

	file_io::file_io(passkey /*unused*/, std::unique_ptr<io_backend> owned, std::unique_ptr<io_backend> owned_adopted, io_backend& backend, io_backend& adopted_backend, const scheduler_ref completion, const std::size_t max_in_flight):
	  m_owned_backend(std::move(owned)),
	  m_owned_adopted_backend(std::move(owned_adopted)),
	  m_shared(std::make_shared<detail::io_shared>(backend, adopted_backend, completion, static_cast<std::ptrdiff_t>(max_in_flight))),
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

		// Backend quiescence only, for both backends at once: an IOCP service's second,
		// adopted-file backend counts its operations in the same m_shared. Both are
		// destroyed after this returns, which is what each of them requires. Resumptions
		// a backend has already posted stay on the completion scheduler, and they hold
		// their own reference to m_shared.
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

#include <alt/async_file.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <filesystem>
#include <memory>
#include <utility>

// Every operation in this file is a coroutine taking its state BY VALUE, and the public
// members are plain functions that forward to it. A member coroutine would capture `this`,
// and a lazy task can start long after the async_file that made it has moved or died; a
// by-value parameter is copied into the frame when the member is called instead. The
// forwarding adds no second frame: the member allocates nothing but the one it returns.
//
// Each coroutine that reaches the backend runs the same two awaits:
//   auto permit = co_await acquire_permit(owner);   // test it; non-const, it is moved from below
//   const io_completion done = co_await io_awaiter{owner, request, file, std::move(*permit)};
// They stay written out in each operation: the awaits cannot leave a coroutine, and a coroutine
// helper returning task<io_completion> would be a second frame per operation, which the
// one-frame-per-operation cost model of docs/superpowers/specs/2026-11-20-awaitable-file-io-design.md
// rules out. What needs no
// coroutine is factored: stateful_request builds the request and failure() the verdict.
// There is nothing to remember beyond that, because the types leave nothing else to get
// wrong: the permit is an object that returns itself if the frame dies holding it (an
// early return, a throw, or cancellation abandoning the chain between the two awaits),
// and the awaiter cannot be built without one. Validation still comes before the first
// await, so a rejecting path never takes a permit at all.

namespace alt
{

	namespace
	{

		using detail::acquire_permit;
		using detail::io_awaiter;
		using detail::io_handle;
		using detail::io_shared;

		std::unexpected<std::error_code> failure(const std::errc code)
		{
			return std::unexpected(std::make_error_code(code));
		}

		/**
		 * The request for an operation that moves no bytes: only `in` and `out` into `state`,
		 * which the operation's block co-owns for as long as a worker can touch them.
		 */
		io_request stateful_request(const io_op op, const native_handle handle, const void* const in, void* const out, std::shared_ptr<void> state)
		{
			return io_request{.op = op, .handle = handle, .buffer = {}, .offset = 0, .size = 0, .in = in, .out = out, .state = std::move(state)};
		}

		/** True if an operation may be started on this descriptor. */
		bool usable(const std::shared_ptr<io_handle>& file, const std::shared_ptr<io_shared>& owner) noexcept
		{
			return file && owner && file->has_handle() && !file->close_requested();
		}

		/** What an open's block owns: the arguments, and the descriptor the backend produced. */
		struct open_state
		{
			explicit open_state(io_open_args arguments): args(std::move(arguments))
			{}

			open_state(const open_state&)            = delete;
			open_state& operator=(const open_state&) = delete;
			open_state(open_state&&)                 = delete;
			open_state& operator=(open_state&&)      = delete;

			/** Closes a descriptor nobody claimed: the open finished after its awaiter was cancelled. */
			~open_state()
			{
				if(created != invalid_native_handle)
				{
					static_cast<void>(io_handle::adopt(created));
				}
			}

			io_open_args  args;
			native_handle created = invalid_native_handle;
		};

		task<std::expected<std::size_t, std::error_code>> transfer(std::shared_ptr<io_shared> owner, std::shared_ptr<io_handle> file, const io_op op, io_buffer buffer, const std::uint64_t offset, const std::size_t size)
		{
			if(!usable(file, owner))
			{
				co_return failure(std::errc::bad_file_descriptor);
			}
			// Rejected, not clamped: the backend clamps as well, but a clamp reports a short
			// transfer that a caller cannot tell from a real one.
			if(size > buffer.size())
			{
				co_return failure(std::errc::invalid_argument);
			}
			if(size == 0)
			{
				co_return std::size_t{0};
			}

			auto permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await io_awaiter{owner, io_request{.op = op, .handle = file->native(), .buffer = std::move(buffer), .offset = offset, .size = size, .in = nullptr, .out = nullptr, .state = nullptr}, file, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return done.bytes;
		}

		task<std::expected<void, std::error_code>> flush(std::shared_ptr<io_shared> owner, std::shared_ptr<io_handle> file, const io_op op)
		{
			if(!usable(file, owner))
			{
				co_return failure(std::errc::bad_file_descriptor);
			}

			auto permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await io_awaiter{owner, stateful_request(op, file->native(), nullptr, nullptr, nullptr), file, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return std::expected<void, std::error_code>{};
		}

		task<std::expected<file_status, std::error_code>> describe(std::shared_ptr<io_shared> owner, std::shared_ptr<io_handle> file)
		{
			if(!usable(file, owner))
			{
				co_return failure(std::errc::bad_file_descriptor);
			}

			const std::shared_ptr<file_status> result = std::make_shared<file_status>();
			auto                               permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await io_awaiter{owner, stateful_request(io_op::stat_handle, file->native(), nullptr, result.get(), result), file, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return *result;
		}

		task<std::expected<async_file, std::error_code>> open_file(std::shared_ptr<io_shared> owner, std::filesystem::path path, const open_flags how, const file_mode mode)
		{
			// Rejected before a permit is spent, as transfer() does for a bad descriptor or an
			// oversized size. The backend repeats the check for callers that drive it directly.
			if(!how.has_any(flags(open_flag::read)) && !how.has_any(flags(open_flag::write)))
			{
				co_return failure(std::errc::invalid_argument);
			}

			const std::shared_ptr<open_state> state = std::make_shared<open_state>(io_open_args{.path = std::move(path), .open_flags = how, .mode = mode});

			auto permit = co_await acquire_permit(owner);
			if(!permit)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await io_awaiter{owner, stateful_request(io_op::open, invalid_native_handle, &state->args, &state->created, state), nullptr, std::move(*permit)};
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}

			// state->created is cleared only once adopt() has succeeded: if it throws, the
			// state's destructor still closes the descriptor. The one caller that passes
			// `opened`: this handle came from the service's own backend, prepared for it, so
			// its operations may go there. Every other adopt() keeps the safe default.
			std::shared_ptr<io_handle> file = io_handle::adopt(state->created, detail::handle_origin::opened);
			state->created                  = invalid_native_handle;
			co_return detail::async_file_access::make(std::move(file), std::move(owner));
		}

		task<std::expected<void, std::error_code>> finish_close(std::shared_ptr<io_handle> file)
		{
			// Dropped here, and not left to the frame's destruction, so that awaiting this
			// task is what closes the descriptor when nothing else holds it.
			file.reset();
			co_return std::expected<void, std::error_code>{};
		}

	}

	async_file::async_file(std::shared_ptr<io_handle> file, std::shared_ptr<io_shared> owner) noexcept: m_file(std::move(file)), m_owner(std::move(owner))
	{}

	async_file async_file::adopt(const file_io& io, const native_handle handle)
	{
		return async_file{io_handle::adopt(handle), detail::file_io_access::shared(io)};
	}

	task<std::expected<std::size_t, std::error_code>> async_file::read_at(io_buffer buffer, const std::uint64_t offset, const std::size_t size) const
	{
		return transfer(m_owner, m_file, io_op::read, std::move(buffer), offset, size);
	}

	task<std::expected<std::size_t, std::error_code>> async_file::write_at(io_buffer buffer, const std::uint64_t offset, const std::size_t size) const
	{
		return transfer(m_owner, m_file, io_op::write, std::move(buffer), offset, size);
	}

	task<std::expected<void, std::error_code>> async_file::sync(const sync_mode mode) const
	{
		return flush(m_owner, m_file, mode == sync_mode::data ? io_op::sync_data : io_op::sync_full);
	}

	task<std::expected<file_status, std::error_code>> async_file::stat() const
	{
		return describe(m_owner, m_file);
	}

	task<std::expected<void, std::error_code>> async_file::close()
	{
		if(m_file)
		{
			m_file->request_close();
		}
		m_owner.reset();
		return finish_close(std::move(m_file));
	}

	native_handle async_file::handle() const noexcept
	{
		return m_file ? m_file->native() : invalid_native_handle;
	}

	io_backend_kind async_file::backend() const noexcept
	{
		// Read from the kinds io_shared stored at construction, never through a backend
		// pointer: the service may be gone and its backends with it, while this state lives on.
		if(!m_owner)
		{
			return io_backend_kind::thread_pool;
		}
		return m_file && m_file->origin() == detail::handle_origin::adopted ? m_owner->adopted_kind : m_owner->primary_kind;
	}

	task<std::expected<async_file, std::error_code>> file_io::open(const std::filesystem::path& path, const open_flags how, const file_mode mode) const
	{
		return open_file(detail::file_io_access::shared(*this), path, how, mode);
	}

}

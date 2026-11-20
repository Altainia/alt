#include <alt/async_file.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

// Every operation in this file is a coroutine taking its state BY VALUE, and the public
// members are plain functions that forward to it. A member coroutine would capture `this`,
// and a lazy task can start long after the async_file that made it has moved or died; a
// by-value parameter is copied into the frame when the member is called instead. The
// forwarding adds no second frame: the member allocates nothing but the one it returns.
//
// Each coroutine that reaches the backend runs the same three steps, in this order:
//   1. construct the io_awaiter, which allocates and so can throw;
//   2. co_await the backpressure permit and check it;
//   3. co_await the awaiter, which takes the permit over and releases it.
// Step 1 comes first because nothing between acquiring the permit and awaiting the
// awaiter may throw or return: the awaiter is what releases the permit.

namespace alt
{

	namespace
	{

		using detail::io_awaiter;
		using detail::io_handle;
		using detail::io_shared;

		std::unexpected<std::error_code> failure(const std::errc code)
		{
			return std::unexpected(std::make_error_code(code));
		}

		/** True if an operation may be started on this descriptor. */
		bool usable(const std::shared_ptr<io_handle>& file, const std::shared_ptr<io_shared>& owner) noexcept
		{
			return file && owner && file->has_handle() && !file->close_requested();
		}

		/**
		 * A buffer that names no bytes and exists to keep @p state alive.
		 *
		 * io_request::in and ::out must outlive the operation block, and the block's only
		 * co-owned storage is the buffer, so this is where a request that carries a
		 * result slot (a stat, an open) parks it.
		 */
		io_buffer keepalive_buffer(std::shared_ptr<void> state) noexcept
		{
			return io_buffer::borrow(std::span<std::byte>{}, std::move(state));
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

			io_awaiter awaiter{owner, io_request{.op = op, .handle = file->native(), .buffer = std::move(buffer), .offset = offset, .size = size, .in = nullptr, .out = nullptr}, file};

			const wait_result slot = co_await owner->acquire_slot();
			if(!slot)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await std::move(awaiter);
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

			io_awaiter awaiter{owner, io_request{.op = op, .handle = file->native(), .buffer = {}, .offset = 0, .size = 0, .in = nullptr, .out = nullptr}, file};

			const wait_result slot = co_await owner->acquire_slot();
			if(!slot)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await std::move(awaiter);
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
			io_awaiter                         awaiter{owner, io_request{.op = io_op::stat_handle, .handle = file->native(), .buffer = keepalive_buffer(result), .offset = 0, .size = 0, .in = nullptr, .out = result.get()}, file};

			const wait_result slot = co_await owner->acquire_slot();
			if(!slot)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await std::move(awaiter);
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}
			co_return *result;
		}

		task<std::expected<async_file, std::error_code>> open_file(std::shared_ptr<io_shared> owner, std::filesystem::path path, const open_flags how, const file_mode mode)
		{
			const std::shared_ptr<open_state> state = std::make_shared<open_state>(io_open_args{.path = std::move(path), .open_flags = how, .mode = mode});

			io_awaiter awaiter{owner, io_request{.op = io_op::open, .handle = invalid_native_handle, .buffer = keepalive_buffer(state), .offset = 0, .size = 0, .in = &state->args, .out = &state->created}, nullptr};

			const wait_result slot = co_await owner->acquire_slot();
			if(!slot)
			{
				co_return failure(std::errc::operation_canceled);
			}
			const io_completion done = co_await std::move(awaiter);
			if(done.error)
			{
				co_return std::unexpected(done.error);
			}

			// state->created is cleared only once adopt() has succeeded: if it throws, the
			// state's destructor still closes the descriptor.
			std::shared_ptr<io_handle> file = io_handle::adopt(state->created);
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

	task<std::expected<async_file, std::error_code>> file_io::open(const std::filesystem::path& path, const open_flags how, const file_mode mode) const
	{
		return open_file(detail::file_io_access::shared(*this), path, how, mode);
	}

}

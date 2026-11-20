#include <alt/io_buffer.hpp>
#include <new>
#include <utility>

namespace alt
{

	io_buffer::io_buffer(std::span<std::byte> bytes, std::shared_ptr<void> owner) noexcept
	  : m_owner(std::move(owner)),
	    m_bytes(bytes),
	    m_named(true)
	{
	}

	std::expected<io_buffer, std::error_code> io_buffer::allocate(const std::size_t size)
	{
		try
		{
			std::shared_ptr<std::byte[]> storage = std::make_shared<std::byte[]>(size);
			const std::span<std::byte>   bytes{storage.get(), size};
			return io_buffer{bytes, std::move(storage)};
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
		}
	}

	io_buffer io_buffer::borrow(const std::span<std::byte> bytes, std::shared_ptr<void> keepalive) noexcept
	{
		return io_buffer{bytes, std::move(keepalive)};
	}

	io_buffer io_buffer::borrow_unsafe(const std::span<std::byte> bytes) noexcept
	{
		return io_buffer{bytes, nullptr};
	}

}

#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace alt
{

	/**
	 * @brief A refcounted byte buffer for asynchronous I/O.
	 *
	 * A read the kernel is still performing writes into memory the caller handed
	 * over, and the coroutine that asked for it can be destroyed first (cancelled,
	 * or abandoned by its owner). A plain std::span would then dangle under the
	 * kernel's feet. io_buffer is refcounted so that the operation can hold its own
	 * copy and the memory lives until the last holder, kernel-facing or not, lets go.
	 *
	 * Three factories cover three ownership stories, and all three fit one shared
	 * pointer plus one span, so there is no variant and no second owner. (The class
	 * also carries a flag recording whether a factory produced it, which is what
	 * operator bool reports; it takes no part in ownership.)
	 *
	 *  - allocate() owns fresh storage.
	 *  - borrow() views storage someone else owns, and holds that owner's keepalive
	 *    for as long as any copy lives.
	 *  - borrow_unsafe() views storage with nothing holding it. The caller promises
	 *    the bytes outlive every operation that uses the buffer, including one the
	 *    caller has abandoned but the kernel has not finished.
	 *
	 * Copies share storage. Copying is cheap (one atomic increment) and never copies
	 * bytes: a write through one copy is visible through every other.
	 *
	 * Thread safety: the reference count is atomic, so copying, moving and
	 * destroying io_buffers across threads is safe. The *bytes* are not interlocked.
	 * Two concurrent operations must not share a buffer region, and a caller must not
	 * touch a region while an operation owns it. This is a precondition, not
	 * something the type enforces.
	 *
	 * The header includes no platform headers; the buffer is plain memory.
	 */
	class io_buffer
	{
	public:
		/**
		 * @brief Constructs an empty buffer that names no storage.
		 *
		 * size() is 0 and the buffer converts to false. This is the only state in
		 * which the buffer is false; see operator bool.
		 */
		io_buffer() noexcept = default;

		/** @brief Copies share storage; see the class description. */
		io_buffer(const io_buffer&) noexcept = default;

		/** @brief Copies share storage; see the class description. */
		io_buffer& operator=(const io_buffer&) noexcept = default;

		/**
		 * @brief Takes the storage and leaves @p other empty and false.
		 *
		 * User-provided on purpose, not defaulted. A defaulted move would move the
		 * owner out but copy the span and flag, leaving the source with a live-looking
		 * bytes() into storage it no longer keeps alive: the dangling view this type
		 * exists to remove, and one that becomes a use-after-free if a moved-from
		 * buffer ever reaches the kernel. Do not simplify back to = default.
		 */
		io_buffer(io_buffer&& other) noexcept
		  : m_owner(std::move(other.m_owner)),
		    m_bytes(std::exchange(other.m_bytes, {})),
		    m_named(std::exchange(other.m_named, false))
		{
		}

		/** @brief Move-assigns as the move constructor does; self-move is a no-op. */
		io_buffer& operator=(io_buffer&& other) noexcept
		{
			if(this != &other)
			{
				m_owner = std::move(other.m_owner);
				m_bytes = std::exchange(other.m_bytes, {});
				m_named = std::exchange(other.m_named, false);
				other.m_owner.reset();
			}
			return *this;
		}

		~io_buffer() = default;

		/**
		 * @brief Allocates zero-initialized storage of the given size.
		 *
		 * Failure to allocate is reported, not thrown, so a caller sizing a buffer
		 * from untrusted input can handle it.
		 *
		 * A size of 0 succeeds and yields a real buffer with size() 0, which converts
		 * to true. Whether bytes().data() is null for it is unspecified.
		 *
		 * @param size Number of bytes.
		 * @return The buffer, or std::errc::not_enough_memory.
		 */
		[[nodiscard]] static std::expected<io_buffer, std::error_code> allocate(std::size_t size);

		/**
		 * @brief Views storage owned elsewhere, holding the owner's keepalive.
		 *
		 * The keepalive is stored as given, so it costs no extra control block, and released
		 * when the last copy of this buffer dies. It should own, or otherwise keep
		 * alive, the memory behind @p bytes. The buffer does not check that it does.
		 *
		 * @param bytes     The region to expose.
		 * @param keepalive Whatever keeps @p bytes valid; may be null, in which case
		 *                  this behaves like borrow_unsafe().
		 */
		[[nodiscard]] static io_buffer borrow(std::span<std::byte> bytes, std::shared_ptr<void> keepalive) noexcept;

		/**
		 * @brief Views storage with nothing keeping it alive.
		 *
		 * The caller guarantees @p bytes stays valid until every operation using this
		 * buffer has completed in the kernel, which can be later than the moment a
		 * cancelled co_await returns. Prefer borrow() or allocate() unless the
		 * storage is static or otherwise provably immortal for that span of time.
		 *
		 * @param bytes The region to expose.
		 */
		[[nodiscard]] static io_buffer borrow_unsafe(std::span<std::byte> bytes) noexcept;

		/**
		 * @brief The bytes this buffer exposes.
		 *
		 * The span is a view: const-qualifying the buffer does not make the bytes
		 * immutable, because copies share them anyway.
		 */
		[[nodiscard]] std::span<std::byte> bytes() const noexcept
		{
			return m_bytes;
		}

		/** @brief The number of bytes exposed; 0 for an empty or zero-size buffer. */
		[[nodiscard]] std::size_t size() const noexcept
		{
			return m_bytes.size();
		}

		/**
		 * @brief Whether this buffer names storage at all.
		 *
		 * False only for a default-constructed buffer. A moved-from buffer is
		 * reset to the default-constructed state, so it is false too. It is
		 * not "has bytes": a zero-size allocate() result is a real buffer and is true,
		 * so test size() when the question is whether there is anything to read or
		 * write.
		 */
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_named;
		}

	private:
		io_buffer(std::span<std::byte> bytes, std::shared_ptr<void> owner) noexcept;

		std::shared_ptr<void> m_owner;
		std::span<std::byte>  m_bytes;
		bool                  m_named = false;
	};

}

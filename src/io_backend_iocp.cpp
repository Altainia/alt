// The IOCP backend exists only on Windows, so this whole file does too.
#ifdef _WIN32

#	include "io_backend_iocp.hpp"

#	include <algorithm>
#	include <alt/io_backend.hpp>
#	include <alt/io_buffer.hpp>
#	include <alt/thread_pool.hpp>
#	include <array>
#	include <atomic>
#	include <cassert>
#	include <cstddef>
#	include <cstdint>
#	include <exception>
#	include <expected>
#	include <limits>
#	include <memory>
#	include <mutex>
#	include <new>
#	include <system_error>
#	include <thread>
#	include <unordered_map>

#	include "io_backend_internal.hpp"
#	include "io_windows.hpp"

namespace alt::detail
{
	namespace
	{
		using windows_io::is_directory_handle;
		using windows_io::last_error;
		using windows_io::normalize_error;
		using windows_io::to_handle;

		/// The completion key of the packet the destructor posts to stop the completion thread.
		constexpr ULONG_PTR shutdown_key = ~ULONG_PTR{0};

		/// The completion key every handle this backend opens is associated with.
		constexpr ULONG_PTR transfer_key = 0;

		/// How many packets one wake of the completion thread takes off the port at most.
		constexpr ULONG batch_size = 64;

		std::error_code bad_alloc_error() noexcept
		{
			return std::make_error_code(std::errc::not_enough_memory);
		}

		/**
		 * One read or write, owned by the backend from submission until its completion is consumed.
		 *
		 * The OVERLAPPED is the first member, so the address the kernel hands back in a packet is
		 * the record's own. It is never cast back to one, though: the completion thread finds the
		 * record by that address in the registry, and a packet whose address is not registered, or
		 * names a record the kernel has not finished with, is not this backend's to touch. The buffer copy is what keeps the bytes alive while the
		 * kernel owns them; it is released only after the callback has returned.
		 */
		struct transfer_record
		{
			OVERLAPPED       overlapped{};
			io_buffer        buffer;
			io_completion_fn on_done  = nullptr;
			void*            state    = nullptr;
			native_handle    handle   = invalid_native_handle;
			std::uint64_t    token    = 0;
			bool             is_write = false;
		};

		/// What a transfer that failed with Win32 error @p code reports.
		io_completion failed(const transfer_record& record, DWORD code) noexcept
		{
			io_completion result;
			// Reading at or past the end is a short count of zero, not a failure, as on the pool arm.
			if(!record.is_write && code == ERROR_HANDLE_EOF)
			{
				return result;
			}
			// Aborted by CancelIoEx, or by the consumer closing the handle: cancelled either way.
			if(code == ERROR_OPERATION_ABORTED)
			{
				result.error = std::make_error_code(std::errc::operation_canceled);
				return result;
			}
			const std::error_code error = normalize_error({static_cast<int>(code), std::system_category()});
			result.error                = is_directory_handle(record.handle) ? std::make_error_code(std::errc::is_a_directory) : error;
			return result;
		}

		/// The outcome of a transfer the kernel has finished with, read from its OVERLAPPED.
		io_completion settle(transfer_record& record) noexcept
		{
			// Without waiting: the transfer is known complete, so this only reads the status the
			// kernel left in the OVERLAPPED and converts it to a Win32 error.
			DWORD moved = 0;
			if(::GetOverlappedResult(to_handle(record.handle), &record.overlapped, &moved, FALSE) == FALSE)
			{
				return failed(record, ::GetLastError());
			}
			return io_completion{.bytes = moved, .error = {}};
		}

		/**
		 * Whether a ReadFile or WriteFile that returned FALSE with @p code still queues a packet.
		 *
		 * ERROR_IO_PENDING does, by definition. So does ERROR_MORE_DATA: it is the documented
		 * failure of a message-mode pipe read shorter than the message, and it maps a warning-class
		 * status (STATUS_BUFFER_OVERFLOW), which unlike an error completes the request and queues
		 * its packet. Completing such a call inline as well would leave that packet to arrive after
		 * the record is freed, where it could match a newer record at the reused address. Measured
		 * on Windows 11: on an overlapped handle kernel32 reports that case as ERROR_IO_PENDING
		 * anyway and the packet carries ERROR_MORE_DATA, so this is defense against the documented
		 * form rather than a path observed here. Every other failure queues nothing.
		 */
		bool will_queue_a_packet(DWORD code) noexcept
		{
			return code == ERROR_IO_PENDING || code == ERROR_MORE_DATA;
		}
		/// The open policy's prepare hook: associates a new handle with the port in @p context.
		std::error_code associate_with_port(void* context, native_handle handle) noexcept
		{
			const HANDLE port = static_cast<HANDLE>(context);
			if(::CreateIoCompletionPort(to_handle(handle), port, transfer_key, 0) == nullptr)
			{
				return last_error();
			}
			// A transfer that completes synchronously is completed inline by submit(); without this
			// a packet would follow for it as well, naming a record already completed and freed.
			if(::SetFileCompletionNotificationModes(to_handle(handle), FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) == FALSE)
			{
				return last_error();
			}
			return {};
		}

		class iocp_backend final: public io_backend
		{
		public:
			/// Takes ownership of @p port, which the destructor closes. Does not start the thread; see start().
			iocp_backend(thread_pool& pool, HANDLE port):
			  m_port(port),
			  m_pool_backend(make_thread_pool_backend(pool, pool_open_policy{.extra_flags = static_cast<std::uint32_t>(FILE_FLAG_OVERLAPPED), .prepare = associate_with_port, .context = port}))
			{}

			iocp_backend(const iocp_backend&)            = delete;
			iocp_backend& operator=(const iocp_backend&) = delete;
			iocp_backend(iocp_backend&&)                 = delete;
			iocp_backend& operator=(iocp_backend&&)      = delete;

			~iocp_backend() override
			{
				bool in_flight = false;
				{
					const std::scoped_lock lock(m_mutex);
					in_flight = !m_by_address.empty();
				}
				// file_io waits for every operation before destroying its backend, so a record left
				// here is a caller's bug. Freeing it would hand the kernel freed memory to complete
				// into, and leaking it would leave a packet for a thread about to be gone, so neither
				// is a way out: debug builds say why, and every build stops.
				if(in_flight)
				{
					assert(false && "IOCP backend destroyed with transfers in flight");
					std::terminate();
				}
				// The same for an operation forwarded to the pool and not yet done with this backend:
				// a queued open would run its prepare hook later against the port closed below, whose
				// handle value may by then name another port.
				if(m_forwarded.load() != 0)
				{
					assert(false && "IOCP backend destroyed with forwarded operations in flight");
					std::terminate();
				}
				if(m_thread.joinable())
				{
					// Without the wake-up the join below never returns.
					if(::PostQueuedCompletionStatus(m_port, 0, shutdown_key, nullptr) == FALSE)
					{
						std::terminate();
					}
					m_thread.join();
				}
				static_cast<void>(::CloseHandle(m_port));
			}

			/// Starts the completion thread. Called once, by make_iocp_backend().
			std::error_code start() noexcept
			{
				try
				{
					m_thread = std::thread([this] { drain(); });
				}
				catch(const std::system_error& error)
				{
					return error.code();
				}
				catch(const std::bad_alloc&)
				{
					return bad_alloc_error();
				}
				return {};
			}

			[[nodiscard]] std::expected<io_token, std::error_code> submit(const io_request& request, io_completion_fn on_done, void* state) override
			{
				if(request.op == io_op::read || request.op == io_op::write)
				{
					return submit_transfer(request, on_done, state);
				}
				// Everything else has no overlapped form on Windows, open included: the pool
				// backend's open policy is what makes the handles it returns fit for the port.
				return submit_forwarded(request, on_done, state);
			}

			void cancel(io_token token) noexcept override
			{
				if(!token)
				{
					return;
				}
				if((token.value() & iocp_token_tag) == 0)
				{
					m_pool_backend->cancel(token);
					return;
				}
				// CancelIoEx matches a request by handle and OVERLAPPED address, so it is called with
				// the mutex held: completion withdraws a record under the same mutex before freeing
				// it, so a record found here is still the kernel's, and its address cannot yet belong
				// to a newer transfer on the same handle. A token not found has completed, or is
				// completing, and there is nothing to cancel.
				const std::scoped_lock lock(m_mutex);
				const auto             found = m_by_token.find(token.value());
				if(found == m_by_token.end())
				{
					return;
				}
				transfer_record& record = *found->second;
				// A hint: the packet still comes, aborted or with the real result. FALSE with
				// ERROR_NOT_FOUND means the kernel finished first and its packet is queued, or the
				// call has not been issued yet; either way the transfer completes on its own.
				static_cast<void>(::CancelIoEx(to_handle(record.handle), &record.overlapped));
			}

			[[nodiscard]] io_backend_kind kind() const noexcept override
			{
				return io_backend_kind::iocp;
			}

			[[nodiscard]] HANDLE port() const noexcept
			{
				return m_port;
			}

			[[nodiscard]] std::size_t ignored_packets() const noexcept
			{
				return m_ignored.load();
			}

			/// The OVERLAPPED of the transfer @p token names while it is registered, or null.
			[[nodiscard]] const OVERLAPPED* pending_overlapped(io_token token) noexcept
			{
				const std::scoped_lock lock(m_mutex);
				const auto             found = m_by_token.find(token.value());
				return found == m_by_token.end() ? nullptr : &found->second->overlapped;
			}

		private:
			std::expected<io_token, std::error_code> submit_transfer(const io_request& request, io_completion_fn on_done, void* state)
			{
				std::unique_ptr<transfer_record> owned;
				try
				{
					owned = std::make_unique<transfer_record>();
				}
				catch(const std::bad_alloc&)
				{
					return std::unexpected(bad_alloc_error());
				}
				owned->buffer                = request.buffer;
				owned->on_done               = on_done;
				owned->state                 = state;
				owned->handle                = request.handle;
				owned->is_write              = request.op == io_op::write;
				owned->token                 = iocp_token_tag | m_next_id.fetch_add(1);
				owned->overlapped.Offset     = static_cast<DWORD>(request.offset & 0xFFFFFFFFU);
				owned->overlapped.OffsetHigh = static_cast<DWORD>(request.offset >> 32U);

				// Everything the rest of this function needs is taken now: once the transfer is
				// pending, the completion thread may finish and free the record at any moment.
				transfer_record* const record = owned.get();
				const io_token         token(record->token);
				const OVERLAPPED*      key      = &record->overlapped;
				const bool             is_write = record->is_write;
				const HANDLE           handle   = to_handle(record->handle);
				void* const            data     = record->buffer.bytes().data();
				// At most what the buffer holds, and ReadFile and WriteFile take a 32-bit count; a
				// short count is a normal result, exactly as on the pool arm.
				const auto count = static_cast<DWORD>(std::min<std::size_t>(std::min(request.size, record->buffer.size()), std::numeric_limits<DWORD>::max()));

				if(const auto error = enroll(std::move(owned)))
				{
					return std::unexpected(error);
				}
				// Registered before the call, so a packet that arrives before the call even returns
				// finds its record.
				const BOOL  ok   = is_write ? ::WriteFile(handle, data, count, nullptr, &record->overlapped) : ::ReadFile(handle, data, count, nullptr, &record->overlapped);
				const DWORD code = ok != FALSE ? static_cast<DWORD>(ERROR_SUCCESS) : ::GetLastError();
				if(ok == FALSE && will_queue_a_packet(code))
				{
					// The packet will come and the completion thread finishes it. The record is not
					// this thread's to touch any more.
					return token;
				}

				// Completed synchronously, or failed outright: no packet is coming
				// (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS for a success, and none is ever queued for an
				// immediate failure), so this thread completes it, before submit() returns, which the
				// seam allows. The record is withdrawn first, exactly as the packet path does it.
				auto mine = withdraw(key);
				if(!mine)
				{
					// Not reachable while the notification mode holds. Were a packet to come after all,
					// it would find the OVERLAPPED already complete, since the call has returned, so
					// whichever of it and this withdrawal took the record completed it, and the other
					// leaves it alone: either way the callback ran exactly once. This withdrawal does
					// not check completion itself: an immediate failure may leave the OVERLAPPED's
					// status as the STATUS_PENDING kernel32 seeded it with.
					return token;
				}
				const io_completion result = ok != FALSE ? settle(*mine) : failed(*mine, code);
				complete(std::move(mine), result);
				return token;
			}

			/// What a forwarded operation's trampoline needs: the caller's callback, and the count it lowers.
			struct forwarded_operation
			{
				io_completion_fn          on_done     = nullptr;
				void*                     state       = nullptr;
				std::atomic<std::size_t>* outstanding = nullptr;
			};

			/**
			 * Runs on the pool worker when a forwarded operation completes.
			 *
			 * The count is lowered BEFORE the caller's callback runs, and that decrement is this
			 * trampoline's last access to the backend. The other order cannot work: file_io's
			 * callback lowers its own quiescence count as its last act, after which ~file_io may
			 * destroy this backend at once, so a decrement after it would write into a destroyed
			 * backend, or race the destructor's check and stop the process for an operation that was
			 * already finished. Lowering it first is enough, because by the time the callback runs the
			 * operation is done with the backend: an open's prepare hook, the one step that uses the
			 * port, has already run, and the pool job holds everything else it needs.
			 */
			static void forwarded_done(void* raw, io_completion result) noexcept
			{
				const std::unique_ptr<forwarded_operation> operation(static_cast<forwarded_operation*>(raw));
				operation->outstanding->fetch_sub(1);
				operation->on_done(operation->state, result);
			}

			/// Hands @p request to the pool backend through forwarded_done(), counting it while it is in flight.
			std::expected<io_token, std::error_code> submit_forwarded(const io_request& request, io_completion_fn on_done, void* state)
			{
				std::unique_ptr<forwarded_operation> operation;
				try
				{
					operation = std::make_unique<forwarded_operation>(forwarded_operation{.on_done = on_done, .state = state, .outstanding = &m_forwarded});
				}
				catch(const std::bad_alloc&)
				{
					return std::unexpected(bad_alloc_error());
				}
				// Raised before the submission, which may complete inline and lower it again.
				m_forwarded.fetch_add(1);
				auto token = m_pool_backend->submit(request, forwarded_done, operation.get());
				if(!token)
				{
					// A refused submission never runs its callback, so the trampoline is still ours.
					m_forwarded.fetch_sub(1);
					return token;
				}
				// From here the trampoline owns itself; it may already have run and freed it.
				static_cast<void>(operation.release());
				assert((token->value() & iocp_token_tag) == 0 && "a pool token collides with the IOCP tag");
				return token;
			}

			/// Registers @p record by token and by OVERLAPPED address. On failure the record is freed.
			std::error_code enroll(std::unique_ptr<transfer_record> record) noexcept
			{
				try
				{
					const std::scoped_lock lock(m_mutex);
					const std::uint64_t    token = record->token;
					const OVERLAPPED*      key   = &record->overlapped;
					const auto [position, added] = m_by_token.emplace(token, record.get());
					assert(added && "a token was issued twice");
					static_cast<void>(added);
					try
					{
						m_by_address.emplace(key, std::move(record));
					}
					catch(...)
					{
						m_by_token.erase(position);
						throw;
					}
				}
				catch(const std::bad_alloc&)
				{
					return bad_alloc_error();
				}
				return {};
			}

			/**
			 * Removes and returns the record whose OVERLAPPED is at @p key, or null if none is
			 * registered there. Taking it out under the mutex is what makes a record completed at
			 * most once, and what lets cancel() find only records the kernel still owns.
			 */
			std::unique_ptr<transfer_record> withdraw(const OVERLAPPED* key) noexcept
			{
				const std::scoped_lock lock(m_mutex);
				const auto             found = m_by_address.find(key);
				if(found == m_by_address.end())
				{
					return nullptr;
				}
				std::unique_ptr<transfer_record> record = std::move(found->second);
				m_by_address.erase(found);
				m_by_token.erase(record->token);
				return record;
			}

			/**
			 * The packet path's withdrawal: as withdraw(), except that a record whose OVERLAPPED the
			 * kernel has not finished with is left registered, and null is returned for it.
			 *
			 * The lookup is by address alone, and an address does not say which call a packet is
			 * for. A consumer's own untagged overlapped call on handle(), with an OVERLAPPED freed
			 * before its packet is dequeued, leaves that packet naming memory a newer record of ours
			 * may occupy by then. Completing that record would report it failed, its status being
			 * still pending, and free it while the kernel owns its OVERLAPPED and buffer. The kernel
			 * writes a request's final status into its OVERLAPPED before queuing its packet, so a
			 * record's own packet always finds it complete; one that finds it pending is somebody
			 * else's, and the record's own is still to come.
			 */
			std::unique_ptr<transfer_record> withdraw_finished(const OVERLAPPED* key) noexcept
			{
				const std::scoped_lock lock(m_mutex);
				const auto             found = m_by_address.find(key);
				if(found == m_by_address.end() || !HasOverlappedIoCompleted(&found->second->overlapped))
				{
					return nullptr;
				}
				std::unique_ptr<transfer_record> record = std::move(found->second);
				m_by_address.erase(found);
				m_by_token.erase(record->token);
				return record;
			}

			/// Delivers @p result, then frees the record, and its buffer copy with it.
			static void complete(std::unique_ptr<transfer_record> record, io_completion result) noexcept
			{
				record->on_done(record->state, result);
			}

			/// The completion thread: completes every packet it finds a record for, until shutdown.
			void drain() noexcept
			{
				std::array<OVERLAPPED_ENTRY, batch_size> entries{};
				for(;;)
				{
					ULONG count = 0;
					// With no timeout and no alerts this fails only on a port that is gone, which
					// the destructor does not allow while this thread runs.
					if(::GetQueuedCompletionStatusEx(m_port, entries.data(), batch_size, &count, INFINITE, FALSE) == FALSE)
					{
						std::terminate();
					}
					bool stop = false;
					for(ULONG i = 0; i < count; ++i)
					{
						const OVERLAPPED_ENTRY& entry = entries[i];
						if(entry.lpCompletionKey == shutdown_key && entry.lpOverlapped == nullptr)
						{
							stop = true;
							continue;
						}
						auto record = withdraw_finished(entry.lpOverlapped);
						if(!record)
						{
							// Not ours: a consumer's own overlapped call on a handle this backend
							// opened, even if it names the address of a record of ours still
							// pending. Never cast, completed or freed.
							m_ignored.fetch_add(1);
							continue;
						}
						const io_completion result = settle(*record);
						complete(std::move(record), result);
					}
					if(stop)
					{
						return;
					}
				}
			}

			HANDLE                      m_port;
			std::unique_ptr<io_backend> m_pool_backend;

			/// The in-flight registry: every transfer submitted to the kernel and not yet completed.
			std::mutex                                                              m_mutex;
			std::unordered_map<const OVERLAPPED*, std::unique_ptr<transfer_record>> m_by_address;
			std::unordered_map<std::uint64_t, transfer_record*>                     m_by_token;

			std::atomic<std::uint64_t> m_next_id{1};
			std::atomic<std::size_t>   m_ignored{0};
			/// Operations forwarded to the pool backend whose trampoline has not yet run.
			std::atomic<std::size_t> m_forwarded{0};
			std::thread              m_thread;
		};
	} // namespace

	std::expected<std::unique_ptr<io_backend>, std::error_code> make_iocp_backend(thread_pool& pool)
	{
		// One concurrent thread: the port has exactly one waiter.
		const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
		if(port == nullptr)
		{
			return std::unexpected(last_error());
		}
		std::unique_ptr<iocp_backend> backend;
		try
		{
			backend = std::make_unique<iocp_backend>(pool, port);
		}
		catch(const std::bad_alloc&)
		{
			static_cast<void>(::CloseHandle(port));
			return std::unexpected(bad_alloc_error());
		}
		// From here the backend owns the port, and its destructor closes it if the thread fails.
		if(const auto error = backend->start())
		{
			return std::unexpected(error);
		}
		return std::unique_ptr<io_backend>(std::move(backend));
	}

	HANDLE iocp_port_for_testing(io_backend& iocp) noexcept
	{
		return static_cast<iocp_backend&>(iocp).port();
	}

	std::size_t iocp_ignored_packets_for_testing(io_backend& iocp) noexcept
	{
		return static_cast<iocp_backend&>(iocp).ignored_packets();
	}

	const OVERLAPPED* iocp_pending_overlapped_for_testing(io_backend& iocp, io_token token) noexcept
	{
		return static_cast<iocp_backend&>(iocp).pending_overlapped(token);
	}
} // namespace alt::detail

#endif

#include <algorithm>
#include <alt/detail/file_types.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/io_backend.hpp>
#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// windows.h defines min and max as function-like macros, which break std::min and
// std::numeric_limits<>::max(). NOMINMAX is its documented switch to leave them out.
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <fcntl.h>
#	include <sys/stat.h>
#	include <unistd.h>
#endif

namespace alt
{
	namespace
	{
		// ---------------------------------------------------------------------
		// Error mapping
		// ---------------------------------------------------------------------

#ifndef _WIN32
		std::error_code errno_error() noexcept
		{
			return {errno, std::system_category()};
		}
#endif

		std::error_code canceled_error() noexcept
		{
			return std::make_error_code(std::errc::operation_canceled);
		}

		std::error_code invalid_argument_error() noexcept
		{
			return std::make_error_code(std::errc::invalid_argument);
		}

		std::error_code bad_alloc_error() noexcept
		{
			return std::make_error_code(std::errc::not_enough_memory);
		}

#ifdef _WIN32
		std::error_code last_error() noexcept
		{
			return {static_cast<int>(::GetLastError()), std::system_category()};
		}

		HANDLE to_handle(native_handle handle) noexcept
		{
			return reinterpret_cast<HANDLE>(handle);
		}
#else
		int to_fd(native_handle handle) noexcept
		{
			return static_cast<int>(handle);
		}
#endif

#ifndef _WIN32
		/// Runs @p call until it stops failing with EINTR: a signal is not a failure, only a reason to try again.
		template<typename Call>
		auto retry_on_eintr(Call call) noexcept
		{
			auto result = call();
			while(result < 0 && errno == EINTR)
			{
				result = call();
			}
			return result;
		}
#endif

		/// The byte count a positional transfer may move: never more than the buffer holds.
		std::size_t transfer_size(const io_request& request) noexcept
		{
			return std::min(request.size, request.buffer.size());
		}

		// ---------------------------------------------------------------------
		// Per-platform primitives
		// ---------------------------------------------------------------------

#ifdef _WIN32
		io_completion do_transfer(const io_request& request, bool is_write) noexcept
		{
			io_completion result;
			// ReadFile and WriteFile take a 32-bit count; a short count is a normal result.
			const auto count = static_cast<DWORD>(std::min<std::size_t>(transfer_size(request), std::numeric_limits<DWORD>::max()));
			OVERLAPPED position{};
			position.Offset     = static_cast<DWORD>(request.offset & 0xFFFFFFFFU);
			position.OffsetHigh = static_cast<DWORD>(request.offset >> 32U);
			DWORD       moved   = 0;
			void* const data    = request.buffer.bytes().data();
			const BOOL  ok      = is_write ? ::WriteFile(to_handle(request.handle), data, count, &moved, &position) : ::ReadFile(to_handle(request.handle), data, count, &moved, &position);
			if(ok == FALSE)
			{
				// Reading at or past the end is a short count of zero, not a failure.
				if(!is_write && ::GetLastError() == ERROR_HANDLE_EOF)
				{
					return result;
				}
				result.error = last_error();
				return result;
			}
			result.bytes = moved;
			return result;
		}

		std::error_code do_sync(native_handle handle) noexcept
		{
			// FlushFileBuffers has no data-only form, so sync_data degrades to a full flush.
			return ::FlushFileBuffers(to_handle(handle)) != FALSE ? std::error_code{} : last_error();
		}

		file_kind kind_of(DWORD attributes) noexcept
		{
			if((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				return file_kind::symlink;
			}
			if((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				return file_kind::directory;
			}
			return file_kind::regular;
		}

		std::error_code do_stat_handle(native_handle handle, file_status& out) noexcept
		{
			BY_HANDLE_FILE_INFORMATION info{};
			if(::GetFileInformationByHandle(to_handle(handle), &info) == FALSE)
			{
				return last_error();
			}
			constexpr std::int64_t windows_to_unix_100ns = 116444736000000000LL;
			const auto             ticks                 = (static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
			                   static_cast<std::int64_t>(info.ftLastWriteTime.dwLowDateTime);
			out.size            = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
			out.mtime_ns        = (ticks - windows_to_unix_100ns) * 100;
			out.kind            = kind_of(info.dwFileAttributes);
			out.identity.device = info.dwVolumeSerialNumber;
			out.identity.serial = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
			return {};
		}

		std::error_code do_open(const io_open_args& args, native_handle& out) noexcept
		{
			const auto how    = args.open_flags;
			const bool reads  = how.has_any(flags(open_flag::read));
			const bool writes = how.has_any(flags(open_flag::write));
			if(!reads && !writes)
			{
				return invalid_argument_error();
			}
			DWORD access = 0;
			if(reads)
			{
				access |= GENERIC_READ;
			}
			if(writes)
			{
				// Append-only access is what makes every write land at the end.
				access |= how.has_any(flags(open_flag::append)) ? static_cast<DWORD>(FILE_APPEND_DATA) : static_cast<DWORD>(GENERIC_WRITE);
			}
			const bool creates     = how.has_any(flags(open_flag::create));
			const bool truncates   = how.has_any(flags(open_flag::truncate));
			DWORD      disposition = OPEN_EXISTING;
			if(creates && how.has_any(flags(open_flag::exclusive)))
			{
				disposition = CREATE_NEW;
			}
			else if(creates)
			{
				disposition = truncates ? CREATE_ALWAYS : OPEN_ALWAYS;
			}
			else if(truncates)
			{
				disposition = TRUNCATE_EXISTING;
			}
			SECURITY_ATTRIBUTES security{};
			security.nLength        = static_cast<DWORD>(sizeof(security));
			security.bInheritHandle = how.has_any(flags(open_flag::close_on_exec)) ? FALSE : TRUE;
			// FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFileW open a directory at all.
			const HANDLE handle = ::CreateFileW(args.path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if(handle == INVALID_HANDLE_VALUE)
			{
				return last_error();
			}
			out = reinterpret_cast<native_handle>(handle);
			return {};
		}

		std::error_code do_stat_path(const std::filesystem::path& path, file_status& out) noexcept
		{
			const HANDLE handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if(handle == INVALID_HANDLE_VALUE)
			{
				return last_error();
			}
			const auto error = do_stat_handle(reinterpret_cast<native_handle>(handle), out);
			::CloseHandle(handle);
			return error;
		}

		std::error_code do_rename(const io_rename_args& args) noexcept
		{
			// REPLACE_EXISTING makes renaming over a file behave as it does on POSIX.
			return ::MoveFileExW(args.from.c_str(), args.to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE ? std::error_code{} : last_error();
		}

		std::error_code do_unlink(const std::filesystem::path& path) noexcept
		{
			return ::DeleteFileW(path.c_str()) != FALSE ? std::error_code{} : last_error();
		}
#else
		io_completion do_transfer(const io_request& request, bool is_write) noexcept
		{
			io_completion result;
			const auto    count  = transfer_size(request);
			const auto    offset = static_cast<off_t>(request.offset);
			void* const   data   = request.buffer.bytes().data();
			const ssize_t moved  = retry_on_eintr([&] { return is_write ? ::pwrite(to_fd(request.handle), data, count, offset) : ::pread(to_fd(request.handle), data, count, offset); });
			if(moved < 0)
			{
				result.error = errno_error();
				return result;
			}
			result.bytes = static_cast<std::size_t>(moved);
			return result;
		}

		std::error_code do_sync(native_handle handle, bool data_only) noexcept
		{
			const int rc = retry_on_eintr(
			  [&] {
#	ifdef __linux__
				  return data_only ? ::fdatasync(to_fd(handle)) : ::fsync(to_fd(handle));
#	else
				  static_cast<void>(data_only); // no fdatasync here: a full flush is the stronger guarantee
				  return ::fsync(to_fd(handle));
#	endif
			  });
			return rc == 0 ? std::error_code{} : errno_error();
		}

		file_kind kind_of(mode_t mode) noexcept
		{
			if(S_ISREG(mode))
			{
				return file_kind::regular;
			}
			if(S_ISDIR(mode))
			{
				return file_kind::directory;
			}
			if(S_ISLNK(mode))
			{
				return file_kind::symlink;
			}
			return file_kind::other;
		}

		void fill_status(const struct stat& info, file_status& out) noexcept
		{
#	ifdef __APPLE__
			const auto& mtime = info.st_mtimespec;
#	else
			const auto& mtime = info.st_mtim;
#	endif
			out.size            = static_cast<std::uint64_t>(info.st_size);
			out.mtime_ns        = (static_cast<std::int64_t>(mtime.tv_sec) * 1'000'000'000LL) + static_cast<std::int64_t>(mtime.tv_nsec);
			out.kind            = kind_of(info.st_mode);
			out.identity.device = static_cast<std::uint64_t>(info.st_dev);
			out.identity.serial = static_cast<std::uint64_t>(info.st_ino);
		}

		std::error_code do_stat_handle(native_handle handle, file_status& out) noexcept
		{
			struct stat info{};
			if(::fstat(to_fd(handle), &info) != 0)
			{
				return errno_error();
			}
			fill_status(info, out);
			return {};
		}

		std::error_code do_stat_path(const std::filesystem::path& path, file_status& out) noexcept
		{
			struct stat info{};
			if(::stat(path.c_str(), &info) != 0)
			{
				return errno_error();
			}
			fill_status(info, out);
			return {};
		}

		std::error_code do_open(const io_open_args& args, native_handle& out) noexcept
		{
			const auto how    = args.open_flags;
			const bool reads  = how.has_any(flags(open_flag::read));
			const bool writes = how.has_any(flags(open_flag::write));
			if(!reads && !writes)
			{
				return invalid_argument_error();
			}
			int native = O_RDONLY;
			if(reads && writes)
			{
				native = O_RDWR;
			}
			else if(writes)
			{
				native = O_WRONLY;
			}
			native |= how.has_any(flags(open_flag::create)) ? O_CREAT : 0;
			native |= how.has_any(flags(open_flag::exclusive)) ? O_EXCL : 0;
			native |= how.has_any(flags(open_flag::truncate)) ? O_TRUNC : 0;
			native |= how.has_any(flags(open_flag::append)) ? O_APPEND : 0;
			native |= how.has_any(flags(open_flag::close_on_exec)) ? O_CLOEXEC : 0;
			const int descriptor =
			  retry_on_eintr([&] { return ::open(args.path.c_str(), native, static_cast<mode_t>(args.mode.permissions)); });
			if(descriptor < 0)
			{
				return errno_error();
			}
			out = descriptor;
			return {};
		}

		std::error_code do_rename(const io_rename_args& args) noexcept
		{
			return ::rename(args.from.c_str(), args.to.c_str()) == 0 ? std::error_code{} : errno_error();
		}

		std::error_code do_unlink(const std::filesystem::path& path) noexcept
		{
			return ::unlink(path.c_str()) == 0 ? std::error_code{} : errno_error();
		}
#endif

		// ---------------------------------------------------------------------
		// Portable pieces
		// ---------------------------------------------------------------------

		file_kind kind_of(std::filesystem::file_type type) noexcept
		{
			switch(type)
			{
			case std::filesystem::file_type::regular:
				return file_kind::regular;
			case std::filesystem::file_type::directory:
				return file_kind::directory;
			case std::filesystem::file_type::symlink:
				return file_kind::symlink;
			case std::filesystem::file_type::none:
			case std::filesystem::file_type::not_found:
			case std::filesystem::file_type::unknown:
				return file_kind::unknown;
			default:
				return file_kind::other;
			}
		}

		/// Lists @p path into @p out, replacing its contents only on success. Never reports . or ..
		std::error_code do_list_directory(const std::filesystem::path& path, std::vector<directory_entry>& out)
		{
			std::error_code                           error;
			std::filesystem::directory_iterator       it(path, error);
			const std::filesystem::directory_iterator end;
			if(error)
			{
				return error;
			}
			std::vector<directory_entry> found;
			for(; it != end; it.increment(error))
			{
				std::error_code type_error;
				const auto      type = it->symlink_status(type_error).type();
				found.push_back(alt::directory_entry{.name = it->path().filename().string(), .kind = type_error ? file_kind::unknown : kind_of(type)});
			}
			if(error)
			{
				return error;
			}
			out = std::move(found);
			return {};
		}

		/// Performs one request synchronously on the calling (worker) thread.
		io_completion perform(const io_request& request)
		{
			// A request missing the pointer its operation reads would otherwise be a segfault on a worker.
			switch(request.op)
			{
			case io_op::stat_handle:
				assert(request.out != nullptr);
				break;
			case io_op::stat_path:
			case io_op::list_directory:
			case io_op::open:
				assert(request.in != nullptr && request.out != nullptr);
				break;
			case io_op::rename:
			case io_op::unlink:
				assert(request.in != nullptr);
				break;
			case io_op::invoke:
				assert(request.out != nullptr);
				break;
			default:
				break;
			}
			io_completion result;
			switch(request.op)
			{
			case io_op::read:
				return do_transfer(request, false);
			case io_op::write:
				return do_transfer(request, true);
#ifdef _WIN32
			case io_op::sync_full:
			case io_op::sync_data:
				result.error = do_sync(request.handle);
				break;
#else
			case io_op::sync_full:
				result.error = do_sync(request.handle, false);
				break;
			case io_op::sync_data:
				result.error = do_sync(request.handle, true);
				break;
#endif
			case io_op::stat_handle:
				result.error = do_stat_handle(request.handle, *static_cast<file_status*>(request.out));
				break;
			case io_op::stat_path:
				result.error = do_stat_path(*static_cast<const std::filesystem::path*>(request.in),
				                            *static_cast<file_status*>(request.out));
				break;
			case io_op::open:
				result.error = do_open(*static_cast<const io_open_args*>(request.in), *static_cast<native_handle*>(request.out));
				break;
			case io_op::rename:
				result.error = do_rename(*static_cast<const io_rename_args*>(request.in));
				break;
			case io_op::unlink:
				result.error = do_unlink(*static_cast<const std::filesystem::path*>(request.in));
				break;
			case io_op::list_directory:
				result.error = do_list_directory(*static_cast<const std::filesystem::path*>(request.in),
				                                 *static_cast<std::vector<directory_entry>*>(request.out));
				break;
			case io_op::invoke:
				// The callable's own exceptions are caught inside invoke(), for the awaiting
				// coroutine to rethrow; nothing here can escape from it.
				static_cast<detail::io_invocable*>(request.out)->invoke();
				break;
			}
			return result;
		}

		// ---------------------------------------------------------------------
		// The backend
		// ---------------------------------------------------------------------

		/// Which jobs are still queued, so cancel() can find one by token.
		class job_registry
		{
		public:
			void add(std::uint64_t id, std::atomic<bool>* cancelled)
			{
				const std::scoped_lock lock(m_mutex);
				m_queued.emplace(id, cancelled);
			}

			/// Removes @p id. A job calls this the moment it starts, which is what ends its cancellability.
			void remove(std::uint64_t id) noexcept
			{
				const std::scoped_lock lock(m_mutex);
				m_queued.erase(id);
			}

			/// Flags the job if it has not started. A job that has started is not in the map.
			void cancel(std::uint64_t id) noexcept
			{
				const std::scoped_lock lock(m_mutex);
				if(const auto found = m_queued.find(id); found != m_queued.end())
				{
					found->second->store(true);
				}
			}

		private:
			std::mutex                                            m_mutex;
			std::unordered_map<std::uint64_t, std::atomic<bool>*> m_queued;
		};

		/// One submission: a private copy of the request and everything needed to finish it.
		struct job
		{
			io_request                    request;
			io_completion_fn              on_done = nullptr;
			void*                         state   = nullptr;
			std::uint64_t                 id      = 0;
			std::atomic<bool>             cancelled{false};
			std::shared_ptr<job_registry> registry;
			std::shared_ptr<job>          self; ///< The job's hold on itself while queued; see run_job().
		};

		void run_job(void* raw) noexcept
		{
			// A queued job holds a shared_ptr to itself, so the queue item needs no owner of its
			// own. Taking that reference here means a cancelled job's record and its io_buffer
			// copy are destroyed on this worker, never under the thread that submitted it.
			auto&                      queued = *static_cast<job*>(raw);
			const std::shared_ptr<job> keep   = std::move(queued.self);
			job&                       self   = *keep;

			self.registry->remove(self.id); // from here on a cancel finds nothing: the job is running
			io_completion result;
			if(self.cancelled.load())
			{
				if(self.request.op == io_op::invoke)
				{
					// Destroy the callable HERE, on this worker, with its captures: dropping it
					// unrun is a behavior consumers rely on, and left to the block it would die
					// on whichever thread released the last reference.
					static_cast<detail::io_invocable*>(self.request.out)->discard();
				}
				result.error = canceled_error();
			}
			else
			{
				try
				{
					result = perform(self.request);
				}
				catch(const std::bad_alloc&)
				{
					result.error = bad_alloc_error();
				}
				catch(...)
				{
					// This function is noexcept and a worker has nobody to hand an exception to,
					// so one would abort the process. The reachable source is a directory entry
					// whose name cannot be converted to a narrow string (Windows), which is
					// filesystem content the caller does not control. Report it instead.
					result.error = std::make_error_code(std::errc::io_error);
				}
			}
			self.on_done(self.state, result);
		}

		class thread_pool_backend final: public io_backend
		{
		public:
			explicit thread_pool_backend(thread_pool& pool): m_pool(pool), m_registry(std::make_shared<job_registry>())
			{}

			[[nodiscard]] std::expected<io_token, std::error_code> submit(const io_request& request, io_completion_fn on_done, void* state) override
			{
				std::shared_ptr<job> record;
				try
				{
					record           = std::make_shared<job>();
					record->request  = request;
					record->on_done  = on_done;
					record->state    = state;
					record->id       = m_next_id.fetch_add(1);
					record->registry = m_registry;
					record->self     = record;
				}
				catch(const std::bad_alloc&)
				{
					return std::unexpected(bad_alloc_error());
				}

				const std::uint64_t id = record->id;
				try
				{
					m_registry->add(id, &record->cancelled);
					m_pool.post(work_item{.invoke = run_job, .state = record.get()});
				}
				catch(const std::bad_alloc&)
				{
					m_registry->remove(id);
					record->self.reset();
					return std::unexpected(bad_alloc_error());
				}
				return io_token(id);
			}

			void cancel(io_token token) noexcept override
			{
				if(token)
				{
					m_registry->cancel(token.value());
				}
			}

			[[nodiscard]] io_backend_kind kind() const noexcept override
			{
				return io_backend_kind::thread_pool;
			}

		private:
			thread_pool&                  m_pool;
			std::shared_ptr<job_registry> m_registry;
			std::atomic<std::uint64_t>    m_next_id{1};
		};

	} // namespace

	std::unique_ptr<io_backend> make_thread_pool_backend(thread_pool& pool)
	{
		return std::make_unique<thread_pool_backend>(pool);
	}

} // namespace alt

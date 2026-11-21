#include <gtest/gtest.h>

#include <algorithm>
#include <alt/detail/file_types.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/thread_pool.hpp>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

#ifdef _WIN32
// windows.h defines min and max as function-like macros; NOMINMAX is its documented switch to leave them out.
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <fcntl.h>
#	include <sys/stat.h>
#	include <sys/syscall.h>
#	include <unistd.h>
#endif

namespace
{
	/// Records what a completion callback saw, and how many times it ran.
	struct recorder
	{
		int                calls = 0;
		alt::io_completion last{};
	};

	void record(void* state, alt::io_completion result) noexcept
	{
		auto& rec = *static_cast<recorder*>(state);
		++rec.calls;
		rec.last = result;
	}

	alt::io_completion done(std::size_t bytes, std::error_code error = {})
	{
		alt::io_completion result;
		result.bytes = bytes;
		result.error = error;
		return result;
	}

	alt::io_request read_request()
	{
		alt::io_request request;
		request.op     = alt::io_op::read;
		request.handle = 7;
		request.offset = 128;
		request.size   = 64;
		return request;
	}
} // namespace

TEST(IoBackendTest, ASubmittedOperationIsPendingUntilCompleted)
{
	alt::manual_io_backend backend;
	recorder               rec;

	EXPECT_EQ(backend.pending(), 0U);
	const auto token = backend.submit(read_request(), record, &rec);

	ASSERT_TRUE(token.has_value());
	EXPECT_TRUE(static_cast<bool>(*token));
	EXPECT_EQ(backend.pending(), 1U);
	EXPECT_EQ(rec.calls, 0);
	EXPECT_EQ(backend.request(0).op, alt::io_op::read);
	EXPECT_EQ(backend.request(0).handle, 7);
	EXPECT_EQ(backend.request(0).offset, 128U);

	backend.complete(0, done(1));
	EXPECT_EQ(backend.pending(), 0U);
}

TEST(IoBackendTest, CompletingRunsTheCallbackExactlyOnceWithTheGivenResult)
{
	alt::manual_io_backend backend;
	recorder               first;
	recorder               second;
	ASSERT_TRUE(backend.submit(read_request(), record, &first).has_value());
	ASSERT_TRUE(backend.submit(read_request(), record, &second).has_value());

	const auto failure = std::make_error_code(std::errc::io_error);
	backend.complete(1, done(0, failure));

	EXPECT_EQ(first.calls, 0);
	EXPECT_EQ(second.calls, 1);
	EXPECT_EQ(second.last.error, failure);
	EXPECT_EQ(backend.pending(), 1U);

	backend.complete(0, done(42));
	EXPECT_EQ(first.calls, 1);
	EXPECT_EQ(first.last.bytes, 42U);
	EXPECT_FALSE(first.last.error);
	EXPECT_EQ(second.calls, 1);
}

TEST(IoBackendTest, CompleteInlineNextRunsTheCallbackBeforeSubmitReturns)
{
	alt::manual_io_backend backend;
	recorder               rec;

	backend.complete_inline_next(done(9));
	const auto token = backend.submit(read_request(), record, &rec);

	ASSERT_TRUE(token.has_value());
	EXPECT_EQ(rec.calls, 1);
	EXPECT_EQ(rec.last.bytes, 9U);
	EXPECT_EQ(backend.pending(), 0U);

	// The arm is consumed: the next submission is an ordinary pending one.
	recorder later;
	ASSERT_TRUE(backend.submit(read_request(), record, &later).has_value());
	EXPECT_EQ(later.calls, 0);
	EXPECT_EQ(backend.pending(), 1U);
}

TEST(IoBackendTest, CancelIsRecordedButTheCallbackStillRunsExactlyOnce)
{
	alt::manual_io_backend backend;
	recorder               rec;
	const auto             token = backend.submit(read_request(), record, &rec);
	ASSERT_TRUE(token.has_value());

	EXPECT_FALSE(backend.was_cancelled(0));
	backend.cancel(*token);

	EXPECT_TRUE(backend.was_cancelled(0));
	EXPECT_EQ(rec.calls, 0);
	EXPECT_EQ(backend.pending(), 1U);

	backend.complete(0, done(0, std::make_error_code(std::errc::operation_canceled)));
	EXPECT_EQ(rec.calls, 1);
	EXPECT_EQ(rec.last.error, std::make_error_code(std::errc::operation_canceled));
	EXPECT_TRUE(backend.was_cancelled(0));
}

TEST(IoBackendTest, CancelThenCompleteWithSuccessStillDeliversSuccess)
{
	alt::manual_io_backend backend;
	recorder               rec;
	const auto             token = backend.submit(read_request(), record, &rec);
	ASSERT_TRUE(token.has_value());

	backend.cancel(*token);
	backend.complete(0, done(64));

	EXPECT_EQ(rec.calls, 1);
	EXPECT_EQ(rec.last.bytes, 64U);
	EXPECT_FALSE(rec.last.error);
}

TEST(IoBackendTest, TokensCompareByValue)
{
	const alt::io_token seven{7};

	EXPECT_EQ(seven, alt::io_token{7});
	EXPECT_NE(seven, alt::io_token{8});
	EXPECT_NE(seven, alt::io_token{});
	EXPECT_EQ(alt::io_token{}, alt::io_token{});
}

TEST(IoBackendTest, ManualBackendReportsItsKindAsManual)
{
	const alt::manual_io_backend backend;
	const alt::io_backend&       seam = backend;

	EXPECT_EQ(seam.kind(), alt::io_backend_kind::manual);
}

TEST(IoBackendTest, ASubmissionConfiguredToFailReturnsAnErrorAndNeverRunsTheCallback)
{
	alt::manual_io_backend backend;
	recorder               rec;
	const auto             failure = std::make_error_code(std::errc::resource_unavailable_try_again);

	backend.fail_next_submission(failure);
	const auto rejected = backend.submit(read_request(), record, &rec);

	ASSERT_FALSE(rejected.has_value());
	EXPECT_EQ(rejected.error(), failure);
	EXPECT_EQ(rec.calls, 0);
	EXPECT_EQ(backend.pending(), 0U);

	// The arm is consumed: the next submission is accepted.
	EXPECT_TRUE(backend.submit(read_request(), record, &rec).has_value());
	EXPECT_EQ(backend.pending(), 1U);
	backend.complete(0, {});
	EXPECT_EQ(rec.calls, 1);
}

namespace
{
	/// Observes, from inside a completion callback, whether the buffer's owner is still alive.
	struct liveness_probe
	{
		std::atomic<bool> freed{false};
		int               calls                 = 0;
		bool              freed_during_callback = true;
	};

	void probe_callback(void* state, alt::io_completion) noexcept
	{
		auto& probe = *static_cast<liveness_probe*>(state);
		++probe.calls;
		probe.freed_during_callback = probe.freed.load();
	}
} // namespace

TEST(IoBackendTest, TheBuffersStorageIsStillAliveInsideTheCompletionCallback)
{
	alt::manual_io_backend    backend;
	liveness_probe            probe;
	std::array<std::byte, 16> storage{};
	{
		const std::shared_ptr<void> owner(storage.data(), [&probe](void*) { probe.freed = true; });
		alt::io_request             request = read_request();
		request.buffer                      = alt::io_buffer::borrow(storage, owner);
		ASSERT_TRUE(backend.submit(request, probe_callback, &probe).has_value());
	}
	// The caller's own copies are gone; only the backend's entry keeps the owner alive.
	EXPECT_FALSE(probe.freed);

	backend.complete(0, done(16));

	EXPECT_EQ(probe.calls, 1);
	EXPECT_FALSE(probe.freed_during_callback);
	EXPECT_TRUE(probe.freed) << "the backend must release its copy once the callback returned";
}

namespace
{
	void count_callback(void* state, alt::io_completion) noexcept
	{
		static_cast<std::atomic<int>*>(state)->fetch_add(1);
	}
} // namespace

namespace
{
	constexpr std::size_t completers             = 3;
	constexpr std::size_t per_completer          = 400;
	constexpr std::size_t preloaded              = completers * per_completer;
	constexpr std::size_t late                   = 800;
	constexpr std::size_t contending_party_count = completers + 2;

	void complete_slice(alt::manual_io_backend& backend, std::atomic<std::size_t>& arrived, std::size_t slice)
	{
		alt_test::rendezvous(arrived, contending_party_count);
		for(std::size_t i = slice * per_completer; i < (slice + 1) * per_completer; ++i)
		{
			backend.complete(i, done(i));
			(void)backend.was_cancelled(i);
		}
	}

	void submit_late(alt::manual_io_backend& backend, std::atomic<std::size_t>& arrived, std::atomic<int>* counts)
	{
		alt_test::rendezvous(arrived, contending_party_count);
		for(std::size_t i = preloaded; i < preloaded + late; ++i)
		{
			const auto token = backend.submit(read_request(), count_callback, &counts[i]);
			EXPECT_TRUE(token.has_value());
			(void)backend.pending();
		}
	}

	void cancel_preloaded(alt::manual_io_backend& backend, std::atomic<std::size_t>& arrived)
	{
		alt_test::rendezvous(arrived, contending_party_count);
		for(std::size_t i = 0; i < preloaded; ++i)
		{
			backend.cancel(alt::io_token(i + 1));
		}
	}
} // namespace

// Completers, a submitter and a canceller are all in flight at once: the spin
// rendezvous releases them within nanoseconds of each other, and each then works
// through hundreds of operations, so complete(), submit(), pending(), cancel() and
// was_cancelled() overlap on the same mutex. ThreadSanitizer is the judge of the
// locking; the exactly-once counts are the judge of the bookkeeping.
TEST(IoBackendTest, CompletingWhileSubmittingAndCancellingFromOtherThreadsRunsEveryCallbackExactlyOnce)
{
	alt::manual_io_backend backend;
	const auto             counts = std::make_unique<std::atomic<int>[]>(preloaded + late);
	for(std::size_t i = 0; i < preloaded; ++i)
	{
		ASSERT_TRUE(backend.submit(read_request(), count_callback, &counts[i]).has_value());
	}

	std::atomic<std::size_t> arrived{0};
	{
		std::vector<std::jthread> threads(contending_party_count);
		for(std::size_t c = 0; c < completers; ++c)
		{
			threads[c] = std::jthread(complete_slice, std::ref(backend), std::ref(arrived), c);
		}
		threads[completers]     = std::jthread(submit_late, std::ref(backend), std::ref(arrived), counts.get());
		threads[completers + 1] = std::jthread(cancel_preloaded, std::ref(backend), std::ref(arrived));
	}

	EXPECT_EQ(backend.pending(), late);
	for(std::size_t i = preloaded; i < preloaded + late; ++i)
	{
		backend.complete(i, done(i));
	}
	EXPECT_EQ(backend.pending(), 0U);
	for(std::size_t i = 0; i < preloaded + late; ++i)
	{
		EXPECT_EQ(counts[i].load(), 1) << "operation " << i;
	}
}

TEST(IoBackendTest, ARejectedSubmissionAlsoConsumesTheInlineArm)
{
	alt::manual_io_backend backend;
	recorder               rec;

	backend.complete_inline_next(done(1));
	backend.fail_next_submission(std::make_error_code(std::errc::io_error));
	EXPECT_FALSE(backend.submit(read_request(), record, &rec).has_value());

	ASSERT_TRUE(backend.submit(read_request(), record, &rec).has_value());
	EXPECT_EQ(rec.calls, 0);
	EXPECT_EQ(backend.pending(), 1U);
}

// ---------------------------------------------------------------------------
// The thread-pool backend
// ---------------------------------------------------------------------------

namespace
{
	/// Collects one completion and lets a test wait for it from another thread.
	struct waiter
	{
		std::binary_semaphore done{0};
		alt::io_completion    result{};
		std::atomic<int>      calls{0};
	};

	void deliver(void* state, alt::io_completion result) noexcept
	{
		auto& w  = *static_cast<waiter*>(state);
		w.result = result;
		w.calls.fetch_add(1);
		w.done.release();
	}

	/// Blocks a pool thread until the test lets it go.
	struct blocker
	{
		std::binary_semaphore entered{0};
		std::binary_semaphore release{0};
	};

	void block(void* state) noexcept
	{
		auto& b = *static_cast<blocker*>(state);
		b.entered.release();
		b.release.acquire();
	}

	class ThreadPoolBackendTest: public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<int> counter{0};
			m_dir.path = std::filesystem::temp_directory_path() /
			             ("alt_io_backend_" + std::to_string(counter.fetch_add(1)) + "_" +
			              std::to_string(reinterpret_cast<std::uintptr_t>(this)));
			std::filesystem::create_directories(m_dir.path);
			ASSERT_TRUE(m_pool.has_value());
		}

		void TearDown() override
		{
			// The directory is removed by m_dir's destructor, which runs after the pool has
			// been joined: no worker can still be touching a file in it.
			m_backend.reset();
		}

		[[nodiscard]] const std::filesystem::path& dir() const
		{
			return m_dir.path;
		}

		alt::thread_pool& pool()
		{
			return *m_pool;
		}

		alt::io_backend& backend()
		{
			if(!m_backend)
			{
				m_backend = alt::make_thread_pool_backend(*m_pool);
			}
			return *m_backend;
		}

		/// Submits and waits. The request must not need a result beyond the completion.
		alt::io_completion run(const alt::io_request& request)
		{
			waiter     w;
			const auto token = backend().submit(request, deliver, &w);
			EXPECT_TRUE(token.has_value());
			if(!token)
			{
				return alt::io_completion{.bytes = 0, .error = token.error()};
			}
			w.done.acquire();
			EXPECT_EQ(w.calls.load(), 1);
			return w.result;
		}

		/// Opens @p path through the backend, so every test also exercises io_op::open.
		std::expected<std::shared_ptr<alt::detail::io_handle>, std::error_code> open_file(const std::filesystem::path& path,
		                                                                                  alt::flags<alt::open_flag>   how)
		{
			alt::io_open_args  args{.path = path, .open_flags = how, .mode = {}};
			alt::native_handle handle = alt::invalid_native_handle;
			alt::io_request    request;
			request.op      = alt::io_op::open;
			request.in      = &args;
			request.out     = &handle;
			const auto done = run(request);
			if(done.error)
			{
				return std::unexpected(done.error);
			}
			return alt::detail::io_handle::adopt(handle);
		}

		std::filesystem::path write_file(const std::string& name, const std::string& content) const
		{
			const auto path = m_dir.path / name;
			std::ofstream(path, std::ios::binary) << content;
			return path;
		}

		static std::string read_whole(const std::filesystem::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		}

		static alt::io_buffer buffer_of(std::size_t size, std::byte fill = std::byte{0})
		{
			auto buffer = alt::io_buffer::allocate(size);
			EXPECT_TRUE(buffer.has_value());
			std::memset(buffer->bytes().data(), static_cast<int>(fill), size);
			return std::move(*buffer);
		}

	private:
		// m_dir is declared before the pool so it is removed after the pool has joined.
		struct scratch_dir
		{
			std::filesystem::path path;

			~scratch_dir()
			{
				std::error_code ignored;
				std::filesystem::remove_all(path, ignored);
			}
		};

		scratch_dir                                      m_dir;
		std::expected<alt::thread_pool, std::error_code> m_pool = alt::thread_pool::create(1);
		std::unique_ptr<alt::io_backend>                 m_backend;
	};

	alt::io_request positional(alt::io_op op, const alt::detail::io_handle& file, alt::io_buffer buffer, std::uint64_t offset, std::size_t size)
	{
		alt::io_request request;
		request.op     = op;
		request.handle = file.native();
		request.buffer = std::move(buffer);
		request.offset = offset;
		request.size   = size;
		return request;
	}

	std::string text_of(const alt::io_buffer& buffer, std::size_t count)
	{
		return {reinterpret_cast<const char*>(buffer.bytes().data()), count};
	}
} // namespace

TEST_F(ThreadPoolBackendTest, AReadAtReturnsTheFilesBytes)
{
	const auto path = write_file("data.txt", "hello, backend");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value());

	auto       buffer = buffer_of(5);
	const auto copy   = buffer;
	const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 7, 5));

	EXPECT_FALSE(done.error);
	EXPECT_EQ(done.bytes, 5U);
	EXPECT_EQ(text_of(copy, 5), "backe");
}

TEST_F(ThreadPoolBackendTest, AWriteAtPutsBytesAtTheOffset)
{
	const auto path = write_file("data.txt", "0123456789");
	auto       file = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value());

	auto buffer = buffer_of(3);
	std::memcpy(buffer.bytes().data(), "abc", 3);
	const auto done = run(positional(alt::io_op::write, **file, std::move(buffer), 4, 3));

	EXPECT_FALSE(done.error);
	EXPECT_EQ(done.bytes, 3U);
	EXPECT_EQ(read_whole(path), "0123abc789");
}

TEST_F(ThreadPoolBackendTest, AReadPastTheEndReturnsAShortCount)
{
	const auto path = write_file("data.txt", "short");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value());

	const auto partial = run(positional(alt::io_op::read, **file, buffer_of(16), 2, 16));
	EXPECT_FALSE(partial.error);
	EXPECT_EQ(partial.bytes, 3U);

	const auto beyond = run(positional(alt::io_op::read, **file, buffer_of(16), 100, 16));
	EXPECT_FALSE(beyond.error);
	EXPECT_EQ(beyond.bytes, 0U);
}

TEST_F(ThreadPoolBackendTest, AnOpenWithNeitherAccessBitFailsInsteadOfOpeningReadOnly)
{
	// The backend is a public seam, so it keeps the check the service performs first.
	const auto path = write_file("data.txt", "x");

	const auto none        = open_file(path, alt::flags<alt::open_flag>{});
	const auto create_only = open_file(path, alt::flags(alt::open_flag::create));

	ASSERT_FALSE(none.has_value());
	EXPECT_EQ(none.error(), std::errc::invalid_argument);
	ASSERT_FALSE(create_only.has_value());
	EXPECT_EQ(create_only.error(), std::errc::invalid_argument);
}

TEST_F(ThreadPoolBackendTest, AReadOfAMissingPathReportsNoSuchFileOrDirectory)
{
	const auto file = open_file(dir() / "missing.txt", alt::flags(alt::open_flag::read));

	ASSERT_FALSE(file.has_value());
	EXPECT_EQ(file.error(), std::errc::no_such_file_or_directory);
}

TEST_F(ThreadPoolBackendTest, AJobCancelledBeforeItStartedNeverTouchesTheFile)
{
	const auto path = write_file("data.txt", "");
	auto       file = open_file(path, alt::flags(alt::open_flag::write));
	ASSERT_TRUE(file.has_value());

	// Occupy the pool's only thread so the write stays queued behind it.
	blocker gate;
	pool().post(alt::work_item{.invoke = block, .state = &gate});
	gate.entered.acquire();

	auto buffer = buffer_of(4);
	std::memcpy(buffer.bytes().data(), "data", 4);
	waiter     w;
	const auto token = backend().submit(positional(alt::io_op::write, **file, std::move(buffer), 0, 4), deliver, &w);
	ASSERT_TRUE(token.has_value());
	backend().cancel(*token);
	gate.release.release();
	w.done.acquire();

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_EQ(w.result.error, std::errc::operation_canceled);
	EXPECT_EQ(w.result.bytes, 0U);
	EXPECT_EQ(read_whole(path), "") << "the cancelled write reached the file";
}

namespace
{
	constexpr auto mid_job_wait_budget = std::chrono::seconds(10);

	/// Retries @p ready every millisecond until it holds or @p budget is spent.
	template<typename Ready>
	bool wait_until(Ready ready, std::chrono::steady_clock::duration budget = mid_job_wait_budget)
	{
		const auto give_up = std::chrono::steady_clock::now() + budget;
		while(!ready())
		{
			if(std::chrono::steady_clock::now() >= give_up)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}
} // namespace

#ifdef _WIN32
namespace
{
	/// Closes a Win32 handle on scope exit; a null handle is left alone.
	struct scoped_handle
	{
		HANDLE value = nullptr;

		scoped_handle() = default;
		explicit scoped_handle(HANDLE owned): value(owned)
		{}
		scoped_handle(const scoped_handle&)            = delete;
		scoped_handle& operator=(const scoped_handle&) = delete;
		scoped_handle(scoped_handle&& other) noexcept: value(std::exchange(other.value, nullptr))
		{}
		scoped_handle& operator=(scoped_handle&&) = delete;

		~scoped_handle()
		{
			reset();
		}

		void reset() noexcept
		{
			if(value != nullptr)
			{
				static_cast<void>(::CloseHandle(value));
				value = nullptr;
			}
		}
	};

	struct threads_probe
	{
		explicit threads_probe(std::ptrdiff_t count): arrived(count), handles(static_cast<std::size_t>(count), nullptr)
		{}

		std::latch          arrived;
		std::atomic<int>    next{0};
		std::vector<HANDLE> handles;
	};

	/// Handles to @p count distinct pool workers that GetThreadIOPendingFlag can query, learned by
	/// running one job on each: every job waits until all have arrived, so no thread runs two.
	/// @p count must not exceed the pool's thread count, or this never returns.
	///
	/// The probe is shared, not on this frame: wait() can return while a worker is still inside
	/// arrive_and_wait(), and a latch destroyed under it leaves that worker parked for good on
	/// whatever the reused memory holds, one pool thread short for the rest of the test.
	std::vector<scoped_handle> worker_threads(alt::thread_pool& pool, std::ptrdiff_t count)
	{
		const auto probe = std::make_shared<threads_probe>(count);
		for(std::ptrdiff_t i = 0; i < count; ++i)
		{
			pool.post(alt::work_item{.invoke = +[](void* state) noexcept {
				                         const std::unique_ptr<std::shared_ptr<threads_probe>> owner(static_cast<std::shared_ptr<threads_probe>*>(state));
				                         auto&                                                 p    = **owner;
				                         const auto                                            slot = static_cast<std::size_t>(p.next.fetch_add(1));
				                         // GetCurrentThread() is a pseudo-handle that means "the caller" on
				                         // every thread; duplicating it yields a real handle to this worker.
				                         // GetThreadIOPendingFlag needs the full query right: with
				                         // THREAD_QUERY_LIMITED_INFORMATION it fails with ERROR_ACCESS_DENIED.
				                         if(::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &p.handles[slot], THREAD_QUERY_INFORMATION, FALSE, 0) == FALSE)
				                         {
					                         p.handles[slot] = nullptr;
				                         }
				                         p.arrived.arrive_and_wait();
			                         },
			                         .state = new std::shared_ptr<threads_probe>(probe)});
		}
		probe->arrived.wait();
		std::vector<scoped_handle> workers;
		workers.reserve(probe->handles.size());
		for(HANDLE handle: probe->handles)
		{
			workers.emplace_back(handle);
		}
		return workers;
	}

	/// A handle to the pool's only worker that GetThreadIOPendingFlag can query.
	scoped_handle worker_thread(alt::thread_pool& pool)
	{
		auto workers = worker_threads(pool, 1);
		return std::move(workers.front());
	}

	/// Whether @p thread has an I/O request outstanding: Windows' counterpart of reading wchan.
	/// A thread blocked in a synchronous ReadFile has its request queued and reports TRUE.
	/// Empty when the query itself fails, which must never read as "idle".
	std::optional<bool> has_pending_io(HANDLE thread)
	{
		BOOL pending = FALSE;
		if(::GetThreadIOPendingFlag(thread, &pending) == FALSE)
		{
			return std::nullopt;
		}
		return pending != FALSE;
	}

	/// Frees a worker still blocked in ReadFile when a failed assertion returns early, so the
	/// fixture's pool join cannot hang on it: closing the write end fails the read with
	/// ERROR_BROKEN_PIPE. Does nothing once the test saw the completion.
	struct pipe_releaser
	{
		scoped_handle& writer;
		waiter&        job;
		bool           completed = false;

		pipe_releaser(scoped_handle& write_end, waiter& pending): writer(write_end), job(pending)
		{}
		pipe_releaser(const pipe_releaser&)            = delete;
		pipe_releaser& operator=(const pipe_releaser&) = delete;

		~pipe_releaser()
		{
			if(!completed)
			{
				writer.reset();
				job.done.acquire();
			}
		}
	};
} // namespace
#else
namespace
{
	/// Frees a reader still blocked in open() when a failed assertion returns early, so the
	/// fixture's pool join cannot hang on it. Does nothing once the test saw the completion.
	struct fifo_releaser
	{
		std::filesystem::path path;
		waiter&               job;
		bool                  completed = false;

		fifo_releaser(std::filesystem::path fifo_path, waiter& pending): path(std::move(fifo_path)), job(pending)
		{}

		fifo_releaser(const fifo_releaser&)            = delete;
		fifo_releaser& operator=(const fifo_releaser&) = delete;

		~fifo_releaser()
		{
			while(!completed && !job.done.try_acquire_for(std::chrono::milliseconds(1)))
			{
				const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
				if(writer >= 0)
				{
					static_cast<void>(::close(writer));
				}
			}
		}
	};

#	ifdef __linux__
	struct tid_probe
	{
		std::binary_semaphore done{0};
		pid_t                 tid = 0;
	};

	/// The kernel thread id of the pool's only worker, learned by running a job on it.
	pid_t worker_tid(alt::thread_pool& pool)
	{
		tid_probe probe;
		pool.post(alt::work_item{.invoke = +[](void* state) noexcept {
			                         auto& p = *static_cast<tid_probe*>(state);
			                         p.tid   = static_cast<pid_t>(::syscall(SYS_gettid));
			                         p.done.release();
		                         },
		                         .state = &probe});
		probe.done.acquire();
		return probe.tid;
	}

	/// Where the kernel says @p tid is sleeping; empty when the file cannot be opened.
	std::optional<std::string> wchan_of(pid_t tid)
	{
		std::ifstream wchan("/proc/self/task/" + std::to_string(tid) + "/wchan");
		if(!wchan)
		{
			return std::nullopt;
		}
		std::string where;
		wchan >> where;
		return where;
	}

	/// A readable wchan names a kernel symbol. Hardened /proc and CONFIG_KALLSYMS=n give "0" or an address.
	bool names_a_symbol(const std::string& where)
	{
		return std::ranges::any_of(where, [](unsigned char c) { return std::isalpha(c) != 0 && c != 'x'; });
	}
#	endif

	/// The pool worker's thread id when this environment reports where it sleeps, else empty.
	std::optional<pid_t> worker_to_watch([[maybe_unused]] alt::thread_pool& pool)
	{
#	ifdef __linux__
		const pid_t worker = worker_tid(pool);
		const auto  idle   = wchan_of(worker);
		if(idle.has_value() && names_a_symbol(*idle))
		{
			return worker;
		}
#	endif
		std::cout << "[  NOTE  ] this environment does not report the worker's wchan; using the non-blocking-writer probe\n";
		return std::nullopt;
	}

	/// Cancels @p token once the job is provably past its start, then returns the FIFO's write end, which
	/// releases the blocked open. With a @p worker it waits for that thread to sleep inside open(); without
	/// one, or if it never shows the expected symbol, it probes with non-blocking writer opens until a
	/// reader exists. A probe that finds none is reported through EXPECT and returns -1.
	int cancel_once_started(alt::io_backend& backend, alt::io_token token, const std::filesystem::path& fifo, std::optional<pid_t> worker)
	{
		int writer = -1;
#	ifdef __linux__
		if(worker)
		{
			// A worker really inside open() gets there in milliseconds, so a short budget: the wait only
			// runs to its end on a kernel that names the symbol differently, or on a runner so loaded
			// (TSan, say) that the worker is slow to arrive. Either way the fallback is a weaker check, not a failure.
			const bool inside_open = wait_until([&] { return wchan_of(*worker).value_or("") == "wait_for_partner"; }, std::chrono::seconds(2));
			if(inside_open)
			{
				backend.cancel(token);
				return ::open(fifo.c_str(), O_WRONLY); // releases the blocked reader
			}
			// wchan names symbols here but never one this test knows: a kernel that renamed or
			// inlined wait_for_partner is not a broken worker, so degrade to the portable probe
			// (a weaker window, not a wrong one) and say so rather than failing.
			std::cout << "[  NOTE  ] the worker's wchan never read wait_for_partner; using the non-blocking-writer probe\n";
			::testing::Test::RecordProperty("cancel_probe", "non-blocking-writer: wchan never showed wait_for_partner");
		}
#	endif
		static_cast<void>(worker);
		const bool reader_appeared = wait_until(
		  [&] {
			  writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
			  return writer >= 0;
		  });
		EXPECT_TRUE(reader_appeared) << "no reader ever appeared on the FIFO; the job never started";
		backend.cancel(token);
		return writer;
	}
} // namespace
#endif

TEST_F(ThreadPoolBackendTest, AJobCancelledAfterItStartedStillCompletesAndReportsItsResult)
{
#ifdef _WIN32
	// A read on a pipe nobody has written to blocks inside ReadFile until data arrives. That
	// holds the job in the middle of its I/O, which no regular-file operation can do, so the
	// cancel lands strictly after the job started and strictly before it finished.
	HANDLE read_end  = nullptr;
	HANDLE write_end = nullptr;
	ASSERT_NE(::CreatePipe(&read_end, &write_end, nullptr, 0), FALSE) << "CreatePipe failed: " << ::GetLastError();
	scoped_handle reader(read_end);
	scoped_handle writer(write_end);

	// GetThreadIOPendingFlag plays wchan's role: it reports whether the worker has an I/O
	// request outstanding. It must read FALSE while the worker is idle, or a TRUE later would
	// not prove the job is inside its read.
	const auto worker = worker_thread(pool());
	ASSERT_NE(worker.value, nullptr) << "could not obtain a handle to the pool's worker";
	const auto idle = has_pending_io(worker.value);
	ASSERT_TRUE(idle.has_value()) << "GetThreadIOPendingFlag failed: " << ::GetLastError();
	ASSERT_FALSE(*idle) << "the idle worker already reports pending I/O; the probe cannot tell anything apart";

	constexpr std::string_view payload = "mid-job";
	alt::io_request            request;
	request.op                = alt::io_op::read;
	request.handle            = reinterpret_cast<alt::native_handle>(reader.value);
	request.buffer            = buffer_of(payload.size());
	request.size              = payload.size();
	const alt::io_buffer view = request.buffer; // the job holds its own copy; this one reads the bytes back

	waiter     w;
	const auto token = backend().submit(request, deliver, &w);
	ASSERT_TRUE(token.has_value());
	pipe_releaser releaser(writer, w);

	ASSERT_TRUE(wait_until([&] { return has_pending_io(worker.value).value_or(false); }))
	  << "the worker never blocked in its read; the job never started";
	backend().cancel(*token);
	DWORD written = 0;
	ASSERT_NE(::WriteFile(writer.value, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr), FALSE);
	ASSERT_EQ(written, payload.size());
	w.done.acquire();
	releaser.completed = true;

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << "a cancel after the job started changed its outcome: " << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size()) << "the read completed but its byte count was discarded";
	EXPECT_EQ(text_of(view, payload.size()), payload) << "the read completed but its data never reached the buffer";
#else
	// Opening a FIFO for reading blocks inside the syscall until a writer appears. That
	// holds the job in the middle of its I/O, which no regular-file operation can do, so
	// the cancel lands strictly after the job started and strictly before it finished.
	const auto fifo = dir() / "gate";
	ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

	// Where the kernel can say what the worker is waiting in (Linux, readable wchan), the
	// test watches for it to be inside open(). Elsewhere the fallback is a non-blocking
	// writer open, which fails with ENXIO until a reader exists: it proves the job started,
	// but it releases the reader, so the cancel then races the completion. That is a weaker
	// window, not a wrong one; a correct backend reports success either way.
	const auto watched = worker_to_watch(pool());

	alt::io_open_args  args{.path = fifo, .open_flags = alt::flags(alt::open_flag::read), .mode = {}};
	alt::native_handle handle = alt::invalid_native_handle;
	alt::io_request    request;
	request.op  = alt::io_op::open;
	request.in  = &args;
	request.out = &handle;

	waiter     w;
	const auto token = backend().submit(request, deliver, &w);
	ASSERT_TRUE(token.has_value());
	fifo_releaser releaser(fifo, w);

	const int writer = cancel_once_started(backend(), *token, fifo, watched);
	ASSERT_GE(writer, 0);
	w.done.acquire();
	releaser.completed = true;
	const auto reader  = alt::detail::io_handle::adopt(handle);
	static_cast<void>(::close(writer));

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << "a cancel after the job started changed its outcome";
	EXPECT_TRUE(reader->has_handle()) << "the open completed but its handle was discarded";
#endif
}

#ifdef _WIN32
namespace
{
	/// A named pipe whose server (read) end is opened with FILE_FLAG_OVERLAPPED and whose client
	/// (write) end is synchronous. A read on the server end with nothing written goes pending
	/// every time, which a regular file does not promise: cached reads and extending writes
	/// usually complete synchronously even on an overlapped handle.
	struct overlapped_pipe
	{
		scoped_handle server;
		scoped_handle client;

		overlapped_pipe()
		{
			static std::atomic<int> counter{0};
			const std::wstring      name = L"\\\\.\\pipe\\alt_io_backend_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
			                               std::to_wstring(counter.fetch_add(1));
			server.value                 = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 4096, 0, nullptr);
			if(server.value == INVALID_HANDLE_VALUE)
			{
				server.value = nullptr;
				return;
			}
			client.value = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if(client.value == INVALID_HANDLE_VALUE)
			{
				client.value = nullptr;
			}
		}

		[[nodiscard]] bool valid() const
		{
			return server.value != nullptr && client.value != nullptr;
		}

		bool write(std::string_view bytes) const
		{
			DWORD written = 0;
			return ::WriteFile(client.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
		}
	};

	/// Opens @p path for overlapped I/O, as a consumer bringing its own handle to adopt() might.
	scoped_handle open_overlapped(const std::filesystem::path& path)
	{
		const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		return scoped_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
	}

	alt::io_request transfer_on(HANDLE handle, alt::io_op op, alt::io_buffer buffer, std::uint64_t offset, std::size_t size)
	{
		alt::io_request request;
		request.op     = op;
		request.handle = reinterpret_cast<alt::native_handle>(handle);
		request.buffer = std::move(buffer);
		request.offset = offset;
		request.size   = size;
		return request;
	}
} // namespace

TEST_F(ThreadPoolBackendTest, AReadThatGoesPendingOnAnOverlappedHandleCompletesWithItsData)
{
	// The defect this pins (issue #52): the transfer arm treated ERROR_IO_PENDING as a failure,
	// so the job completed and released its buffer while the kernel still owned it.
	overlapped_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << "could not create the pipe: " << ::GetLastError();
	const auto worker = worker_thread(pool());
	ASSERT_NE(worker.value, nullptr);

	constexpr std::string_view payload = "pending";
	auto                       request = transfer_on(pipe.server.value, alt::io_op::read, buffer_of(payload.size()), 0, payload.size());
	const alt::io_buffer       view    = request.buffer;
	waiter                     w;
	ASSERT_TRUE(backend().submit(request, deliver, &w).has_value());
	pipe_releaser releaser(pipe.client, w);

	// Write only once the read is outstanding, so it cannot have completed synchronously. A job
	// that gave up on ERROR_IO_PENDING finishes first, and the assertions below report it.
	bool finished_early = false;
	ASSERT_TRUE(wait_until([&] {
		finished_early = w.calls.load() != 0;
		return finished_early || has_pending_io(worker.value).value_or(false);
	}));
	if(!finished_early)
	{
		ASSERT_TRUE(pipe.write(payload));
	}
	w.done.acquire();
	releaser.completed = true;

	EXPECT_FALSE(finished_early) << "the job completed before anything was written to the pipe";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);
}

TEST(ThreadPoolBackendConcurrencyTest, OneOverlappedTransferCompletingDoesNotFinishAnotherPendingOnTheSameHandle)
{
	// async_file allows concurrent positional transfers on one handle. A transfer that waited on
	// the handle rather than on an event of its own would wake when any of them completed, and
	// report a result that is not its own while the kernel still owned its buffer.
	auto pool = alt::thread_pool::create(2);
	ASSERT_TRUE(pool.has_value());
	const auto backend = alt::make_thread_pool_backend(*pool);
	const auto workers = worker_threads(*pool, 2);
	ASSERT_NE(workers[0].value, nullptr);
	ASSERT_NE(workers[1].value, nullptr);
	const auto pending_count = [&] {
		return static_cast<int>(has_pending_io(workers[0].value).value_or(false)) + static_cast<int>(has_pending_io(workers[1].value).value_or(false));
	};

	overlapped_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << "could not create the pipe: " << ::GetLastError();
	auto first_buffer  = alt::io_buffer::allocate(4);
	auto second_buffer = alt::io_buffer::allocate(4);
	ASSERT_TRUE(first_buffer.has_value());
	ASSERT_TRUE(second_buffer.has_value());
	const alt::io_buffer first_view  = *first_buffer;
	const alt::io_buffer second_view = *second_buffer;

	// The two reads are submitted one at a time so that the pipe serves them in that order.
	waiter first;
	ASSERT_TRUE(backend->submit(transfer_on(pipe.server.value, alt::io_op::read, std::move(*first_buffer), 0, 4), deliver, &first).has_value());
	pipe_releaser first_releaser(pipe.client, first);
	ASSERT_TRUE(wait_until([&] { return first.calls.load() != 0 || pending_count() == 1; }));
	waiter second;
	ASSERT_TRUE(backend->submit(transfer_on(pipe.server.value, alt::io_op::read, std::move(*second_buffer), 0, 4), deliver, &second).has_value());
	pipe_releaser second_releaser(pipe.client, second);
	ASSERT_TRUE(wait_until([&] { return first.calls.load() != 0 || second.calls.load() != 0 || pending_count() == 2; }))
	  << "both reads never showed as in flight: worker 0 pending=" << has_pending_io(workers[0].value).value_or(false)
	  << ", worker 1 pending=" << has_pending_io(workers[1].value).value_or(false);
	ASSERT_EQ(first.calls.load(), 0) << "the first read finished before anything was written";
	ASSERT_EQ(second.calls.load(), 0) << "the second read finished before anything was written";

	ASSERT_TRUE(pipe.write("AAAA"));
	first.done.acquire();
	first_releaser.completed = true;
	// Give a transfer wrongly woken by the first one's completion every chance to report.
	EXPECT_FALSE(second.done.try_acquire_for(std::chrono::milliseconds(250))) << "the second read finished when only the first had data";

	ASSERT_TRUE(pipe.write("BBBB"));
	if(second.calls.load() == 0)
	{
		second.done.acquire();
	}
	second_releaser.completed = true;

	EXPECT_FALSE(first.result.error) << first.result.error.message();
	EXPECT_EQ(first.result.bytes, 4U);
	EXPECT_EQ(text_of(first_view, 4), "AAAA");
	EXPECT_FALSE(second.result.error) << second.result.error.message();
	EXPECT_EQ(second.result.bytes, 4U);
	EXPECT_EQ(text_of(second_view, 4), "BBBB");
}

TEST_F(ThreadPoolBackendTest, AnOverlappedTransferQueuesNothingToACompletionPortTheHandleIsAssociatedWith)
{
	// A consumer may adopt a handle it has already associated with its own completion port.
	// The transfer's OVERLAPPED lives on a worker's stack, so a packet queued for it would hand
	// that port a dangling pointer. A transfer must keep its completion to itself.
	overlapped_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << "could not create the pipe: " << ::GetLastError();
	const scoped_handle port(::CreateIoCompletionPort(pipe.server.value, nullptr, 0, 1));
	ASSERT_NE(port.value, nullptr) << "could not associate the pipe with a completion port: " << ::GetLastError();

	ASSERT_TRUE(pipe.write("port"));
	auto       request = transfer_on(pipe.server.value, alt::io_op::read, buffer_of(4), 0, 4);
	const auto done    = run(request);
	ASSERT_FALSE(done.error) << done.error.message();
	EXPECT_EQ(done.bytes, 4U);

	DWORD       bytes = 0;
	ULONG_PTR   key   = 0;
	OVERLAPPED* entry = nullptr;
	EXPECT_EQ(::GetQueuedCompletionStatus(port.value, &bytes, &key, &entry, 0), FALSE);
	EXPECT_EQ(entry, nullptr) << "the transfer queued a completion packet for an OVERLAPPED that no longer exists";
}

TEST_F(ThreadPoolBackendTest, PositionalTransfersOnAnAdoptedOverlappedFileHandleWork)
{
	const auto path = dir() / "overlapped.bin";
	auto       file = open_overlapped(path);
	ASSERT_NE(file.value, nullptr) << "could not open the file for overlapped I/O: " << ::GetLastError();

	auto write_buffer = buffer_of(5);
	std::memcpy(write_buffer.bytes().data(), "world", 5);
	auto written = run(transfer_on(file.value, alt::io_op::write, std::move(write_buffer), 6, 5));
	auto hello   = buffer_of(6);
	std::memcpy(hello.bytes().data(), "hello ", 6);
	const auto first = run(transfer_on(file.value, alt::io_op::write, std::move(hello), 0, 6));
	ASSERT_FALSE(written.error) << written.error.message();
	ASSERT_FALSE(first.error) << first.error.message();
	EXPECT_EQ(written.bytes, 5U);
	EXPECT_EQ(first.bytes, 6U);

	auto                 read_buffer = buffer_of(5);
	const alt::io_buffer view        = read_buffer;
	const auto           read        = run(transfer_on(file.value, alt::io_op::read, std::move(read_buffer), 6, 5));
	ASSERT_FALSE(read.error) << read.error.message();
	EXPECT_EQ(read.bytes, 5U);
	EXPECT_EQ(text_of(view, 5), "world");

	// At and past the end is a short count of zero, as on a synchronous handle, whichever of
	// ReadFile and the wait for its completion reports ERROR_HANDLE_EOF.
	for(const std::uint64_t offset: {std::uint64_t{11}, std::uint64_t{4096}})
	{
		const auto at_end = run(transfer_on(file.value, alt::io_op::read, buffer_of(4), offset, 4));
		EXPECT_FALSE(at_end.error) << "offset " << offset << ": " << at_end.error.message();
		EXPECT_EQ(at_end.bytes, 0U) << "offset " << offset;
	}
	file.reset();
	EXPECT_EQ(read_whole(path), "hello world");
}
#endif

TEST_F(ThreadPoolBackendTest, AReadOrWriteOnAClosedDescriptorReportsTheOperatingSystemsError)
{
	auto file = open_file(write_file("data.txt", "x"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value());
	const auto stale = (*file)->native();
	file->reset(); // closes the descriptor; the number now names nothing

	for(const auto op: {alt::io_op::read, alt::io_op::write})
	{
		alt::io_request request;
		request.op      = op;
		request.handle  = stale;
		request.buffer  = buffer_of(1);
		request.size    = 1;
		const auto done = run(request);
		EXPECT_EQ(done.bytes, 0U);
#ifdef _WIN32
		EXPECT_TRUE(done.error) << "a transfer on a closed handle must fail";
#else
		EXPECT_EQ(done.error, std::errc::bad_file_descriptor);
#endif
	}
}

// The remaining operations are exercised end to end by the file layer's own tests; these
// pin the backend's side of each field contract on its own.

TEST_F(ThreadPoolBackendTest, StatOfAHandleAndOfItsPathAgree)
{
	const auto path = write_file("data.txt", "twelve bytes");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value());

	alt::file_status by_handle;
	alt::io_request  handle_request;
	handle_request.op     = alt::io_op::stat_handle;
	handle_request.handle = (*file)->native();
	handle_request.out    = &by_handle;
	ASSERT_FALSE(run(handle_request).error);

	alt::file_status by_path;
	alt::io_request  path_request;
	path_request.op  = alt::io_op::stat_path;
	path_request.in  = &path;
	path_request.out = &by_path;
	ASSERT_FALSE(run(path_request).error);

	EXPECT_EQ(by_handle.size, 12U);
	EXPECT_EQ(by_handle.kind, alt::file_kind::regular);
	EXPECT_EQ(by_handle.identity, by_path.identity);
	EXPECT_EQ(by_handle.mtime_ns, by_path.mtime_ns);
}

TEST_F(ThreadPoolBackendTest, RenameThenUnlinkMovesAndRemovesTheFile)
{
	const auto          from = write_file("from.txt", "x");
	const auto          to   = dir() / "to.txt";
	alt::io_rename_args args{.from = from, .to = to};
	alt::io_request     rename;
	rename.op = alt::io_op::rename;
	rename.in = &args;
	ASSERT_FALSE(run(rename).error);
	EXPECT_FALSE(std::filesystem::exists(from));
	EXPECT_TRUE(std::filesystem::exists(to));

	alt::io_request unlink;
	unlink.op = alt::io_op::unlink;
	unlink.in = &to;
	ASSERT_FALSE(run(unlink).error);
	EXPECT_FALSE(std::filesystem::exists(to));

	EXPECT_EQ(run(unlink).error, std::errc::no_such_file_or_directory);
}

TEST_F(ThreadPoolBackendTest, ListDirectoryReportsEntriesWithoutDotAndDotDot)
{
	write_file("a.txt", "");
	std::filesystem::create_directory(dir() / "sub");

	std::vector<alt::directory_entry> entries;
	alt::io_request                   request;
	request.op  = alt::io_op::list_directory;
	request.in  = &dir();
	request.out = &entries;
	ASSERT_FALSE(run(request).error);

	ASSERT_EQ(entries.size(), 2U);
	std::ranges::sort(entries, {}, &alt::directory_entry::name);
	EXPECT_EQ(entries[0].name, "a.txt");
	EXPECT_EQ(entries[0].kind, alt::file_kind::regular);
	EXPECT_EQ(entries[1].name, "sub");
	EXPECT_EQ(entries[1].kind, alt::file_kind::directory);

	const auto file = write_file("plain", "");
	request.in      = &file;
	EXPECT_EQ(run(request).error, std::errc::not_a_directory);
}

TEST_F(ThreadPoolBackendTest, ListDirectoryReportsNonAsciiNamesAsUtf8)
{
	// On POSIX the name is the filesystem's bytes, which for these files are the UTF-8 they
	// were created from. On Windows it is converted from UTF-16, and must be converted to UTF-8
	// rather than to the ANSI code page (issue #59): é is a different byte there, and CJK and
	// anything outside the BMP have no representation at all. The emoji is a surrogate pair.
	const std::u8string      names[] = {u8"café.txt", u8"日本.txt", u8"\U0001F4C1.txt"};
	std::vector<std::string> expected;
	for(const auto& name: names)
	{
		std::ofstream(dir() / std::filesystem::path(name), std::ios::binary) << "x";
		expected.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
	}

	std::vector<alt::directory_entry> entries;
	alt::io_request                   request;
	request.op      = alt::io_op::list_directory;
	request.in      = &dir();
	request.out     = &entries;
	const auto done = run(request);
	ASSERT_FALSE(done.error) << done.error.message();

	std::vector<std::string> listed;
	listed.reserve(entries.size());
	for(const auto& entry: entries)
	{
		listed.push_back(entry.name);
	}
	std::ranges::sort(listed);
	std::ranges::sort(expected);
	EXPECT_EQ(listed, expected);

	// The promise is that a name round-trips: rebuilt as a UTF-8 path, it names the same file.
	for(const auto& name: listed)
	{
		const std::u8string utf8(reinterpret_cast<const char8_t*>(name.data()), name.size());
		EXPECT_TRUE(std::filesystem::exists(dir() / std::filesystem::path(utf8))) << "listed name does not round-trip: " << name;
	}
}

#ifdef _WIN32
TEST_F(ThreadPoolBackendTest, ListDirectoryReportsANameThatIsNotValidUtf16AsAnIllegalByteSequence)
{
	// NTFS permits an unpaired surrogate in a name. It has no UTF-8 form, so the listing reports
	// that precisely instead of a lossy name that would not round-trip, or a generic io_error.
	const std::wstring name = (dir() / L"bad").wstring() + L"\xD800" + L"name.txt";
	const HANDLE       file = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	ASSERT_NE(file, INVALID_HANDLE_VALUE) << "could not create the file: " << ::GetLastError();
	::CloseHandle(file);

	std::vector<alt::directory_entry> entries;
	alt::io_request                   request;
	request.op  = alt::io_op::list_directory;
	request.in  = &dir();
	request.out = &entries;
	EXPECT_EQ(run(request).error, std::errc::illegal_byte_sequence);
	EXPECT_TRUE(entries.empty()) << "a failed listing must leave the output untouched";
}
#endif

TEST_F(ThreadPoolBackendTest, BothSyncModesSucceedOnAnOpenFile)
{
	auto file = open_file(write_file("data.txt", "x"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value());
	for(const auto op: {alt::io_op::sync_full, alt::io_op::sync_data})
	{
		alt::io_request request;
		request.op     = op;
		request.handle = (*file)->native();
		EXPECT_FALSE(run(request).error);
	}
}

// The driver's preconditions terminate in every build type, NDEBUG included, so these tests do not
// skip without asserts: they are what proves a release-tests run still stops a mis-driven test.
namespace
{
	/// Runs @p misuse in a forked child under the threadsafe style, or skips where death tests cannot fork.
	template<typename Misuse>
	// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, which holds one skip guard and one expectation. Measured with the pinned clang-tidy-19 against the default threshold of 25: the body reports 38.
	void expect_driver_terminates(Misuse&& misuse, const char* message)
	{
		if(alt_test::thread_sanitizer_is_active)
		{
			GTEST_SKIP() << alt_test::thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
		}

		const alt_test::scoped_death_test_style style{"threadsafe"};
		EXPECT_DEATH(misuse(), message);
	}
} // namespace

TEST(IoBackendDeathTest, CompletingAnOperationTwiceTerminatesInsteadOfRunningItsCallbackAgain)
{
	expect_driver_terminates(
	  [] {
		  alt::manual_io_backend backend;
		  recorder               rec;
		  const auto             token = backend.submit(read_request(), record, &rec);
		  if(!token)
		  {
			  std::abort(); // not the termination under test: the second complete() must be what ends the child
		  }
		  backend.complete(0, done(1));
		  backend.complete(0, done(1));
	  },
	  "complete\\(\\) of an operation already completed");
}

TEST(IoBackendDeathTest, CompletingAnIndexNeverSubmittedTerminates)
{
	expect_driver_terminates(
	  [] {
		  alt::manual_io_backend backend;
		  backend.complete(0, done(1));
	  },
	  "complete\\(\\) of an index never submitted");
}

TEST(IoBackendDeathTest, AskingWhetherAnIndexNeverSubmittedWasCancelledTerminates)
{
	expect_driver_terminates(
	  [] {
		  const alt::manual_io_backend backend;
		  static_cast<void>(backend.was_cancelled(0));
	  },
	  "was_cancelled\\(\\) of an index never submitted");
}

TEST(IoBackendDeathTest, ReadingTheRequestOfAnIndexNeverSubmittedTerminates)
{
	expect_driver_terminates(
	  [] {
		  const alt::manual_io_backend backend;
		  static_cast<void>(backend.request(0));
	  },
	  "request\\(\\) of an index never submitted");
}

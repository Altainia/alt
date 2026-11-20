#include <gtest/gtest.h>

#include <algorithm>
#include <alt/detail/file_types.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/thread_pool.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <semaphore>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "coroutine_test_support.hpp"

#ifdef __linux__
#	include <fcntl.h>
#	include <sys/stat.h>
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
			m_dir = std::filesystem::temp_directory_path() /
			        ("alt_io_backend_" + std::to_string(counter.fetch_add(1)) + "_" +
			         std::to_string(reinterpret_cast<std::uintptr_t>(this)));
			std::filesystem::create_directories(m_dir);
			// Constructed in place: the expected holding an immovable pool cannot be moved.
			ASSERT_TRUE(m_pool.has_value());
		}

		void TearDown() override
		{
			m_backend.reset();
			std::error_code ignored;
			std::filesystem::remove_all(m_dir, ignored);
		}

		[[nodiscard]] const std::filesystem::path& dir() const
		{
			return m_dir;
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
			const auto path = m_dir / name;
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
		// The pool is declared first so it is destroyed last: the backend borrows it.
		std::expected<alt::thread_pool, std::error_code> m_pool = alt::thread_pool::create(1);
		std::filesystem::path                            m_dir;
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

#ifdef __linux__
namespace
{
	/// True once some thread of this process is blocked inside open() waiting for a FIFO's other end.
	bool a_thread_waits_for_a_fifo_partner()
	{
		std::error_code ignored;
		for(const auto& task: std::filesystem::directory_iterator("/proc/self/task", ignored))
		{
			std::ifstream wchan(task.path() / "wchan");
			std::string   where;
			wchan >> where;
			if(where == "wait_for_partner")
			{
				return true;
			}
		}
		return false;
	}

	/// Polls until a worker is blocked in open(), or ten seconds pass.
	bool wait_for_a_worker_blocked_on_a_fifo()
	{
		const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while(std::chrono::steady_clock::now() < give_up)
		{
			if(a_thread_waits_for_a_fifo_partner())
			{
				return true;
			}
			std::this_thread::yield();
		}
		return false;
	}
} // namespace
#endif

TEST_F(ThreadPoolBackendTest, AJobCancelledAfterItStartedStillCompletesAndReportsItsResult)
{
#ifndef __linux__
	GTEST_SKIP() << "needs a syscall that blocks mid-job and a way to see it block; only /proc on Linux offers both";
#else
	// Opening a FIFO for reading blocks inside the syscall until a writer appears. That
	// holds the job in the middle of its I/O, which no regular-file operation can do, so
	// the cancel lands strictly after the job started and strictly before it finished.
	const auto fifo = dir() / "gate";
	ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

	alt::io_open_args  args{.path = fifo, .open_flags = alt::flags(alt::open_flag::read), .mode = {}};
	alt::native_handle handle = alt::invalid_native_handle;
	alt::io_request    request;
	request.op  = alt::io_op::open;
	request.in  = &args;
	request.out = &handle;

	waiter     w;
	const auto token = backend().submit(request, deliver, &w);
	ASSERT_TRUE(token.has_value());

	// Wait until the worker is provably inside open(): then the job has started.
	ASSERT_TRUE(wait_for_a_worker_blocked_on_a_fifo()) << "the worker never blocked in open(); the cancel would not be mid-job";

	backend().cancel(*token);

	const int writer = ::open(fifo.c_str(), O_WRONLY); // releases the blocked reader
	ASSERT_GE(writer, 0);
	w.done.acquire();
	const auto reader = alt::detail::io_handle::adopt(handle);
	static_cast<void>(::close(writer));

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << "a cancel after the job started changed its outcome";
	EXPECT_TRUE(reader->has_handle()) << "the open completed but its handle was discarded";
#endif
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

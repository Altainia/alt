// The io_uring backend exists only on Linux, so this whole file does too.
#ifdef __linux__

#	include <gtest/gtest.h>

#	include <alt/detail/file_types.hpp>
#	include <alt/detail/io_handle.hpp>
#	include <alt/io_backend.hpp>
#	include <alt/io_buffer.hpp>
#	include <alt/thread_pool.hpp>
#	include <atomic>
#	include <chrono>
#	include <cstddef>
#	include <cstdint>
#	include <cstdlib>
#	include <cstring>
#	include <expected>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <memory>
#	include <semaphore>
#	include <string>
#	include <system_error>
#	include <thread>
#	include <unistd.h>
#	include <utility>

#	include "death_test_support.hpp"
#	include "io_backend_io_uring.hpp"

namespace
{
	/// The backpressure bound the file layer's own default uses, and this suite's default.
	constexpr std::size_t default_max_in_flight = 64;

	/// How long a test waits for a callback before it fails rather than hangs.
	constexpr auto completion_guard = std::chrono::seconds(5);

	/**
	 * The most submission entries io_uring_setup accepts (IORING_MAX_ENTRIES).
	 *
	 * Spelled as a literal because this file does not include liburing.h: the dependency belongs
	 * to one translation unit in src/, and no header this suite can reach names the constant.
	 */
	constexpr std::size_t max_submission_entries = 32768;

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

	/// Collects one completion, lets a test wait for it, and says whether it ran inside submit().
	struct waiter
	{
		std::binary_semaphore done{0};
		alt::io_completion    result{};
		std::atomic<int>      calls{0};
		/// Set by the test once submit() has returned; read by the callback.
		std::atomic<bool> submitted{false};
		/// Whether the callback ran before submit() returned.
		std::atomic<bool> ran_inline{false};
		std::thread::id   thread;
	};

	void deliver(void* state, alt::io_completion result) noexcept
	{
		auto& w = *static_cast<waiter*>(state);
		w.ran_inline.store(!w.submitted.load());
		w.thread = std::this_thread::get_id();
		w.result = result;
		w.calls.fetch_add(1);
		w.done.release();
	}

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

	class IoUringBackendTest: public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<int> counter{0};
			m_dir.path = std::filesystem::temp_directory_path() /
			             ("alt_io_uring_backend_" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(::getpid()));
			std::filesystem::create_directories(m_dir.path);
			ASSERT_TRUE(m_pool.has_value());
			m_backend = build(0, default_max_in_flight);
			ASSERT_NE(m_backend, nullptr);
		}

		void TearDown() override
		{
			m_backend.reset();
		}

		/**
		 * Builds a backend on this fixture's pool, or FAILS the test if a ring cannot be created.
		 *
		 * Deliberately not a skip. A runner that stops permitting io_uring would otherwise leave
		 * this whole backend unexercised behind a green sweep, which is the one outcome worth
		 * failing for; see TheRunnerSupportsEveryOpcodeThisBackendNeeds.
		 */
		std::unique_ptr<alt::io_backend> build(std::size_t queue_depth, std::size_t max_in_flight)
		{
			auto made = alt::detail::make_io_uring_backend(*m_pool, queue_depth, max_in_flight);
			EXPECT_TRUE(made.has_value()) << "no io_uring ring: " << (made ? std::string{} : made.error().message());
			if(!made)
			{
				return nullptr;
			}
			return std::move(*made);
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
			return *m_backend;
		}

		/// Submits, records that submit() returned, and waits for the callback.
		alt::io_completion run(const alt::io_request& request, waiter& w)
		{
			const auto token = backend().submit(request, deliver, &w);
			w.submitted.store(true);
			EXPECT_TRUE(token.has_value());
			if(!token)
			{
				return alt::io_completion{.bytes = 0, .error = token.error()};
			}
			EXPECT_TRUE(w.done.try_acquire_for(completion_guard)) << "the operation never completed";
			EXPECT_EQ(w.calls.load(), 1);
			return w.result;
		}

		alt::io_completion run(const alt::io_request& request)
		{
			waiter w;
			return run(request, w);
		}

		/// Opens @p path through the backend, which forwards the open to its internal pool backend.
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

		static alt::io_buffer buffer_of(std::size_t size)
		{
			auto buffer = alt::io_buffer::allocate(size);
			EXPECT_TRUE(buffer.has_value());
			return std::move(*buffer);
		}

	private:
		// m_dir is declared first so it is removed after the backend and the pool are gone.
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
} // namespace

TEST_F(IoUringBackendTest, ReportsItsOwnKind)
{
	EXPECT_EQ(backend().kind(), alt::io_backend_kind::io_uring);
}

TEST_F(IoUringBackendTest, DerivesBothRingSizesFromMaxInFlightWhenDepthIsZero)
{
	constexpr std::size_t max_in_flight = 48;
	const auto            built         = build(0, max_in_flight);
	ASSERT_NE(built, nullptr);

	EXPECT_GE(alt::detail::io_uring_submission_depth_for_testing(*built), max_in_flight);
	// The completion ring also covers the reserved internal completions: the wake-up NOP, and one
	// cancel for every operation that can be in flight at once.
	EXPECT_GE(alt::detail::io_uring_completion_depth_for_testing(*built), max_in_flight + max_in_flight + 1);
}

TEST_F(IoUringBackendTest, SizesTheCompletionRingForMaxInFlightEvenAtDepthOne)
{
	const auto built = build(1, default_max_in_flight);
	ASSERT_NE(built, nullptr);

	// The caller asked for the smallest submission ring there is, and got it.
	EXPECT_EQ(alt::detail::io_uring_submission_depth_for_testing(*built), 1U);
	// The completion ring is sized from max_in_flight regardless, so completion-queue overflow is
	// unreachable at any queue_depth rather than only at a generous one.
	EXPECT_GE(alt::detail::io_uring_completion_depth_for_testing(*built), default_max_in_flight);
}

TEST_F(IoUringBackendTest, HonoursAnExplicitQueueDepth)
{
	const auto exact = build(128, default_max_in_flight);
	ASSERT_NE(exact, nullptr);
	EXPECT_EQ(alt::detail::io_uring_submission_depth_for_testing(*exact), 128U);

	// A depth that is not a power of two is rounded up, and the accessor reports what is in
	// effect rather than what was asked for.
	const auto rounded = build(100, default_max_in_flight);
	ASSERT_NE(rounded, nullptr);
	EXPECT_EQ(alt::detail::io_uring_submission_depth_for_testing(*rounded), 128U);
}

TEST_F(IoUringBackendTest, ClampsAQueueDepthLargerThanTheKernelAllows)
{
	// io_uring_setup refuses a submission ring larger than IORING_MAX_ENTRIES outright, so the
	// clamp is what turns an out-of-range queue_depth into a working ring instead of EINVAL. The
	// factory is called directly rather than through build(), which cannot tell the two apart.
	auto made = alt::detail::make_io_uring_backend(pool(), 1U << 20U, default_max_in_flight);
	if(!made && made.error() == std::errc::not_enough_memory)
	{
		// The largest ring the kernel allows is a few megabytes of its memory. A clamp regression
		// reports EINVAL rather than this, so skipping here does not hide one.
		GTEST_SKIP() << "the largest ring the kernel allows does not fit this environment's budget: " << made.error().message();
	}
	ASSERT_TRUE(made.has_value()) << "an out-of-range queue_depth must be clamped, not refused: " << made.error().message();
	EXPECT_EQ(alt::detail::io_uring_submission_depth_for_testing(**made), max_submission_entries);
}

TEST_F(IoUringBackendTest, ForwardsAnOperationItDoesNotHandleToThePool)
{
	const std::filesystem::path where = dir();
	alt::file_status            status;
	alt::io_request             stat;
	stat.op  = alt::io_op::stat_path;
	stat.in  = &where;
	stat.out = &status;

	waiter     w;
	const auto token = backend().submit(stat, deliver, &w);
	w.submitted.store(true);
	ASSERT_TRUE(token.has_value()) << token.error().message();
	EXPECT_EQ(token->value() & alt::detail::io_uring_token_tag, 0U) << "a forwarded operation's token carries the ring tag";

	ASSERT_TRUE(w.done.try_acquire_for(completion_guard)) << "the forwarded stat never completed";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(status.kind, alt::file_kind::directory);
	// Nothing was submitted to the ring, so the reaper saw no completion it had no operation for.
	EXPECT_EQ(alt::detail::io_uring_ignored_completions_for_testing(backend()), 0U);
}

TEST_F(IoUringBackendTest, AReadReturnsTheFilesBytes)
{
	const auto path = write_file("data.txt", "hello, backend");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto       buffer = buffer_of(5);
	const auto copy   = buffer;
	const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 7, 5));

	EXPECT_FALSE(done.error) << done.error.message();
	EXPECT_EQ(done.bytes, 5U);
	EXPECT_EQ(text_of(copy, 5), "backe");
}

TEST_F(IoUringBackendTest, AWriteAtPutsBytesAtTheOffset)
{
	const auto path = write_file("data.txt", "0123456789");
	auto       file = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto buffer = buffer_of(3);
	std::memcpy(buffer.bytes().data(), "abc", 3);
	const auto done = run(positional(alt::io_op::write, **file, std::move(buffer), 4, 3));

	EXPECT_FALSE(done.error) << done.error.message();
	EXPECT_EQ(done.bytes, 3U);
	file->reset();
	EXPECT_EQ(read_whole(path), "0123abc789");
}

/**
 * The capability check, and this suite's deliberate canary.
 *
 * The factory probes the running kernel for READ, WRITE, FSYNC and ASYNC_CANCEL all-or-nothing
 * and refuses with std::errc::not_supported if any is missing, so a backend that was built at all
 * is the evidence that all four are supported here. Rings have existed since kernel 5.1 while
 * READ and WRITE arrived in 5.6 and ASYNC_CANCEL in 5.5, which is why creating a ring is not that
 * evidence by itself.
 *
 * Its entire job is to FAIL, never to skip, when a ring cannot be created or an opcode is
 * missing: a runner that stops permitting io_uring must turn this suite red rather than leave the
 * whole backend unexercised behind a green sweep. Do not "fix" it into a GTEST_SKIP.
 */
TEST_F(IoUringBackendTest, TheRunnerSupportsEveryOpcodeThisBackendNeeds)
{
	auto made = alt::detail::make_io_uring_backend(pool(), 0, default_max_in_flight);
	ASSERT_TRUE(made.has_value()) << "io_uring is unavailable or lacks an opcode this backend needs: " << (made ? std::string{} : made.error().message());
	EXPECT_EQ((*made)->kind(), alt::io_backend_kind::io_uring);
}

TEST_F(IoUringBackendTest, CancellingAForwardedOperationReachesThePool)
{
	// Occupy the pool's only thread so the forwarded stat stays queued behind it, which is what
	// makes the pool's own detach-on-cancel observable: a job that has already started runs to
	// completion and reports its real result instead.
	blocker gate;
	pool().post(alt::work_item{.invoke = block, .state = &gate});
	gate.entered.acquire();

	const std::filesystem::path where = dir();
	alt::file_status            status;
	alt::io_request             stat;
	stat.op  = alt::io_op::stat_path;
	stat.in  = &where;
	stat.out = &status;
	waiter     w;
	const auto token = backend().submit(stat, deliver, &w);
	w.submitted.store(true);
	if(!token)
	{
		gate.release.release();
	}
	ASSERT_TRUE(token.has_value()) << token.error().message();

	// The token carries no tag, so cancel() must hand it to the internal pool backend; this
	// backend has nothing of its own to cancel for it.
	backend().cancel(*token);
	gate.release.release();
	ASSERT_TRUE(w.done.try_acquire_for(completion_guard)) << "the cancelled stat never completed";

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_EQ(w.result.error, std::errc::operation_canceled) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 0U);
}

namespace
{
	/**
	 * The death test's body: a stat forwarded to the pool, still queued behind a blocked worker,
	 * when the backend is destroyed. Every operation in flight is a precondition violation,
	 * whether it is the ring's or the pool's, because the backend is what the trampoline reports
	 * back to.
	 */
	void destroy_the_backend_with_a_forwarded_operation_in_flight()
	{
		auto pool = alt::thread_pool::create(1);
		if(!pool)
		{
			std::abort();
		}
		auto made = alt::detail::make_io_uring_backend(*pool, 0, default_max_in_flight);
		if(!made)
		{
			std::abort();
		}
		std::unique_ptr<alt::io_backend> backend = std::move(*made);
		blocker                          gate;
		pool->post(alt::work_item{.invoke = block, .state = &gate});
		gate.entered.acquire();

		const std::filesystem::path where = std::filesystem::temp_directory_path();
		alt::file_status            status;
		alt::io_request             stat;
		stat.op  = alt::io_op::stat_path;
		stat.in  = &where;
		stat.out = &status;
		waiter queued;
		if(!backend->submit(stat, deliver, &queued))
		{
			std::abort();
		}
		backend.reset(); // the stat is still queued behind the blocked worker
		// Reached only if the destructor did not stop the process: let the pool drain so the child
		// exits normally, which is what fails the expectation.
		gate.release.release();
		queued.done.acquire();
	}
} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, which holds two skip guards and one expectation. Measured with the pinned clang-tidy-19 against the default threshold of 25: the body reports 39.
TEST(IoUringBackendDeathTest, DestroyingItWithAForwardedOperationInFlightTerminates)
{
	if(!alt_test::asserts_are_compiled_in)
	{
		GTEST_SKIP() << "the in-flight check is an assert in debug builds and std::terminate() under NDEBUG; "
		                "the death test pins the assert's message, so it runs only where the assert is compiled in.";
	}
	if(alt_test::thread_sanitizer_is_active)
	{
		GTEST_SKIP() << alt_test::thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const alt_test::scoped_death_test_style style{"threadsafe"};
	EXPECT_DEATH(destroy_the_backend_with_a_forwarded_operation_in_flight(), "io_uring backend destroyed with forwarded operations in flight");
}

#endif

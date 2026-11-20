#include <gtest/gtest.h>

#include <algorithm>
#include <alt/cancellable_task.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/scheduler.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
#ifndef _WIN32
#	include <sys/stat.h>
#	include <unistd.h>
#endif
#include <thread>
#include <utility>

#include "coroutine_test_support.hpp"
#include "death_test_support.hpp"

namespace
{

	using alt::detail::io_awaiter;
	using alt::detail::io_handle;
	using alt::detail::io_shared;

	/** What one awaiting chain reports back to the test that started it. */
	struct op_record
	{
		bool slot_acquired = false;
		bool body_ran      = false;
	};

	/** A read request over @p buffer, with every field spelled out. */
	alt::io_request read_request(const alt::io_buffer& buffer)
	{
		return alt::io_request{
		  .op     = alt::io_op::read,
		  .handle = alt::invalid_native_handle,
		  .buffer = buffer,
		  .offset = 0,
		  .size   = buffer.size(),
		  .in     = nullptr,
		  .out    = nullptr,
		  .state  = nullptr,
		};
	}

	/** The two-phase operation: a backpressure permit, then the submitted phase. */
	alt::cancellable_task awaiting_chain(std::shared_ptr<io_shared> shared, alt::io_request request, std::shared_ptr<op_record> record)
	{
		auto permit = co_await alt::detail::acquire_permit(shared);
		if(!permit)
		{
			co_return;
		}
		record->slot_acquired = true;

		static_cast<void>(co_await io_awaiter{shared, std::move(request), nullptr, std::move(*permit)});
		record->body_ran = true;
	}

	/** Starts a pool, which every test here needs to name and none of them uses. */
	alt::thread_pool& pool_of(std::expected<alt::thread_pool, std::error_code>& created)
	{
		EXPECT_TRUE(created.has_value());
		return *created;
	}

	/**
	 * Completes operation @p index from another thread, but only once @p destroying is
	 * set and a little longer after that, so the destructor under test is already
	 * blocked on the backend when the completion lands. Lifted out of the test body it
	 * serves, where the lambda's nesting pushes that body past clang-tidy's
	 * cognitive-complexity threshold.
	 */
	std::jthread complete_once_destroying(alt::manual_io_backend& backend, const std::atomic<bool>& destroying, const std::size_t index)
	{
		return std::jthread{[&backend, &destroying, index] {
			while(!destroying.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{20});
			backend.complete(index, alt::io_completion{.bytes = 16, .error = {}});
		}};
	}

	/**
	 * The scenario the death test runs in its child: one operation holds the only permit
	 * at the backend, a second is parked behind it, and the service is destroyed.
	 */
	void destroy_a_service_with_an_operation_parked()
	{
		auto                   created = alt::thread_pool::create(1);
		alt::thread_pool&      pool    = pool_of(created);
		alt::manual_io_backend backend;
		alt::manual_scheduler  home;
		const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

		const alt::file_io_options options{.backend = alt::io_backend_kind::automatic, .max_in_flight = 1, .queue_depth = 0};

		{
			const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend, options);

			const std::shared_ptr<op_record> first  = std::make_shared<op_record>();
			const std::shared_ptr<op_record> second = std::make_shared<op_record>();

			const alt::cancellation_handle chain_a = alt::start(awaiting_chain(alt::detail::file_io_access::shared(*io), read_request(buffer), first));
			const alt::cancellation_handle chain_b = alt::start(awaiting_chain(alt::detail::file_io_access::shared(*io), read_request(buffer), second));

			// Destroyed at the end of this scope with the second operation still parked: the violation.
		}
	}

}

namespace
{

	/** What a started task produced, and whether it has produced it yet. */
	template<typename T>
	struct outcome
	{
		bool             finished = false;
		std::optional<T> result;
	};

	/** Starts @p work and stores what it produced in @p out, so a test can run it without blocking. */
	template<typename T>
	alt::cancellable_task capture(alt::task<T> work, std::shared_ptr<outcome<T>> out)
	{
		out->result.emplace(co_await std::move(work));
		out->finished = true;
	}

	/**
	 * A real service on the thread-pool backend, completing onto a scheduler the test
	 * drives itself, plus a scratch directory removed afterwards.
	 */
	class FileIoPathTest: public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<int> counter{0};
			m_dir.path = std::filesystem::temp_directory_path() / ("alt_file_io_path_" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
			std::filesystem::create_directories(m_dir.path);
			ASSERT_TRUE(m_pool.has_value());
			ASSERT_TRUE(m_io.has_value());
		}

		[[nodiscard]] const std::filesystem::path& dir() const
		{
			return m_dir.path;
		}

		alt::file_io& io()
		{
			return *m_io;
		}

		/** Runs @p work to completion on the test's scheduler, failing rather than hanging past the deadline. */
		template<typename T>
		T run(alt::task<T> work)
		{
			const auto                     state = std::make_shared<outcome<T>>();
			const alt::cancellation_handle chain = alt::start(capture(std::move(work), state));
			const auto                     limit = std::chrono::steady_clock::now() + std::chrono::seconds{10};
			while(!state->finished)
			{
				if(!m_home.run_one())
				{
					if(std::chrono::steady_clock::now() > limit)
					{
						ADD_FAILURE() << "the operation did not finish within 10 seconds";
						return std::unexpected(std::make_error_code(std::errc::timed_out));
					}
					std::this_thread::sleep_for(std::chrono::milliseconds{1});
				}
			}
			std::optional<T>& produced = state->result;
			if(produced.has_value())
			{
				return std::move(*produced);
			}
			return std::unexpected(std::make_error_code(std::errc::timed_out));
		}

		std::filesystem::path write_file(const std::string& name, const std::string& content) const
		{
			const std::filesystem::path path = m_dir.path / name;
			std::ofstream(path, std::ios::binary) << content;
			return path;
		}

		static std::string read_whole(const std::filesystem::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		}

	private:
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
		alt::manual_scheduler                            m_home;
		std::expected<alt::file_io, std::error_code>     m_io = alt::file_io::create(*m_pool, alt::scheduler_ref{m_home});
	};

}

TEST_F(FileIoPathTest, StatOfAPathReportsItsSizeAndKind)
{
	const auto path = write_file("data.txt", "0123456789");

	const auto status = run(io().stat(path));

	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(status->size, 10U);
	EXPECT_EQ(status->kind, alt::file_kind::regular);
}

TEST_F(FileIoPathTest, StatOfADirectoryReportsKindDirectory)
{
	const auto status = run(io().stat(dir()));

	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(status->kind, alt::file_kind::directory);
}

TEST_F(FileIoPathTest, RenameMovesAFileAndTheOldPathIsGone)
{
	const auto from = write_file("old.txt", "payload");
	const auto to   = dir() / "new.txt";

	const auto renamed = run(io().rename(from, to));

	ASSERT_TRUE(renamed.has_value());
	EXPECT_FALSE(std::filesystem::exists(from));
	EXPECT_EQ(read_whole(to), "payload");
}

TEST_F(FileIoPathTest, RenameOverAnExistingFileReplacesIt)
{
	const auto from   = write_file("from.txt", "new contents");
	const auto target = write_file("target.txt", "old contents");

	const auto renamed = run(io().rename(from, target));

	ASSERT_TRUE(renamed.has_value());
	EXPECT_EQ(read_whole(target), "new contents");
	EXPECT_FALSE(std::filesystem::exists(from));
}

TEST_F(FileIoPathTest, RenameAcrossDevicesReportsCrossDeviceLink)
{
#ifdef _WIN32
	GTEST_SKIP() << "Windows moves a file across volumes (MOVEFILE_COPY_ALLOWED), so there is no cross-device error to observe.";
#else
	const std::filesystem::path other{"/dev/shm"};
	std::error_code             ignored;
	if(!std::filesystem::is_directory(other, ignored))
	{
		GTEST_SKIP() << "/dev/shm is not available, so there is no second filesystem to rename across.";
	}
	// Same device means the rename would succeed, and asserting an error would be wrong.
	struct stat on_dir{};
	struct stat on_other{};
	if(::stat(dir().c_str(), &on_dir) != 0 || ::stat(other.c_str(), &on_other) != 0 || on_dir.st_dev == on_other.st_dev)
	{
		GTEST_SKIP() << "the scratch directory and /dev/shm are on the same filesystem, so a rename between them does not cross a device.";
	}

	const auto                  from = write_file("crossing.txt", "x");
	const std::filesystem::path to   = other / ("alt_cross_" + std::to_string(::getpid()) + ".txt");

	const auto renamed = run(io().rename(from, to));

	std::filesystem::remove(to, ignored);
	ASSERT_FALSE(renamed.has_value());
	EXPECT_EQ(renamed.error().default_error_condition(), std::make_error_condition(std::errc::cross_device_link));
	EXPECT_TRUE(std::filesystem::exists(from)) << "a failed rename must leave the source in place";
#endif
}

TEST_F(FileIoPathTest, UnlinkRemovesAFile)
{
	const auto path = write_file("doomed.txt", "x");

	const auto removed = run(io().unlink(path));

	ASSERT_TRUE(removed.has_value());
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(FileIoPathTest, UnlinkOfAMissingPathReportsNoSuchFileOrDirectory)
{
	const auto removed = run(io().unlink(dir() / "never_existed.txt"));

	ASSERT_FALSE(removed.has_value());
	EXPECT_EQ(removed.error().default_error_condition(), std::make_error_condition(std::errc::no_such_file_or_directory));
}

TEST_F(FileIoPathTest, ListDirectoryReturnsEveryEntryWithoutDotOrDotDot)
{
	write_file("a.txt", "a");
	write_file("b.txt", "b");
	std::filesystem::create_directory(dir() / "sub");

	auto listed = run(io().list_directory(dir()));

	ASSERT_TRUE(listed.has_value());
	std::ranges::sort(*listed, {}, &alt::directory_entry::name);
	ASSERT_EQ(listed->size(), 3U);
	EXPECT_EQ((*listed)[0].name, "a.txt");
	EXPECT_EQ((*listed)[0].kind, alt::file_kind::regular);
	EXPECT_EQ((*listed)[1].name, "b.txt");
	EXPECT_EQ((*listed)[1].kind, alt::file_kind::regular);
	EXPECT_EQ((*listed)[2].name, "sub");
	EXPECT_EQ((*listed)[2].kind, alt::file_kind::directory);
}

TEST_F(FileIoPathTest, ListDirectoryOfAnEmptyDirectoryReturnsNoEntries)
{
	const auto listed = run(io().list_directory(dir()));

	ASSERT_TRUE(listed.has_value());
	EXPECT_TRUE(listed->empty());
}

TEST_F(FileIoPathTest, ListDirectoryOfAFileReportsNotADirectory)
{
	const auto path = write_file("plain.txt", "x");

	const auto listed = run(io().list_directory(path));

	ASSERT_FALSE(listed.has_value());
	EXPECT_EQ(listed.error().default_error_condition(), std::make_error_condition(std::errc::not_a_directory));
}

TEST(FileIoTest, CreateSucceedsAndReportsTheThreadPoolBackend)
{
	auto                  created = alt::thread_pool::create(1);
	alt::thread_pool&     pool    = pool_of(created);
	alt::manual_scheduler home;

	const auto io = alt::file_io::create(pool, alt::scheduler_ref{home});

	ASSERT_TRUE(io.has_value());
	EXPECT_EQ(io->backend(), alt::io_backend_kind::thread_pool);
}

TEST(FileIoTest, CreateWithAnExplicitBackendReportsThatBackendsKind)
{
	auto                  created = alt::thread_pool::create(1);
	alt::thread_pool&     pool    = pool_of(created);
	alt::manual_scheduler home;

	const auto named = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::thread_pool});
	ASSERT_TRUE(named.has_value());
	EXPECT_EQ(named->backend(), alt::io_backend_kind::thread_pool);

	alt::manual_io_backend manual;
	const auto             injected = alt::file_io::create(pool, alt::scheduler_ref{home}, manual);
	ASSERT_TRUE(injected.has_value());
	EXPECT_EQ(injected->backend(), alt::io_backend_kind::manual);

	// A kind this build cannot provide is an error, not a silent substitution.
	const auto uring = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::io_uring});
	ASSERT_FALSE(uring.has_value());
	EXPECT_EQ(uring.error(), std::errc::not_supported);

	// manual cannot be conjured from an enumerator.
	const auto conjured = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::manual});
	ASSERT_FALSE(conjured.has_value());
	EXPECT_EQ(conjured.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, CreateWithABackendOverloadRejectsAMismatchingOptionsBackend)
{
	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_scheduler  home;
	alt::manual_io_backend manual;

	const auto mismatched = alt::file_io::create(pool, alt::scheduler_ref{home}, manual, alt::file_io_options{.backend = alt::io_backend_kind::io_uring});
	ASSERT_FALSE(mismatched.has_value());
	EXPECT_EQ(mismatched.error(), std::errc::invalid_argument);

	const auto agreeing = alt::file_io::create(pool, alt::scheduler_ref{home}, manual, alt::file_io_options{.backend = alt::io_backend_kind::manual});
	ASSERT_TRUE(agreeing.has_value());
	EXPECT_EQ(agreeing->backend(), alt::io_backend_kind::manual);
}

TEST(FileIoTest, CreateRejectsAZeroMaxInFlight)
{
	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_scheduler  home;
	alt::manual_io_backend manual;

	// A zero bound would park the first operation on a permit that can never exist.
	const auto built_in = alt::file_io::create(pool, alt::scheduler_ref{home}, alt::file_io_options{.max_in_flight = 0});
	ASSERT_FALSE(built_in.has_value());
	EXPECT_EQ(built_in.error(), std::errc::invalid_argument);

	const auto injected = alt::file_io::create(pool, alt::scheduler_ref{home}, manual, alt::file_io_options{.max_in_flight = 0});
	ASSERT_FALSE(injected.has_value());
	EXPECT_EQ(injected.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, CreateRejectsAnEmptyCompletionScheduler)
{
	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend manual;

	// Reported here, not asserted inside scheduler_ref::post on an I/O thread.
	const auto built_in = alt::file_io::create(pool, alt::scheduler_ref{});
	ASSERT_FALSE(built_in.has_value());
	EXPECT_EQ(built_in.error(), std::errc::invalid_argument);

	const auto injected = alt::file_io::create(pool, alt::scheduler_ref{}, manual);
	ASSERT_FALSE(injected.has_value());
	EXPECT_EQ(injected.error(), std::errc::invalid_argument);
}

TEST(FileIoTest, TheDestructorReturnsOnceTheBackendIsQuietEvenWithResumptionsQueued)
{
	alt_test::reset_coroutine_test_state();

	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> finished  = std::make_shared<op_record>();
	const std::shared_ptr<op_record> straggler = std::make_shared<op_record>();

	std::atomic<bool> destroying{false};
	std::jthread      late_completer;

	{
		const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend);
		ASSERT_TRUE(io.has_value());

		const alt::cancellation_handle chain_a = alt::start(awaiting_chain(alt::detail::file_io_access::shared(*io), read_request(buffer), finished));
		const alt::cancellation_handle chain_b = alt::start(awaiting_chain(alt::detail::file_io_access::shared(*io), read_request(buffer), straggler));
		ASSERT_EQ(backend.pending(), 2U);

		// One completion lands now, so its resumption is queued on a scheduler nobody
		// pumps. Were the destructor to wait for that to run, it would never return.
		backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
		ASSERT_EQ(backend.pending(), 1U);
		ASSERT_FALSE(finished->body_ran);

		// The other completes only once the destructor is under way, from another
		// thread, so the destructor has to wait for it.
		late_completer = complete_once_destroying(backend, destroying, 1);

		destroying.store(true, std::memory_order_release);
	}

	// Only reachable once the destructor returned, and the backend had to be quiet for
	// that: operation 1 is gone from it. The ordering rests on a 20 ms sleep after
	// `destroying` is stored, which is before the destructor has actually begun, so under
	// extreme scheduling delay a destructor that did NOT wait could still pass here. The
	// positive path is deterministic; the mutation result is weaker evidence than it looks.
	EXPECT_EQ(backend.pending(), 0U);

	// And the resumptions are still waiting, unrun, with nobody having pumped them.
	EXPECT_FALSE(finished->body_ran);
	EXPECT_FALSE(straggler->body_ran);

	EXPECT_EQ(home.drain(), 2U);
	EXPECT_TRUE(finished->body_ran);
	EXPECT_TRUE(straggler->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(FileIoTest, AQueuedResumptionRunSafelyAfterTheDestructorReturned)
{
	alt_test::reset_coroutine_test_state();

	auto                   created = alt::thread_pool::create(1);
	alt::thread_pool&      pool    = pool_of(created);
	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();

	{
		const auto io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend);
		ASSERT_TRUE(io.has_value());

		const alt::cancellation_handle chain = alt::start(awaiting_chain(alt::detail::file_io_access::shared(*io), read_request(buffer), record));
		ASSERT_EQ(backend.pending(), 1U);

		// Completed at the backend, so the resumption is queued and unpumped when the
		// service goes out of scope.
		backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
		ASSERT_FALSE(record->body_ran);
	}

	// The handle is gone. The resumption releases its permit against io_shared, which
	// the operation's own co-owners (its block and the chain's frame, both of which hold
	// a reference) are keeping alive now. This does NOT separate those two from each
	// other; it shows that file_io is not what owns the state, which is the property
	// that matters. AddressSanitizer is the instrument that reports the release if it
	// touches freed memory.
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body. See the death tests in test_waiter_list.cpp, whose suppression carries the measurement.
TEST(FileIoDeathTest, DestroyingAFileIoWithAnOperationParkedOnASlotAborts)
{
	if(!alt_test::asserts_are_compiled_in)
	{
		GTEST_SKIP() << "NDEBUG compiles out the assert in ~file_io that this test observes.";
	}

	if(alt_test::thread_sanitizer_is_active)
	{
		GTEST_SKIP() << alt_test::thread_sanitizer_forks_death_tests << ' ' << alt_test::debug_and_asan_presets_run_it;
	}

	const alt_test::scoped_death_test_style style{"threadsafe"};

	EXPECT_DEATH(destroy_a_service_with_an_operation_parked(), "parked on a backpressure slot");
}

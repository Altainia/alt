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
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
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
	 * A backend that behaves like a worker thread that is late: it keeps the raw `in` and
	 * `out` pointers it was handed, deliberately WITHOUT the request's `state`, and only
	 * dereferences them in finish(), which a test calls after the awaiting chain has been
	 * cancelled and destroyed. Whatever keeps those pointers valid then is the operation
	 * block's co-ownership and nothing else, which is the property under test; under
	 * AddressSanitizer a pointer into a destroyed frame is a heap-use-after-free here.
	 */
	class late_worker_backend final: public alt::io_backend
	{
	public:
		[[nodiscard]] std::expected<alt::io_token, std::error_code> submit(const alt::io_request& request, const alt::io_completion_fn on_done, void* const state) override
		{
			m_op      = request.op;
			m_in      = request.in;
			m_out     = request.out;
			m_on_done = on_done;
			m_state   = state;
			return alt::io_token{1};
		}

		void cancel(const alt::io_token /*token*/) noexcept override
		{
			m_cancel_requested = true;
		}

		[[nodiscard]] alt::io_backend_kind kind() const noexcept override
		{
			return alt::io_backend_kind::manual;
		}

		[[nodiscard]] bool submitted() const noexcept
		{
			return m_on_done != nullptr;
		}

		[[nodiscard]] bool cancel_requested() const noexcept
		{
			return m_cancel_requested;
		}

		/** What finish() read through `in`, as text, so a test can check it was intact. */
		[[nodiscard]] const std::string& observed() const noexcept
		{
			return m_observed;
		}

		/** Does the work the real backend would, through the raw pointers, then reports success. */
		void finish()
		{
			switch(m_op)
			{
			case alt::io_op::stat_path:
				m_observed                             = static_cast<const std::filesystem::path*>(m_in)->string();
				*static_cast<alt::file_status*>(m_out) = alt::file_status{.size = 42, .mtime_ns = 0, .kind = alt::file_kind::regular, .identity = {}};
				break;
			case alt::io_op::list_directory:
				m_observed = static_cast<const std::filesystem::path*>(m_in)->string();
				static_cast<std::vector<alt::directory_entry>*>(m_out)->push_back(alt::directory_entry{.name = "late", .kind = alt::file_kind::regular});
				break;
			case alt::io_op::unlink:
				m_observed = static_cast<const std::filesystem::path*>(m_in)->string();
				break;
			case alt::io_op::rename:
				m_observed = static_cast<const alt::io_rename_args*>(m_in)->from.string() + ">" + static_cast<const alt::io_rename_args*>(m_in)->to.string();
				break;
			default:
				ADD_FAILURE() << "an operation this backend does not model was submitted";
				break;
			}
			m_on_done(m_state, alt::io_completion{});
		}

	private:
		alt::io_op            m_op               = alt::io_op::stat_path;
		const void*           m_in               = nullptr;
		void*                 m_out              = nullptr;
		alt::io_completion_fn m_on_done          = nullptr;
		void*                 m_state            = nullptr;
		bool                  m_cancel_requested = false;
		std::string           m_observed;
	};

	/**
	 * Starts @p work against a late_worker_backend, cancels the chain once the operation
	 * has reached the backend, lets the destroyed frame go, and only then runs the
	 * backend's work. Returns what the backend read through `in`.
	 */
	template<typename T, typename MakeTask>
	std::string finish_after_the_chain_is_cancelled(MakeTask make_task)
	{
		auto                  created = alt::thread_pool::create(1);
		alt::thread_pool&     pool    = pool_of(created);
		alt::manual_scheduler home;
		late_worker_backend   backend;
		const auto            io = alt::file_io::create(pool, alt::scheduler_ref{home}, backend);
		EXPECT_TRUE(io.has_value());

		const auto               state = std::make_shared<outcome<T>>();
		alt::cancellation_handle chain = alt::start(capture(make_task(*io), state));
		EXPECT_TRUE(backend.submitted()) << "the operation never reached the backend";

		EXPECT_TRUE(chain.request_stop());
		home.drain();
		EXPECT_TRUE(backend.cancel_requested());
		EXPECT_FALSE(state->finished) << "a cancelled chain is abandoned, not resumed";

		backend.finish();
		home.drain();
		return backend.observed();
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

TEST_F(FileIoPathTest, StatOfAMissingPathReportsNoSuchFileOrDirectory)
{
	const auto status = run(io().stat(dir() / "never_existed.txt"));

	ASSERT_FALSE(status.has_value());
	EXPECT_EQ(status.error().default_error_condition(), std::make_error_condition(std::errc::no_such_file_or_directory));
}

TEST_F(FileIoPathTest, RenameOfAMissingSourceReportsNoSuchFileOrDirectoryAndCreatesNothing)
{
	const auto to = dir() / "target.txt";

	const auto renamed = run(io().rename(dir() / "never_existed.txt", to));

	ASSERT_FALSE(renamed.has_value());
	EXPECT_EQ(renamed.error().default_error_condition(), std::make_error_condition(std::errc::no_such_file_or_directory));
	EXPECT_FALSE(std::filesystem::exists(to));
}

TEST_F(FileIoPathTest, AFailedRenameLeavesAnExistingTargetUntouched)
{
	const auto target = write_file("target.txt", "keep me");

	const auto renamed = run(io().rename(dir() / "never_existed.txt", target));

	ASSERT_FALSE(renamed.has_value());
	EXPECT_EQ(read_whole(target), "keep me");
}

TEST_F(FileIoPathTest, ListDirectoryReportsALinkAsALinkAndNotAsItsTarget)
{
#ifdef _WIN32
	GTEST_SKIP() << "Creating a symbolic link on Windows needs a privilege a test runner may not hold.";
#else
	write_file("target.txt", "12345");
	std::filesystem::create_symlink("target.txt", dir() / "link");
	std::filesystem::create_symlink("nowhere", dir() / "dangling");

	auto listed = run(io().list_directory(dir()));

	ASSERT_TRUE(listed.has_value());
	std::ranges::sort(*listed, {}, &alt::directory_entry::name);
	ASSERT_EQ(listed->size(), 3U);
	EXPECT_EQ((*listed)[0].name, "dangling");
	EXPECT_EQ((*listed)[0].kind, alt::file_kind::symlink);
	EXPECT_EQ((*listed)[1].name, "link");
	EXPECT_EQ((*listed)[1].kind, alt::file_kind::symlink);
	EXPECT_EQ((*listed)[2].name, "target.txt");
	EXPECT_EQ((*listed)[2].kind, alt::file_kind::regular);
#endif
}

TEST_F(FileIoPathTest, StatFollowsALinkAndADanglingLinkIsAnError)
{
#ifdef _WIN32
	GTEST_SKIP() << "Creating a symbolic link on Windows needs a privilege a test runner may not hold.";
#else
	write_file("target.txt", "12345");
	std::filesystem::create_symlink("target.txt", dir() / "link");
	std::filesystem::create_symlink("nowhere", dir() / "dangling");

	const auto followed = run(io().stat(dir() / "link"));
	const auto dangling = run(io().stat(dir() / "dangling"));

	ASSERT_TRUE(followed.has_value());
	EXPECT_EQ(followed->kind, alt::file_kind::regular);
	EXPECT_EQ(followed->size, 5U);
	ASSERT_FALSE(dangling.has_value());
	EXPECT_EQ(dangling.error().default_error_condition(), std::make_error_condition(std::errc::no_such_file_or_directory));
#endif
}

TEST_F(FileIoPathTest, UnlinkOfALinkRemovesTheLinkAndLeavesItsTarget)
{
#ifdef _WIN32
	GTEST_SKIP() << "Creating a symbolic link on Windows needs a privilege a test runner may not hold.";
#else
	const auto target = write_file("target.txt", "12345");
	const auto link   = dir() / "link";
	std::filesystem::create_symlink("target.txt", link);

	const auto removed = run(io().unlink(link));

	ASSERT_TRUE(removed.has_value());
	EXPECT_FALSE(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
	EXPECT_EQ(read_whole(target), "12345");
#endif
}

TEST(FileIoPathLifetimeTest, ACancelledStatStillHasItsPathAndResultSlotWhenTheBackendFinishesLate)
{
	const std::string seen = finish_after_the_chain_is_cancelled<std::expected<alt::file_status, std::error_code>>([](const alt::file_io& io) { return io.stat("/stat/path/that/outlives/the/frame"); });

	EXPECT_EQ(seen, "/stat/path/that/outlives/the/frame");
}

TEST(FileIoPathLifetimeTest, ACancelledListDirectoryStillHasItsPathAndResultSlotWhenTheBackendFinishesLate)
{
	const std::string seen = finish_after_the_chain_is_cancelled<std::expected<std::vector<alt::directory_entry>, std::error_code>>([](const alt::file_io& io) { return io.list_directory("/list/path/that/outlives/the/frame"); });

	EXPECT_EQ(seen, "/list/path/that/outlives/the/frame");
}

TEST(FileIoPathLifetimeTest, ACancelledUnlinkStillHasItsPathWhenTheBackendFinishesLate)
{
	const std::string seen = finish_after_the_chain_is_cancelled<std::expected<void, std::error_code>>([](const alt::file_io& io) { return io.unlink("/unlink/path/that/outlives/the/frame"); });

	EXPECT_EQ(seen, "/unlink/path/that/outlives/the/frame");
}

TEST(FileIoPathLifetimeTest, ACancelledRenameStillHasBothPathsWhenTheBackendFinishesLate)
{
	const std::string seen = finish_after_the_chain_is_cancelled<std::expected<void, std::error_code>>([](const alt::file_io& io) { return io.rename("/rename/from/that/outlives/the/frame", "/rename/to/that/outlives/the/frame"); });

	EXPECT_EQ(seen, "/rename/from/that/outlives/the/frame>/rename/to/that/outlives/the/frame");
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

namespace
{

	/** A pool, a completion scheduler and a service over them, sized by the test that needs it. */
	struct run_rig
	{
		run_rig(const std::size_t threads, const std::size_t max_in_flight): pool(alt::thread_pool::create(threads)), io(make_io(max_in_flight))
		{}

		std::expected<alt::file_io, std::error_code> make_io(const std::size_t max_in_flight)
		{
			if(!pool.has_value())
			{
				return std::unexpected(pool.error()); // the fixture's ASSERT on the pool reports it; do not dereference
			}
			return alt::file_io::create(*pool, alt::scheduler_ref{home}, alt::file_io_options{.backend = alt::io_backend_kind::automatic, .max_in_flight = max_in_flight, .queue_depth = 0});
		}

		std::expected<alt::thread_pool, std::error_code> pool;
		alt::manual_scheduler                            home;
		std::expected<alt::file_io, std::error_code>     io;
	};

	/** Polls @p ready until it holds, failing the test rather than hanging past the deadline. */
	template<typename Predicate>
	bool wait_until(Predicate ready)
	{
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{10};
		while(!ready())
		{
			if(std::chrono::steady_clock::now() > limit)
			{
				ADD_FAILURE() << "the condition did not hold within 10 seconds";
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		return true;
	}

	/** A latch a worker waits on until the test opens it. */
	class gate
	{
	public:
		void wait()
		{
			m_open.wait();
		}

		void open()
		{
			m_released.set_value();
		}

	private:
		std::promise<void>       m_released;
		std::shared_future<void> m_open = m_released.get_future().share();
	};

	/** What the sentinel below reports, shared with the test that owns it. */
	struct sentinel_record
	{
		std::atomic<bool> destroyed{false};
		std::thread::id   destroyed_on;
	};

	/**
	 * A move-only capture that notes which thread destroyed it. A moved-from one records
	 * nothing, so the thread it names is the one that destroyed the LIVE object.
	 */
	class thread_sentinel
	{
	public:
		explicit thread_sentinel(std::shared_ptr<sentinel_record> record) noexcept: m_record(std::move(record))
		{}

		thread_sentinel(thread_sentinel&& other) noexcept: m_record(std::move(other.m_record))
		{}

		thread_sentinel(const thread_sentinel&)            = delete;
		thread_sentinel& operator=(const thread_sentinel&) = delete;
		thread_sentinel& operator=(thread_sentinel&&)      = delete;

		~thread_sentinel()
		{
			if(m_record)
			{
				m_record->destroyed_on = std::this_thread::get_id();
				m_record->destroyed.store(true);
			}
		}

	private:
		std::shared_ptr<sentinel_record> m_record;
	};

	/** What counted_result reports, shared with the test that owns it. */
	struct result_counters
	{
		std::atomic<int> destroyed{0};
		std::atomic<int> moves{0};
	};

	/**
	 * A result type that counts its moves and its live destructions. A moved-from one counts
	 * nothing, so `destroyed` is the number of LIVE objects that died.
	 */
	struct counted_result
	{
		explicit counted_result(std::shared_ptr<result_counters> counters) noexcept: m_counters(std::move(counters))
		{}

		counted_result(counted_result&& other) noexcept: m_counters(std::move(other.m_counters))
		{
			m_counters->moves.fetch_add(1);
		}

		counted_result(const counted_result&)            = delete;
		counted_result& operator=(const counted_result&) = delete;
		counted_result& operator=(counted_result&&)      = delete;

		~counted_result()
		{
			if(m_counters)
			{
				m_counters->destroyed.fetch_add(1);
			}
		}

	private:
		std::shared_ptr<result_counters> m_counters;
	};

	/** Awaits @p work and reports the exception's message, or "none" when it completed. */
	alt::task<std::string> message_of_the_exception(alt::task<std::expected<int, std::error_code>> work)
	{
		try
		{
			static_cast<void>(co_await std::move(work));
		}
		catch(const std::runtime_error& error)
		{
			co_return std::string{error.what()};
		}
		co_return std::string{"none"};
	}

	class FileIoRunTest: public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			start_rig(1, 64);
		}

		void TearDown() override
		{
			m_rig->home.drain();
		}

		void start_rig(const std::size_t threads, const std::size_t max_in_flight)
		{
			m_rig = std::make_unique<run_rig>(threads, max_in_flight);
			ASSERT_TRUE(m_rig->pool.has_value());
			ASSERT_TRUE(m_rig->io.has_value());
		}

		alt::file_io& io()
		{
			return *m_rig->io;
		}

		alt::thread_pool& pool()
		{
			return *m_rig->pool;
		}

		alt::manual_scheduler& home()
		{
			return m_rig->home;
		}

		/** Starts @p work and keeps what it produced for result(); nothing is pumped. */
		template<typename T>
		std::shared_ptr<outcome<T>> begin(alt::task<T> work)
		{
			auto state = std::make_shared<outcome<T>>();
			m_chains.push_back(alt::start(capture(std::move(work), state)));
			return state;
		}

		/** Runs the completion scheduler until @p state has finished. */
		template<typename T>
		T finish(const std::shared_ptr<outcome<T>>& state)
		{
			EXPECT_TRUE(wait_until([&] {
				home().run_one();
				return state->finished;
			}));
			std::optional<T>& produced = state->result;
			if(!produced.has_value())
			{
				ADD_FAILURE() << "the task finished without a result";
				std::terminate();
			}
			return std::move(*produced);
		}

		template<typename T>
		T run(alt::task<T> work)
		{
			return finish(begin(std::move(work)));
		}

		/**
		 * Occupies the only worker with a job that waits on @p hold, and returns once it is
		 * running. Records the worker's thread id in @p worker.
		 */
		std::shared_ptr<outcome<std::expected<int, std::error_code>>> occupy_the_worker(const std::shared_ptr<gate>& hold, const std::shared_ptr<std::atomic<std::thread::id>>& worker, const std::shared_ptr<std::atomic<bool>>& entered)
		{
			auto blocker = begin(io().run([hold, worker, entered] {
				worker->store(std::this_thread::get_id());
				entered->store(true);
				hold->wait();
				return 0;
			}));
			wait_until([&] { return entered->load(); });
			return blocker;
		}

		/** The chain begun @p index-th (counting from zero), for the test to stop. */
		alt::cancellation_handle& chain(const std::size_t index)
		{
			return m_chains.at(index);
		}

	private:
		// Declared rig first so the chains, destroyed first, never outlive the service they hold.
		std::unique_ptr<run_rig>              m_rig;
		std::vector<alt::cancellation_handle> m_chains;
	};

}

TEST_F(FileIoRunTest, RunReturnsTheCallablesValue)
{
	const auto result = run(io().run([] { return 42; }));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, 42);
}

TEST_F(FileIoRunTest, RunOfAVoidCallableYieldsAnExpectedVoid)
{
	const auto ran = std::make_shared<std::atomic<bool>>(false);

	const std::expected<void, std::error_code> result = run(io().run([ran] { ran->store(true); }));

	EXPECT_TRUE(result.has_value());
	EXPECT_TRUE(ran->load());
}

TEST_F(FileIoRunTest, RunExecutesTheCallableOnAWorkerNotTheCallersThread)
{
	// The pool's own worker, identified by posting to the pool directly rather than through run().
	std::atomic<std::thread::id> pool_worker{};
	std::atomic<bool>            posted{false};
	struct probe
	{
		std::atomic<std::thread::id>* id;
		std::atomic<bool>*            done;
	} const where{.id = &pool_worker, .done = &posted};
	pool().post(alt::work_item{.invoke = [](void* raw) noexcept {
		                           const auto& target = *static_cast<const probe*>(raw);
		                           target.id->store(std::this_thread::get_id());
		                           target.done->store(true);
	                           },
	                           .state = const_cast<probe*>(&where)}); // NOLINT(cppcoreguidelines-pro-type-const-cast): work_item::state is void* and the probe is only read
	ASSERT_TRUE(wait_until([&] { return posted.load(); }));

	const auto result = run(io().run([] { return std::this_thread::get_id(); }));

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, pool_worker.load());
	EXPECT_NE(*result, std::this_thread::get_id());
}

TEST_F(FileIoRunTest, AnExceptionFromTheCallableIsRethrownFromTheCoAwait)
{
	const std::string seen = run(message_of_the_exception(io().run([]() -> int { throw std::runtime_error{"thrown by the callable"}; })));

	EXPECT_EQ(seen, "thrown by the callable");
}

TEST_F(FileIoRunTest, ACallableCancelledBeforeItRanIsNeverInvoked)
{
	const auto hold    = std::make_shared<gate>();
	const auto worker  = std::make_shared<std::atomic<std::thread::id>>();
	const auto entered = std::make_shared<std::atomic<bool>>(false);
	const auto invoked = std::make_shared<std::atomic<bool>>(false);
	const auto behind  = std::make_shared<std::atomic<bool>>(false);

	const auto blocker = occupy_the_worker(hold, worker, entered);
	// Load-bearing: the one-thread pool runs its queue in FIFO order, so `behind` starting
	// means the cancelled job's turn has passed. A LIFO or work-stealing pool would turn this
	// into a race that can pass with the defect present.
	begin(io().run([invoked] {
		invoked->store(true);
		return 1;
	}));
	// Queued behind the job that holds the worker, so it is still waiting to start.
	begin(io().run([behind] {
		behind->store(true);
		return 2;
	}));
	EXPECT_TRUE(chain(1).request_stop());

	hold->open();
	wait_until([&] { return behind->load(); });

	EXPECT_FALSE(invoked->load()) << "a callable cancelled before it started must not run";
	EXPECT_TRUE(finish(blocker).has_value());
}

TEST_F(FileIoRunTest, ACallablesCapturesAreDestroyedOnTheWorkerWhenCancelled)
{
	const auto hold    = std::make_shared<gate>();
	const auto worker  = std::make_shared<std::atomic<std::thread::id>>();
	const auto entered = std::make_shared<std::atomic<bool>>(false);
	const auto behind  = std::make_shared<std::atomic<bool>>(false);
	const auto record  = std::make_shared<sentinel_record>();

	const auto blocker = occupy_the_worker(hold, worker, entered);
	begin(io().run([sentinel = thread_sentinel{record}] { return 1; }));
	begin(io().run([behind] {
		behind->store(true);
		return 2;
	}));
	// Stopped, but the completion scheduler is NOT pumped: the teardown this posts holds the
	// last other reference to the operation block, and would drop it on this thread.
	EXPECT_TRUE(chain(1).request_stop());

	hold->open();
	wait_until([&] { return behind->load(); });

	EXPECT_TRUE(record->destroyed.load()) << "the cancelled callable's captures were not destroyed when the worker dropped the job";
	EXPECT_EQ(record->destroyed_on, worker->load());
	EXPECT_NE(record->destroyed_on, std::this_thread::get_id());
	EXPECT_TRUE(finish(blocker).has_value());
}

TEST_F(FileIoRunTest, ACallableCancelledWhileRunningCompletesAndItsResultIsDiscarded)
{
	const auto hold     = std::make_shared<gate>();
	const auto entered  = std::make_shared<std::atomic<bool>>(false);
	const auto finished = std::make_shared<std::atomic<bool>>(false);
	const auto counters = std::make_shared<result_counters>();

	const auto state = begin(io().run([hold, entered, finished, counters] {
		entered->store(true);
		hold->wait();
		finished->store(true);
		return counted_result{counters};
	}));
	wait_until([&] { return entered->load(); });
	EXPECT_TRUE(chain(0).request_stop());
	home().drain();

	hold->open();
	wait_until([&] { return finished->load(); });

	// The result was moved into the block's storage exactly once (a callable whose result
	// were dropped instead of stored would move it zero times), and that stored object dies
	// with the block once the worker is done with the job.
	EXPECT_TRUE(wait_until([&] { return counters->destroyed.load() == 1; }));
	EXPECT_EQ(counters->moves.load(), 1);
	home().drain();
	EXPECT_EQ(counters->destroyed.load(), 1);
	EXPECT_FALSE(state->finished) << "a cancelled chain is abandoned, not resumed";
}

TEST_F(FileIoRunTest, RunConsumesAnInFlightSlot)
{
	start_rig(2, 1);
	const auto hold    = std::make_shared<gate>();
	const auto worker  = std::make_shared<std::atomic<std::thread::id>>();
	const auto entered = std::make_shared<std::atomic<bool>>(false);
	const auto second  = std::make_shared<std::atomic<bool>>(false);

	const auto first = occupy_the_worker(hold, worker, entered);
	const auto next  = begin(io().run([second] {
    second->store(true);
    return 7;
  }));

	// A second worker is idle, so only the slot bound can be what holds the second job back.
	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	EXPECT_FALSE(second->load()) << "the second operation ran while the first held the only slot";

	hold->open();
	EXPECT_TRUE(finish(first).has_value());
	const auto second_result = finish(next);
	EXPECT_TRUE(second->load());
	ASSERT_TRUE(second_result.has_value());
	EXPECT_EQ(*second_result, 7);
}

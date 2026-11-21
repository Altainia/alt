#include <gtest/gtest.h>

#include <algorithm>
#include <alt/async_file.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <semaphore>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
// windows.h defines min and max as macros otherwise; see src/io_windows.hpp.
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

#include "file_io_test_support.hpp"

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

	/** Parks the awaiting coroutine and hands its handle out, so a test can resume it at a chosen moment. */
	struct park
	{
		std::coroutine_handle<>* where;

		// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the compiler calls await_ready() through an instance at every co_await of this type.
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(const std::coroutine_handle<> handle) const noexcept
		{
			*where = handle;
		}

		void await_resume() const noexcept
		{}
	};

	/** Requests the awaiting chain's stop from inside the chain, which is the only way to land it between two awaits. */
	struct stop_this_chain
	{
		alt::cancellation_handle* chain;

		[[nodiscard]] bool await_ready() const noexcept
		{
			static_cast<void>(chain->request_stop());
			return true;
		}

		void await_suspend(std::coroutine_handle<> /*handle*/) const noexcept
		{}

		void await_resume() const noexcept
		{}
	};

	/**
	 * The file layer's own two awaits, with a stop requested between them. Written out here
	 * rather than reached through async_file because nothing in the public surface can land
	 * a stop at that exact point: it needs the request made from inside the chain. The
	 * four operations in async_file.cpp have no line of their own that this test could
	 * guard, since the types leave nothing else to get wrong (see io_permit).
	 */
	alt::cancellable_task permit_then_stop_then_submit(std::shared_ptr<alt::detail::io_shared> owner, std::shared_ptr<alt::detail::io_handle> file, std::coroutine_handle<>* parked, alt::cancellation_handle* chain, std::shared_ptr<bool> submitted)
	{
		co_await park{parked};

		auto permit = co_await alt::detail::acquire_permit(owner);
		if(!permit)
		{
			co_return;
		}

		co_await stop_this_chain{chain};
		// The cancellation check ahead of this await destroys the frame before the awaiter
		// is called, so nothing below runs. The permit is in the awaiter's argument, which the
		// frame destroys with everything else, and that is what returns it.
		static_cast<void>(co_await alt::detail::io_awaiter{owner, alt::io_request{.op = alt::io_op::sync_full, .handle = file->native(), .buffer = {}, .offset = 0, .size = 0, .in = nullptr, .out = nullptr, .state = nullptr}, file, std::move(*permit)});
		*submitted = true;
	}

	/** Collects one completion from a backend callback. */
	struct completion_waiter
	{
		std::binary_semaphore done{0};
		alt::io_completion    result{};
	};

	void deliver(void* state, const alt::io_completion result) noexcept
	{
		auto& waiter  = *static_cast<completion_waiter*>(state);
		waiter.result = result;
		waiter.done.release();
	}

	std::string text_of(const alt::io_buffer& buffer, const std::size_t count)
	{
		return {reinterpret_cast<const char*>(buffer.bytes().data()), count};
	}

	alt::io_buffer buffer_of(const std::size_t size)
	{
		auto buffer = alt::io_buffer::allocate(size);
		EXPECT_TRUE(buffer.has_value());
		return std::move(*buffer);
	}

	alt::io_buffer buffer_holding(const std::string& text)
	{
		alt::io_buffer buffer = buffer_of(text.size());
		std::memcpy(buffer.bytes().data(), text.data(), text.size());
		return buffer;
	}

	/**
	 * The service twice over: one on the backend `automatic` selects, which does real I/O
	 * (IOCP on Windows, the thread pool elsewhere), and one on a manual backend, which lets a
	 * test see exactly what was submitted and when. Both complete onto the same manual
	 * scheduler, which a test drives itself. A test that needs one particular real backend
	 * builds it with make_service().
	 */
	class AsyncFileTest: public ::testing::Test
	{
	protected:
		void TearDown() override
		{
			finish_manual_operations();
		}

		void SetUp() override
		{
			static std::atomic<int> counter{0};
			m_dir.path = std::filesystem::temp_directory_path() / ("alt_async_file_" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
			std::filesystem::create_directories(m_dir.path);
			ASSERT_TRUE(m_pool.has_value());
			ASSERT_TRUE(m_real.has_value());
			ASSERT_TRUE(m_manual.has_value());
		}

		[[nodiscard]] const std::filesystem::path& dir() const
		{
			return m_dir.path;
		}

		alt::thread_pool& pool()
		{
			return *m_pool;
		}

		alt::manual_scheduler& home()
		{
			return m_home;
		}

		/** The service that performs real I/O. */
		alt::file_io& real_io()
		{
			return *m_real;
		}

		/**
		 * A further real service on the fixture's pool and scheduler, on the backend @p kind
		 * names. The caller keeps it in a local, so it is destroyed before the fixture's pool.
		 */
		std::expected<alt::file_io, std::error_code> make_service(const alt::io_backend_kind kind)
		{
			return alt::file_io::create(*m_pool, alt::scheduler_ref{m_home}, alt::file_io_options{.backend = kind, .max_in_flight = 64, .queue_depth = 0});
		}

		/** The service whose backend does nothing until the test completes an operation by hand. */
		alt::file_io& manual_io()
		{
			return *m_manual;
		}

		alt::manual_io_backend& manual_backend()
		{
			return m_manual_backend;
		}

		/** How long a test waits for anything before it calls the wait a failure. */
		static constexpr std::chrono::seconds deadline{10};

		/**
		 * Runs the completion scheduler until @p done reports true or the deadline passes.
		 *
		 * Polls rather than blocks, because a blocking pump is exactly how a defect that
		 * submits when it should not turns into a hung CI job instead of a failed test.
		 * Returns whether @p done became true.
		 */
		template<typename Predicate>
		bool pump_until(Predicate done)
		{
			const auto limit = std::chrono::steady_clock::now() + deadline;
			while(!done())
			{
				if(!m_home.run_one())
				{
					if(std::chrono::steady_clock::now() > limit)
					{
						return false;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds{1});
				}
			}
			return true;
		}

		/**
		 * Runs @p work to completion, driving the completion scheduler, and fails by
		 * assertion rather than hanging if it does not finish.
		 *
		 * On expiry every operation still at the manual backend is completed with an error
		 * so that the fixture's teardown, which waits for the backend to go quiet, returns.
		 */
		template<typename T>
		T run(alt::task<T> work)
		{
			const auto                     state = std::make_shared<outcome<T>>();
			const alt::cancellation_handle chain = alt::start(capture(std::move(work), state));
			if(!pump_until([&] { return state->finished; }))
			{
				ADD_FAILURE() << "the operation did not finish within " << deadline.count() << " seconds";
				finish_manual_operations();
				return std::unexpected(std::make_error_code(std::errc::timed_out));
			}
			std::optional<T>& produced = state->result;
			if(produced.has_value())
			{
				return std::move(*produced);
			}
			return std::unexpected(std::make_error_code(std::errc::timed_out));
		}

		/** Completes operation @p index at the manual backend, remembering that it did. */
		void complete_by_hand(const std::size_t index, const alt::io_completion result)
		{
			m_completed.insert(index);
			m_manual_backend.complete(index, result);
		}

		/** Completes every operation still pending at the manual backend with an error. */
		void finish_manual_operations()
		{
			for(std::size_t index = 0; m_manual_backend.pending() > 0; ++index)
			{
				if(!m_completed.contains(index))
				{
					complete_by_hand(index, alt::io_completion{.bytes = 0, .error = std::make_error_code(std::errc::timed_out)});
				}
			}
			home().drain();
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

		/**
		 * Opens @p path directly through a backend and returns the raw handle, which is how
		 * a test gets a descriptor to adopt onto the MANUAL service, whose own backend
		 * cannot open anything.
		 */
		alt::native_handle open_native(const std::filesystem::path& path, const alt::open_flags how)
		{
			const std::unique_ptr<alt::io_backend> backend = alt::make_thread_pool_backend(*m_pool);
			const alt::io_open_args                args{.path = path, .open_flags = how, .mode = {}};
			alt::native_handle                     handle = alt::invalid_native_handle;
			completion_waiter                      waiter;
			alt::io_request                        request;
			request.op       = alt::io_op::open;
			request.in       = &args;
			request.out      = &handle;
			const auto token = backend->submit(request, deliver, &waiter);
			EXPECT_TRUE(token.has_value());
			waiter.done.acquire();
			EXPECT_FALSE(waiter.result.error);
			return handle;
		}

		/**
		 * Whether @p handle still names an open descriptor, asked of the operating system.
		 *
		 * Builds a fresh thread-pool backend for every probe and does not open anything, so
		 * the answer is the operating system's. That is also its weakness: a descriptor
		 * number the system has since recycled would read as open. Nothing here opens a file
		 * while a test runs, but a future backend that consumed a descriptor on construction
		 * could take the recycled number and make the three tests that call this flaky.
		 */
		bool descriptor_is_open(const alt::native_handle handle)
		{
			const std::unique_ptr<alt::io_backend> backend = alt::make_thread_pool_backend(*m_pool);
			alt::file_status                       status;
			completion_waiter                      waiter;
			alt::io_request                        request;
			request.op       = alt::io_op::stat_handle;
			request.handle   = handle;
			request.out      = &status;
			const auto token = backend->submit(request, deliver, &waiter);
			EXPECT_TRUE(token.has_value());
			waiter.done.acquire();
			return !waiter.result.error;
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

		// Declaration order is destruction order reversed: the services go first, then the
		// backend and scheduler they use, then the pool they borrow, then the directory.
		scratch_dir                                      m_dir;
		std::set<std::size_t>                            m_completed;
		std::expected<alt::thread_pool, std::error_code> m_pool = alt::thread_pool::create(1);
		alt::manual_scheduler                            m_home;
		alt::manual_io_backend                           m_manual_backend;
		std::expected<alt::file_io, std::error_code>     m_real   = alt::file_io::create(*m_pool, alt::scheduler_ref{m_home}, alt_test::file_io_options_under_test());
		std::expected<alt::file_io, std::error_code>     m_manual = alt::file_io::create(alt::scheduler_ref{m_home}, m_manual_backend);
	};

}

TEST_F(AsyncFileTest, ReadAtYieldsTheBytesAtTheOffset)
{
	const auto path = write_file("data.txt", "0123456789");
	auto       file = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(file.has_value());

	const alt::io_buffer buffer = buffer_of(4);
	const auto           read   = run(file->read_at(buffer, 3, 4));

	ASSERT_TRUE(read.has_value());
	EXPECT_EQ(*read, 4U);
	EXPECT_EQ(text_of(buffer, 4), "3456");
}

TEST_F(AsyncFileTest, AReadThatRunsPastTheEndCompletesShortAndAtTheEndWithZero)
{
	const auto path = write_file("data.txt", "0123456789");
	auto       file = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(file.has_value());

	const alt::io_buffer buffer     = buffer_of(8);
	const auto           short_read = run(file->read_at(buffer, 8, 8));
	const auto           at_end     = run(file->read_at(buffer, 10, 8));

	ASSERT_TRUE(short_read.has_value());
	EXPECT_EQ(*short_read, 2U);
	EXPECT_EQ(text_of(buffer, 2), "89");
	ASSERT_TRUE(at_end.has_value());
	EXPECT_EQ(*at_end, 0U);
}

TEST_F(AsyncFileTest, WriteAtThenReadAtRoundTrips)
{
	const auto path = dir() / "created.bin";
	auto       file = run(real_io().open(path, alt::flags(alt::open_flag::read_write) | alt::flags(alt::open_flag::create)));
	ASSERT_TRUE(file.has_value());

	const auto written = run(file->write_at(buffer_holding("hello world"), 0, 11));
	ASSERT_TRUE(written.has_value());
	EXPECT_EQ(*written, 11U);

	const auto overwritten = run(file->write_at(buffer_holding("WORLD"), 6, 5));
	ASSERT_TRUE(overwritten.has_value());
	EXPECT_EQ(*overwritten, 5U);

	const alt::io_buffer back = buffer_of(11);
	const auto           read = run(file->read_at(back, 0, 11));
	ASSERT_TRUE(read.has_value());
	EXPECT_EQ(*read, 11U);
	EXPECT_EQ(text_of(back, 11), "hello WORLD");
	EXPECT_EQ(read_whole(path), "hello WORLD");
}

TEST_F(AsyncFileTest, AZeroLengthReadSucceedsWithZeroBytes)
{
	// The manual backend never completes anything on its own, so a zero-length read that
	// submitted would hang this run instead of returning.
	const auto      path = write_file("data.txt", "abc");
	alt::async_file file = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::read)));

	const auto read = run(file.read_at(buffer_of(4), 0, 0));

	ASSERT_TRUE(read.has_value());
	EXPECT_EQ(*read, 0U);
	EXPECT_EQ(manual_backend().pending(), 0U);
}

TEST_F(AsyncFileTest, AZeroLengthWriteSucceedsWithZeroBytes)
{
	const auto      path = write_file("data.txt", "abc");
	alt::async_file file = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::write)));

	const auto written = run(file.write_at(buffer_of(4), 0, 0));

	ASSERT_TRUE(written.has_value());
	EXPECT_EQ(*written, 0U);
	EXPECT_EQ(manual_backend().pending(), 0U);
}

TEST_F(AsyncFileTest, ASizeLargerThanTheBufferIsRejectedBeforeSubmission)
{
	const auto      path = write_file("data.txt", "0123456789");
	alt::async_file file = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::read_write)));

	const auto read    = run(file.read_at(buffer_of(4), 0, 5));
	const auto written = run(file.write_at(buffer_of(4), 0, 5));

	ASSERT_FALSE(read.has_value());
	EXPECT_EQ(read.error(), std::errc::invalid_argument);
	ASSERT_FALSE(written.has_value());
	EXPECT_EQ(written.error(), std::errc::invalid_argument);
	// A clamp would complete with a short count rather than an error, and would have
	// submitted; the backend seeing nothing is what separates rejection from clamping.
	EXPECT_EQ(manual_backend().pending(), 0U);
	EXPECT_EQ(read_whole(path), "0123456789");
}

TEST_F(AsyncFileTest, ReadAtOnAClosedFileReportsBadFileDescriptor)
{
	const auto      path = write_file("data.txt", "0123456789");
	alt::async_file file = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::read)));

	// Made BEFORE the close, so it holds its own reference to the descriptor and only the
	// closed-by-request flag can stop it. Made after, it would fail for want of a handle.
	auto earlier = file.read_at(buffer_of(4), 0, 4);

	const auto closed = run(file.close());
	ASSERT_TRUE(closed.has_value());
	EXPECT_EQ(file.handle(), alt::invalid_native_handle);

	const auto from_earlier = run(std::move(earlier));
	const auto later        = run(file.read_at(buffer_of(4), 0, 4));

	ASSERT_FALSE(from_earlier.has_value());
	EXPECT_EQ(from_earlier.error(), std::errc::bad_file_descriptor);
	ASSERT_FALSE(later.has_value());
	EXPECT_EQ(later.error(), std::errc::bad_file_descriptor);
	EXPECT_EQ(manual_backend().pending(), 0U);
}

TEST_F(AsyncFileTest, StatReportsSizeKindAndAStableIdentity)
{
	const auto path  = write_file("data.txt", "twelve bytes");
	const auto other = write_file("other.txt", "x");

	auto first  = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	auto second = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	auto third  = run(real_io().open(other, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(third.has_value());
	ASSERT_NE(first->handle(), second->handle());

	const auto status_first  = run(first->stat());
	const auto status_second = run(second->stat());
	const auto status_other  = run(third->stat());

	ASSERT_TRUE(status_first.has_value());
	ASSERT_TRUE(status_second.has_value());
	ASSERT_TRUE(status_other.has_value());
	EXPECT_EQ(status_first->size, 12U);
	EXPECT_EQ(status_first->kind, alt::file_kind::regular);
	EXPECT_GT(status_first->mtime_ns, 0);
	// The property an ETag built from inode, size and mtime depends on.
	EXPECT_EQ(status_first->identity, status_second->identity);
	EXPECT_NE(status_first->identity.serial, 0U);
	// And it identifies the file, not just "something": another file differs.
	EXPECT_NE(status_first->identity, status_other->identity);
}

TEST_F(AsyncFileTest, SyncDataSucceedsOnThisPlatform)
{
	auto file = run(real_io().open(dir() / "synced.bin", alt::flags(alt::open_flag::read_write) | alt::flags(alt::open_flag::create)));
	ASSERT_TRUE(file.has_value());
	ASSERT_TRUE(run(file->write_at(buffer_holding("durable"), 0, 7)).has_value());

	EXPECT_TRUE(run(file->sync(alt::sync_mode::data)).has_value());
	EXPECT_TRUE(run(file->sync(alt::sync_mode::full)).has_value());
	EXPECT_TRUE(run(file->sync()).has_value());
}

TEST_F(AsyncFileTest, TwoPositionalReadsOnOneFileEachGetTheirOwnOffsetsBytes)
{
	const auto path = write_file("data.txt", "AAAABBBBCCCC");
	auto       file = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(file.has_value());

	const alt::io_buffer first_buffer  = buffer_of(4);
	const alt::io_buffer second_buffer = buffer_of(4);
	using result_type                  = std::expected<std::size_t, std::error_code>;
	const auto first                   = std::make_shared<outcome<result_type>>();
	const auto second                  = std::make_shared<outcome<result_type>>();

	const alt::cancellation_handle chain_first  = alt::start(capture(file->read_at(first_buffer, 8, 4), first));
	const alt::cancellation_handle chain_second = alt::start(capture(file->read_at(second_buffer, 0, 4), second));
	ASSERT_TRUE(pump_until([&] { return first->finished && second->finished; }));

	ASSERT_TRUE(first->result.has_value());
	ASSERT_TRUE(second->result.has_value());
	EXPECT_EQ(text_of(first_buffer, 4), "CCCC");
	EXPECT_EQ(text_of(second_buffer, 4), "AAAA");
}

TEST_F(AsyncFileTest, ManyConcurrentReadsAllComplete)
{
	// More reads than the fixture's max_in_flight of 64, so most of them wait for a permit
	// and are submitted as earlier ones complete: every completion must reach its own read,
	// and none may be lost while the backend has a full complement in flight.
	constexpr std::size_t count = 200;
	std::string           content;
	for(std::size_t index = 0; index < count; ++index)
	{
		content.push_back(static_cast<char>(index));
	}
	const auto path = write_file("many.bin", content);
	auto       file = run(real_io().open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(file.has_value());

	using result_type = std::expected<std::size_t, std::error_code>;
	std::vector<alt::io_buffer>                        buffers;
	std::vector<std::shared_ptr<outcome<result_type>>> results;
	std::vector<alt::cancellation_handle>              chains;
	for(std::size_t index = 0; index < count; ++index)
	{
		buffers.push_back(buffer_of(1));
		results.push_back(std::make_shared<outcome<result_type>>());
		chains.push_back(alt::start(capture(file->read_at(buffers.back(), index, 1), results.back())));
	}
	ASSERT_TRUE(pump_until([&] { return std::ranges::all_of(results, [](const auto& result) { return result->finished; }); }));

	for(std::size_t index = 0; index < count; ++index)
	{
		const std::optional<result_type>& read = results[index]->result;
		ASSERT_TRUE(read.has_value()) << "read " << index;
		ASSERT_TRUE(read->has_value()) << "read " << index << ": " << read->error().message();
		EXPECT_EQ(**read, 1U) << "read " << index;
		EXPECT_EQ(text_of(buffers[index], 1), std::string(1, static_cast<char>(index))) << "read " << index;
	}
}

TEST_F(AsyncFileTest, TwoReadsOnOneFileAreBothAtTheBackendTogetherWithTheirOwnOffsets)
{
	const auto      path = write_file("data.txt", "AAAABBBBCCCC");
	alt::async_file file = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::read)));

	using result_type = std::expected<std::size_t, std::error_code>;
	const auto first  = std::make_shared<outcome<result_type>>();
	const auto second = std::make_shared<outcome<result_type>>();

	const alt::cancellation_handle chain_first  = alt::start(capture(file.read_at(buffer_of(4), 8, 4), first));
	const alt::cancellation_handle chain_second = alt::start(capture(file.read_at(buffer_of(4), 0, 4), second));

	ASSERT_EQ(manual_backend().pending(), 2U);
	EXPECT_EQ(manual_backend().request(0).offset, 8U);
	EXPECT_EQ(manual_backend().request(1).offset, 0U);
	EXPECT_EQ(manual_backend().request(0).handle, file.handle());
	EXPECT_EQ(manual_backend().request(1).handle, file.handle());

	// Completed out of submission order: each task gets the outcome of ITS operation.
	complete_by_hand(1, alt::io_completion{.bytes = 3, .error = {}});
	complete_by_hand(0, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();

	ASSERT_TRUE(first->finished);
	ASSERT_TRUE(second->finished);
	EXPECT_TRUE(first->result == result_type{4U});
	EXPECT_TRUE(second->result == result_type{3U});
}

TEST_F(AsyncFileTest, TheDestructorWithAnOutstandingOperationClosesNothing)
{
	const auto path   = write_file("data.txt", "0123456789");
	const auto handle = open_native(path, alt::flags(alt::open_flag::read));

	using result_type               = std::expected<std::size_t, std::error_code>;
	const auto               result = std::make_shared<outcome<result_type>>();
	alt::cancellation_handle chain;
	{
		alt::async_file file = alt::async_file::adopt(manual_io(), handle);
		chain                = alt::start(capture(file.read_at(buffer_of(4), 0, 4), result));
		ASSERT_EQ(manual_backend().pending(), 1U);
	}

	// The file object is gone, and the operation at the backend still holds the descriptor.
	EXPECT_TRUE(descriptor_is_open(handle));

	complete_by_hand(0, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();
	ASSERT_TRUE(result->finished);

	// The last operation to finish dropped the last reference, and that closed it.
	EXPECT_FALSE(descriptor_is_open(handle));
}

TEST_F(AsyncFileTest, TheDestructorWithNothingOutstandingClosesTheDescriptor)
{
	const auto path   = write_file("data.txt", "0123456789");
	const auto handle = open_native(path, alt::flags(alt::open_flag::read));
	{
		const alt::async_file file = alt::async_file::adopt(manual_io(), handle);
		ASSERT_TRUE(descriptor_is_open(handle));
	}

	EXPECT_FALSE(descriptor_is_open(handle));
}

TEST_F(AsyncFileTest, CloseClosesTheDescriptorWhenTheTaskCompletesAndIsIdempotent)
{
	const auto      path   = write_file("data.txt", "0123456789");
	const auto      handle = open_native(path, alt::flags(alt::open_flag::read));
	alt::async_file file   = alt::async_file::adopt(manual_io(), handle);

	EXPECT_TRUE(run(file.close()).has_value());
	EXPECT_FALSE(descriptor_is_open(handle));
	EXPECT_TRUE(run(file.close()).has_value());
	EXPECT_EQ(file.handle(), alt::invalid_native_handle);
}

TEST_F(AsyncFileTest, AnOperationOnAFileAdoptedWithAnInvalidHandleReportsBadFileDescriptor)
{
	const alt::async_file file = alt::async_file::adopt(manual_io(), alt::invalid_native_handle);

	const auto read = run(file.read_at(buffer_of(4), 0, 4));
	const auto sync = run(file.sync());
	const auto stat = run(file.stat());

	ASSERT_FALSE(read.has_value());
	EXPECT_EQ(read.error(), std::errc::bad_file_descriptor);
	ASSERT_FALSE(sync.has_value());
	EXPECT_EQ(sync.error(), std::errc::bad_file_descriptor);
	ASSERT_FALSE(stat.has_value());
	EXPECT_EQ(stat.error(), std::errc::bad_file_descriptor);
	EXPECT_EQ(manual_backend().pending(), 0U);
}

TEST_F(AsyncFileTest, OpenOfAMissingPathReportsNoSuchFileOrDirectory)
{
	const auto opened = run(real_io().open(dir() / "missing.txt", alt::flags(alt::open_flag::read)));

	ASSERT_FALSE(opened.has_value());
	EXPECT_EQ(opened.error(), std::errc::no_such_file_or_directory);
}

namespace
{
	/** The error a finished open failed with; an empty code if it has not finished or succeeded. */
	std::error_code failure_of(const outcome<std::expected<alt::async_file, std::error_code>>& finished)
	{
		if(!finished.result.has_value() || finished.result->has_value())
		{
			return {};
		}
		return finished.result->error();
	}
}

TEST_F(AsyncFileTest, OpenWithNeitherAccessBitIsRejectedWithoutWaitingForAPermit)
{
	// One permit, held by a read in flight, so an open that tried to take a permit before
	// validating would park behind it instead of answering.
	const auto                 path = write_file("data.txt", "0123456789");
	const alt::file_io_options options{.backend = alt::io_backend_kind::automatic, .max_in_flight = 1, .queue_depth = 0};
	const auto                 service = alt::file_io::create(alt::scheduler_ref{home()}, manual_backend(), options);
	ASSERT_TRUE(service.has_value());
	const alt::async_file file = alt::async_file::adopt(*service, open_native(path, alt::flags(alt::open_flag::read)));

	using result_type                           = std::expected<std::size_t, std::error_code>;
	using opened_type                           = std::expected<alt::async_file, std::error_code>;
	const auto                     holder       = std::make_shared<outcome<result_type>>();
	const alt::cancellation_handle holder_chain = alt::start(capture(file.read_at(buffer_of(4), 0, 4), holder));
	ASSERT_EQ(manual_backend().pending(), 1U);
	const auto shared = alt::detail::file_io_access::shared(*service);
	ASSERT_EQ(shared->slots.available(), 0);

	const auto                     none              = std::make_shared<outcome<opened_type>>();
	const auto                     create_only       = std::make_shared<outcome<opened_type>>();
	const alt::cancellation_handle none_chain        = alt::start(capture(service->open(path, alt::open_flags{}), none));
	const alt::cancellation_handle create_only_chain = alt::start(capture(service->open(dir() / "made.txt", alt::flags(alt::open_flag::create)), create_only));
	home().drain();

	// Answered while the only permit is still held: nothing waited, nothing was submitted.
	// An open that took its permit first would be parked here and neither would have finished.
	ASSERT_TRUE(none->finished);
	ASSERT_TRUE(create_only->finished);
	EXPECT_EQ(failure_of(*none), std::errc::invalid_argument);
	EXPECT_EQ(failure_of(*create_only), std::errc::invalid_argument);
	EXPECT_EQ(manual_backend().pending(), 1U);
	EXPECT_EQ(shared->slots.available(), 0);
	EXPECT_FALSE(std::filesystem::exists(dir() / "made.txt"));

	complete_by_hand(0, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();
	EXPECT_TRUE(holder->finished);
	EXPECT_EQ(shared->slots.available(), 1);
}

TEST_F(AsyncFileTest, ReadAtOnADirectoryReportsIsADirectory)
{
	// Opening a directory for reading succeeds on both platforms; it is the transfer that fails.
	auto opened = run(real_io().open(dir(), alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(opened.has_value());

	const auto read = run(opened->read_at(buffer_of(4), 0, 4));

	ASSERT_FALSE(read.has_value());
	EXPECT_EQ(read.error(), std::errc::is_a_directory);
}

TEST_F(AsyncFileTest, OpenWithExclusiveOverAnExistingFileReportsFileExists)
{
	const auto path   = write_file("data.txt", "x");
	const auto opened = run(real_io().open(path, alt::flags(alt::open_flag::write) | alt::flags(alt::open_flag::create) | alt::flags(alt::open_flag::exclusive)));

	ASSERT_FALSE(opened.has_value());
	EXPECT_EQ(opened.error(), std::errc::file_exists);
}

TEST_F(AsyncFileTest, AStopBetweenThePermitWaitAndTheSubmissionReturnsThePermit)
{
	const alt::file_io_options options{.backend = alt::io_backend_kind::automatic, .max_in_flight = 1, .queue_depth = 0};
	const auto                 service = alt::file_io::create(alt::scheduler_ref{home()}, manual_backend(), options);
	ASSERT_TRUE(service.has_value());
	const auto shared = alt::detail::file_io_access::shared(*service);
	const auto file   = alt::detail::io_handle::adopt(alt::invalid_native_handle);

	std::coroutine_handle<>     parked;
	alt::cancellation_handle    chain;
	const std::shared_ptr<bool> submitted = std::make_shared<bool>(false);
	chain                                 = alt::start(permit_then_stop_then_submit(shared, file, &parked, &chain, submitted));
	ASSERT_TRUE(static_cast<bool>(parked));

	parked.resume();
	home().drain();

	EXPECT_FALSE(*submitted);
	EXPECT_EQ(manual_backend().pending(), 0U);
	// The only permit there is. Had the abandoned frame kept it, this would be 0 and
	// every later operation on the service would park forever.
	EXPECT_EQ(shared->slots.available(), 1);
}

TEST_F(AsyncFileTest, AStopWhileParkedOnTheBackpressureLimitDeliversNothingAndLeavesTheLimitUsable)
{
	const auto                 path = write_file("data.txt", "0123456789");
	const alt::file_io_options options{.backend = alt::io_backend_kind::automatic, .max_in_flight = 1, .queue_depth = 0};
	const auto                 service = alt::file_io::create(alt::scheduler_ref{home()}, manual_backend(), options);
	ASSERT_TRUE(service.has_value());
	const alt::async_file file = alt::async_file::adopt(*service, open_native(path, alt::flags(alt::open_flag::read)));

	using result_type = std::expected<std::size_t, std::error_code>;
	const auto holder = std::make_shared<outcome<result_type>>();
	const auto parked = std::make_shared<outcome<result_type>>();
	const auto later  = std::make_shared<outcome<result_type>>();

	const alt::cancellation_handle holder_chain = alt::start(capture(file.read_at(buffer_of(4), 0, 4), holder));
	alt::cancellation_handle       parked_chain = alt::start(capture(file.read_at(buffer_of(4), 0, 4), parked));
	ASSERT_EQ(manual_backend().pending(), 1U);

	ASSERT_TRUE(parked_chain.request_stop());
	home().drain();

	// A cancelled chain is abandoned, not resumed: the operation_canceled the permit wait
	// produces is never delivered to anyone. It also never reached the backend.
	EXPECT_FALSE(parked->finished);
	EXPECT_EQ(manual_backend().pending(), 1U);

	complete_by_hand(0, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();
	ASSERT_TRUE(holder->finished);

	// The permit the stopped waiter never took is still there for the next operation.
	const alt::cancellation_handle later_chain = alt::start(capture(file.read_at(buffer_of(4), 0, 4), later));
	EXPECT_EQ(manual_backend().pending(), 1U);
	complete_by_hand(1, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();
	EXPECT_TRUE(later->finished);

	// Exactly one permit exists: the stopped waiter neither kept one nor returned two.
	const auto shared = alt::detail::file_io_access::shared(*service);
	EXPECT_EQ(shared->slots.available(), 1);
}

TEST_F(AsyncFileTest, AnAdoptedFileReportsTheServicesBackend)
{
	// Only an IOCP service routes adopted files elsewhere; on every other backend an adopted file
	// is served, and reported, exactly as an opened one is.
	const auto path   = write_file("data.txt", "0123456789");
	const auto pooled = make_service(alt::io_backend_kind::thread_pool);
	ASSERT_TRUE(pooled.has_value());

	const alt::async_file adopted = alt::async_file::adopt(*pooled, open_native(path, alt::flags(alt::open_flag::read)));
	EXPECT_EQ(adopted.backend(), alt::io_backend_kind::thread_pool);

	auto opened = run(pooled->open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(opened.has_value());
	EXPECT_EQ(opened->backend(), alt::io_backend_kind::thread_pool);

	const alt::async_file on_manual = alt::async_file::adopt(manual_io(), open_native(path, alt::flags(alt::open_flag::read)));
	EXPECT_EQ(on_manual.backend(), alt::io_backend_kind::manual);

	const alt::async_file empty = alt::async_file::adopt(*pooled, alt::invalid_native_handle);
	EXPECT_EQ(empty.backend(), alt::io_backend_kind::thread_pool);

	// And it is served where it says it is.
	alt::io_buffer       buffer = buffer_of(4);
	const alt::io_buffer view   = buffer;
	const auto           read   = run(adopted.read_at(std::move(buffer), 2, 4));
	ASSERT_TRUE(read.has_value()) << read.error().message();
	EXPECT_EQ(text_of(view, *read), "2345");
}

#ifdef _WIN32
namespace
{
	/** A Win32 handle the test owns until it hands it to adopt(), which then owns it. */
	class win32_handle
	{
	public:
		explicit win32_handle(const HANDLE value) noexcept: m_value(value == INVALID_HANDLE_VALUE ? nullptr : value)
		{}

		win32_handle(const win32_handle&)            = delete;
		win32_handle& operator=(const win32_handle&) = delete;
		win32_handle(win32_handle&&)                 = delete;
		win32_handle& operator=(win32_handle&&)      = delete;

		~win32_handle()
		{
			reset();
		}

		[[nodiscard]] HANDLE get() const noexcept
		{
			return m_value;
		}

		[[nodiscard]] bool valid() const noexcept
		{
			return m_value != nullptr;
		}

		/** Gives the handle up to an owner that will close it. */
		[[nodiscard]] alt::native_handle release() noexcept
		{
			return reinterpret_cast<alt::native_handle>(std::exchange(m_value, nullptr));
		}

		void reset() noexcept
		{
			if(m_value != nullptr)
			{
				static_cast<void>(::CloseHandle(m_value));
				m_value = nullptr;
			}
		}

	private:
		HANDLE m_value;
	};

	/** Opens @p path for reading and writing, as a consumer bringing its own handle might; @p extra is OR'd into the flags. */
	win32_handle open_for_adoption(const std::filesystem::path& path, const DWORD extra)
	{
		return win32_handle{::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | extra, nullptr)};
	}

	/** A pipe name no other test, and no other process, is using. */
	std::wstring unique_pipe_name()
	{
		static std::atomic<int> counter{0};
		return L"\\\\.\\pipe\\alt_async_file_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(counter.fetch_add(1));
	}

	/**
	 * A named pipe whose server end is opened with FILE_FLAG_OVERLAPPED, for the test to adopt,
	 * and whose client end is synchronous and stays with the test. A read on the server end with
	 * nothing written goes pending every time, which a regular file does not promise: a cached
	 * read on an overlapped file handle usually completes synchronously.
	 */
	struct overlapped_pipe
	{
		std::wstring name = unique_pipe_name();
		win32_handle server{::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr)};
		// Opening the client end after the server exists connects the pipe; no ConnectNamedPipe is needed.
		win32_handle client{::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};

		[[nodiscard]] bool valid() const
		{
			return server.valid() && client.valid();
		}

		/** Writes @p bytes into the pipe from the client end. */
		[[nodiscard]] bool write(const std::string& bytes) const
		{
			DWORD written = 0;
			return ::WriteFile(client.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
		}

		/** Reads exactly @p size bytes at the client end, or what arrived before the pipe failed. */
		[[nodiscard]] std::string read(const std::size_t size) const
		{
			std::string bytes(size, '\0');
			std::size_t got = 0;
			while(got < size)
			{
				DWORD count = 0;
				if(::ReadFile(client.get(), bytes.data() + got, static_cast<DWORD>(size - got), &count, nullptr) == FALSE || count == 0)
				{
					break;
				}
				got += count;
			}
			bytes.resize(got);
			return bytes;
		}
	};

	/** What only_worker_of() learns on the worker. Shared, so the job can finish after the wait returns. */
	struct worker_probe
	{
		HANDLE                handle = nullptr;
		std::binary_semaphore done{0};
	};

	/**
	 * A handle to the only worker of @p pool that GetThreadIOPendingFlag can query, learned by
	 * running one job on it. GetCurrentThread() is a pseudo-handle; duplicating it yields a real
	 * one, and the query needs THREAD_QUERY_INFORMATION rather than the limited right.
	 */
	win32_handle only_worker_of(alt::thread_pool& pool)
	{
		const auto probe = std::make_shared<worker_probe>();
		pool.post(alt::work_item{.invoke = +[](void* state) noexcept {
			                         const std::unique_ptr<std::shared_ptr<worker_probe>> owner(static_cast<std::shared_ptr<worker_probe>*>(state));
			                         worker_probe&                                        p = **owner;
			                         if(::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &p.handle, THREAD_QUERY_INFORMATION, FALSE, 0) == FALSE)
			                         {
				                         p.handle = nullptr;
			                         }
			                         p.done.release();
		                         },
		                         .state = new std::shared_ptr<worker_probe>(probe)});
		probe->done.acquire();
		return win32_handle{probe->handle};
	}

	/** Whether @p thread has an I/O request outstanding. Empty when the query fails, which must never read as idle. */
	std::optional<bool> has_pending_io(const HANDLE thread)
	{
		BOOL pending = FALSE;
		if(::GetThreadIOPendingFlag(thread, &pending) == FALSE)
		{
			return std::nullopt;
		}
		return pending != FALSE;
	}

	/** Opens a gate a pool job spins on when the test leaves, however it leaves, so the pool can join. */
	struct open_on_exit
	{
		std::shared_ptr<std::atomic<bool>> gate;

		explicit open_on_exit(std::shared_ptr<std::atomic<bool>> opened) noexcept: gate(std::move(opened))
		{}

		open_on_exit(const open_on_exit&)            = delete;
		open_on_exit& operator=(const open_on_exit&) = delete;
		open_on_exit(open_on_exit&&)                 = delete;
		open_on_exit& operator=(open_on_exit&&)      = delete;

		~open_on_exit()
		{
			gate->store(true);
		}
	};
}

TEST_F(AsyncFileTest, AnOpenedFileReportsIocp)
{
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();

	auto opened = run(service->open(dir() / "opened.txt", alt::flags(alt::open_flag::read) | alt::flags(alt::open_flag::write) | alt::flags(alt::open_flag::create)));
	ASSERT_TRUE(opened.has_value()) << opened.error().message();
	EXPECT_EQ(opened->backend(), alt::io_backend_kind::iocp);

	const auto wrote = run(opened->write_at(buffer_holding("opened"), 0, 6));
	ASSERT_TRUE(wrote.has_value()) << wrote.error().message();
	alt::io_buffer       buffer = buffer_of(6);
	const alt::io_buffer view   = buffer;
	const auto           read   = run(opened->read_at(std::move(buffer), 0, 6));
	ASSERT_TRUE(read.has_value()) << read.error().message();
	EXPECT_EQ(text_of(view, *read), "opened");
}

TEST_F(AsyncFileTest, AnAdoptedSynchronousHandleReportsThreadPoolAndWorks)
{
	// This guards the report, not the routing: a transfer on a synchronous handle completes
	// inline even on the IOCP backend, so it would pass with every adopted file sent there.
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();
	win32_handle handle = open_for_adoption(dir() / "synchronous.txt", 0);
	ASSERT_TRUE(handle.valid()) << ::GetLastError();

	const alt::async_file file = alt::async_file::adopt(*service, handle.release());
	EXPECT_EQ(file.backend(), alt::io_backend_kind::thread_pool);
	EXPECT_EQ(service->backend(), alt::io_backend_kind::iocp) << "the service reports its own backend, not the file's";

	const auto wrote = run(file.write_at(buffer_holding("synchronous"), 0, 11));
	ASSERT_TRUE(wrote.has_value()) << wrote.error().message();
	EXPECT_EQ(*wrote, 11U);
	alt::io_buffer       buffer = buffer_of(11);
	const alt::io_buffer view   = buffer;
	const auto           read   = run(file.read_at(std::move(buffer), 0, 11));
	ASSERT_TRUE(read.has_value()) << read.error().message();
	EXPECT_EQ(text_of(view, *read), "synchronous");
}

TEST_F(AsyncFileTest, AnAdoptedOverlappedFileHandleReportsThreadPoolAndWorks)
{
	// The regular-file variant. A cached read on an overlapped file usually completes
	// synchronously, so this can pass with the routing wrong; the pipe test below cannot.
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();
	win32_handle handle = open_for_adoption(dir() / "overlapped.txt", FILE_FLAG_OVERLAPPED);
	ASSERT_TRUE(handle.valid()) << ::GetLastError();

	const alt::async_file file = alt::async_file::adopt(*service, handle.release());
	EXPECT_EQ(file.backend(), alt::io_backend_kind::thread_pool);

	const auto wrote = run(file.write_at(buffer_holding("overlapped"), 0, 10));
	ASSERT_TRUE(wrote.has_value()) << wrote.error().message();
	EXPECT_EQ(*wrote, 10U);
	alt::io_buffer       buffer = buffer_of(10);
	const alt::io_buffer view   = buffer;
	const auto           read   = run(file.read_at(std::move(buffer), 0, 10));
	ASSERT_TRUE(read.has_value()) << read.error().message();
	EXPECT_EQ(text_of(view, *read), "overlapped");
}

TEST_F(AsyncFileTest, AnAdoptedOverlappedHandleReportsThreadPoolAndWorks)
{
	// The routing itself. An adopted handle is not associated with the IOCP backend's port, so
	// an overlapped read the IOCP backend issued on it would go pending and its packet would
	// never come. A read on an empty pipe always goes pending, so this read completes only if it
	// went to the thread pool, which waits for it on an event of its own.
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();
	overlapped_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << "could not create the pipe: " << ::GetLastError();
	const win32_handle worker = only_worker_of(pool());
	ASSERT_TRUE(worker.valid()) << ::GetLastError();
	const std::optional<bool> idle = has_pending_io(worker.get());
	ASSERT_TRUE(idle.has_value()) << "GetThreadIOPendingFlag failed: " << ::GetLastError();
	ASSERT_FALSE(*idle) << "the idle worker already reports pending I/O; the probe cannot tell anything apart";

	const alt::async_file file = alt::async_file::adopt(*service, pipe.server.release());
	EXPECT_EQ(file.backend(), alt::io_backend_kind::thread_pool);

	const std::string    payload         = "pending";
	alt::io_buffer       buffer          = buffer_of(payload.size());
	const alt::io_buffer view            = buffer;
	using result_type                    = std::expected<std::size_t, std::error_code>;
	const auto                     read  = std::make_shared<outcome<result_type>>();
	const alt::cancellation_handle chain = alt::start(capture(file.read_at(std::move(buffer), 0, payload.size()), read));

	// Written only once the read is outstanding on the pool's worker, so it cannot have
	// completed synchronously.
	EXPECT_TRUE(pump_until([&] { return read->finished || has_pending_io(worker.get()).value_or(false); }))
	  << "the read never became outstanding on the pool's worker: it was not routed to the thread pool";
	EXPECT_FALSE(read->finished) << "the read finished before anything was written into the pipe";
	ASSERT_TRUE(pipe.write(payload)) << ::GetLastError();

	if(!pump_until([&] { return read->finished; }))
	{
		ADD_FAILURE() << "the read did not finish within " << deadline.count() << " seconds";
		// Closing the client end fails a read the pool is waiting on with ERROR_BROKEN_PIPE, so the
		// service can go quiet. A read stranded at the IOCP backend never finishes, and then the
		// service's destructor waits for it until the test's timeout ends the process.
		pipe.client.reset();
		static_cast<void>(pump_until([&] { return read->finished; }));
		return;
	}
	ASSERT_TRUE(read->result.has_value());
	const result_type& got = *read->result;
	ASSERT_TRUE(got.has_value()) << got.error().message();
	EXPECT_EQ(*got, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);

	// And the other direction. A pipe ignores the offset.
	const auto wrote = run(file.write_at(buffer_holding("reply"), 0, 5));
	ASSERT_TRUE(wrote.has_value()) << wrote.error().message();
	EXPECT_EQ(*wrote, 5U);
	EXPECT_EQ(pipe.read(5), "reply");
}

TEST_F(AsyncFileTest, CancellingAnAdoptedFilesQueuedReadReachesThePool)
{
	// The cancel must go to the backend the submission went to. Sent to the IOCP backend instead,
	// a pool token names nothing there (or another operation), the queued jobs run once the
	// worker frees up, and the read fills its buffer and the write reaches the file.
	const auto path    = write_file("data.txt", "0123456789");
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();
	win32_handle handle = open_for_adoption(path, 0);
	ASSERT_TRUE(handle.valid()) << ::GetLastError();
	const alt::async_file file = alt::async_file::adopt(*service, handle.release());
	ASSERT_EQ(file.backend(), alt::io_backend_kind::thread_pool);

	// Occupy the pool's only worker, so the read and the write queue behind it.
	const auto         gate    = std::make_shared<std::atomic<bool>>(false);
	const auto         entered = std::make_shared<std::atomic<bool>>(false);
	const open_on_exit release{gate};
	using void_type    = std::expected<void, std::error_code>;
	const auto blocker = std::make_shared<outcome<void_type>>();
	const auto hold    = [gate, entered] {
		entered->store(true);
		while(!gate->load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
	};
	const alt::cancellation_handle blocker_chain = alt::start(capture(service->run(hold), blocker));
	ASSERT_TRUE(pump_until([&] { return entered->load(); }));

	using count_type                     = std::expected<std::size_t, std::error_code>;
	alt::io_buffer           target      = buffer_holding("zzzz");
	const alt::io_buffer     view        = target;
	const auto               read        = std::make_shared<outcome<count_type>>();
	const auto               wrote       = std::make_shared<outcome<count_type>>();
	alt::cancellation_handle read_chain  = alt::start(capture(file.read_at(std::move(target), 0, 4), read));
	alt::cancellation_handle write_chain = alt::start(capture(file.write_at(buffer_holding("XXXX"), 0, 4), wrote));
	EXPECT_TRUE(read_chain.request_stop());
	EXPECT_TRUE(write_chain.request_stop());

	// Queued after the cancels, so it takes no token a misdirected cancel could have named. The
	// pool runs its queue in FIFO order, so this running means both cancelled jobs had their turn.
	const auto                     behind       = std::make_shared<std::atomic<bool>>(false);
	const auto                     marker       = std::make_shared<outcome<void_type>>();
	const alt::cancellation_handle marker_chain = alt::start(capture(service->run([behind] { behind->store(true); }), marker));

	gate->store(true);
	ASSERT_TRUE(pump_until([&] { return blocker->finished && marker->finished; }));
	home().drain();

	// A cancelled chain is abandoned rather than resumed, so neither reports anything.
	EXPECT_FALSE(read->finished);
	EXPECT_FALSE(wrote->finished);
	EXPECT_TRUE(behind->load());
	EXPECT_EQ(text_of(view, 4), "zzzz") << "the cancelled read ran: its cancel did not reach the pool";
	EXPECT_EQ(read_whole(path), "0123456789") << "the cancelled write ran: its cancel did not reach the pool";
}

TEST_F(AsyncFileTest, AMovedFromFileReportsThreadPool)
{
	const auto path    = write_file("data.txt", "0123456789");
	const auto service = make_service(alt::io_backend_kind::iocp);
	ASSERT_TRUE(service.has_value()) << service.error().message();

	auto opened = run(service->open(path, alt::flags(alt::open_flag::read)));
	ASSERT_TRUE(opened.has_value()) << opened.error().message();
	alt::async_file moved = std::move(*opened);

	EXPECT_EQ(moved.backend(), alt::io_backend_kind::iocp);
	EXPECT_EQ(opened->backend(), alt::io_backend_kind::thread_pool); // NOLINT(bugprone-use-after-move): checking the moved-from state

	// A closed file has no service either.
	EXPECT_TRUE(run(moved.close()).has_value());
	EXPECT_EQ(moved.backend(), alt::io_backend_kind::thread_pool);
}
#endif

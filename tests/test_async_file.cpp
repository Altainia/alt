#include <gtest/gtest.h>

#include <alt/async_file.hpp>
#include <alt/cancellable_task.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/file_io.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/scheduler.hpp>
#include <alt/sync_wait.hpp>
#include <alt/task.hpp>
#include <alt/thread_pool.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <semaphore>
#include <string>
#include <system_error>
#include <utility>

namespace
{

	/** What a started task produced, and whether it has produced it yet. */
	template<typename T>
	struct outcome
	{
		bool finished = false;
		T    result{};
	};

	/** Starts @p work and stores what it produced in @p out, so a test can run it without blocking. */
	template<typename T>
	alt::cancellable_task capture(alt::task<T> work, std::shared_ptr<outcome<T>> out)
	{
		out->result   = co_await std::move(work);
		out->finished = true;
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
	 * The service twice over: one on the real thread-pool backend, which does real I/O, and
	 * one on a manual backend, which lets a test see exactly what was submitted and when.
	 * Both complete onto the same manual scheduler, which a test drives itself.
	 */
	class AsyncFileTest: public ::testing::Test
	{
	protected:
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

		/** The service whose backend does nothing until the test completes an operation by hand. */
		alt::file_io& manual_io()
		{
			return *m_manual;
		}

		alt::manual_io_backend& manual_backend()
		{
			return m_manual_backend;
		}

		/** Runs @p work to completion, driving the completion scheduler. */
		template<typename T>
		T run(alt::task<T> work)
		{
			return alt::sync_wait(std::move(work), m_home);
		}

		/** Runs the completion scheduler until @p done reports true. */
		template<typename Predicate>
		void pump_until(Predicate done)
		{
			while(!done())
			{
				m_home.run_one_or_wait();
			}
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

		/** Whether @p handle still names an open descriptor, asked of the operating system. */
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
		std::expected<alt::thread_pool, std::error_code> m_pool = alt::thread_pool::create(1);
		alt::manual_scheduler                            m_home;
		alt::manual_io_backend                           m_manual_backend;
		std::expected<alt::file_io, std::error_code>     m_real   = alt::file_io::create(*m_pool, alt::scheduler_ref{m_home});
		std::expected<alt::file_io, std::error_code>     m_manual = alt::file_io::create(*m_pool, alt::scheduler_ref{m_home}, m_manual_backend);
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

TEST_F(AsyncFileTest, SyncDataSucceedsOnEveryPlatform)
{
	auto file = run(real_io().open(dir() / "synced.bin", alt::flags(alt::open_flag::read_write) | alt::flags(alt::open_flag::create)));
	ASSERT_TRUE(file.has_value());
	ASSERT_TRUE(run(file->write_at(buffer_holding("durable"), 0, 7)).has_value());

	EXPECT_TRUE(run(file->sync(alt::sync_mode::data)).has_value());
	EXPECT_TRUE(run(file->sync(alt::sync_mode::full)).has_value());
	EXPECT_TRUE(run(file->sync()).has_value());
}

TEST_F(AsyncFileTest, TwoConcurrentPositionalReadsEachGetTheirOwnOffsetsBytes)
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
	pump_until([&] { return first->finished && second->finished; });

	ASSERT_TRUE(first->result.has_value());
	ASSERT_TRUE(second->result.has_value());
	EXPECT_EQ(text_of(first_buffer, 4), "CCCC");
	EXPECT_EQ(text_of(second_buffer, 4), "AAAA");
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
	manual_backend().complete(1, alt::io_completion{.bytes = 3, .error = {}});
	manual_backend().complete(0, alt::io_completion{.bytes = 4, .error = {}});
	home().drain();

	ASSERT_TRUE(first->finished);
	ASSERT_TRUE(second->finished);
	ASSERT_TRUE(first->result.has_value());
	ASSERT_TRUE(second->result.has_value());
	EXPECT_EQ(*first->result, 4U);
	EXPECT_EQ(*second->result, 3U);
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

	manual_backend().complete(0, alt::io_completion{.bytes = 4, .error = {}});
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

TEST_F(AsyncFileTest, OpenWithExclusiveOverAnExistingFileReportsFileExists)
{
	const auto path   = write_file("data.txt", "x");
	const auto opened = run(real_io().open(path, alt::flags(alt::open_flag::write) | alt::flags(alt::open_flag::create) | alt::flags(alt::open_flag::exclusive)));

	ASSERT_FALSE(opened.has_value());
	EXPECT_EQ(opened.error(), std::errc::file_exists);
}

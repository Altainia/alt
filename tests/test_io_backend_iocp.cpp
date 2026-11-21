// The IOCP backend exists only on Windows, so this whole file does too.
#ifdef _WIN32

#	include <gtest/gtest.h>

#	include <alt/detail/file_types.hpp>
#	include <alt/detail/io_handle.hpp>
#	include <alt/io_backend.hpp>
#	include <alt/io_buffer.hpp>
#	include <alt/thread_pool.hpp>
#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <chrono>
#	include <cstddef>
#	include <cstdint>
#	include <cstdlib>
#	include <cstring>
#	include <expected>
#	include <filesystem>
#	include <fstream>
#	include <iostream>
#	include <iterator>
#	include <latch>
#	include <limits>
#	include <memory>
#	include <semaphore>
#	include <string>
#	include <string_view>
#	include <system_error>
#	include <thread>
#	include <utility>
#	include <vector>

#	include "death_test_support.hpp"
#	include "io_backend_iocp.hpp"

namespace
{
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

	/// Retries @p ready every millisecond until it holds or @p budget is spent.
	template<typename Ready>
	bool wait_until(Ready ready, std::chrono::steady_clock::duration budget = std::chrono::seconds(10))
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

	/// Closes a Win32 handle on scope exit; a null handle is left alone.
	struct scoped_handle
	{
		HANDLE value = nullptr;

		scoped_handle() = default;
		explicit scoped_handle(HANDLE owned): value(owned == INVALID_HANDLE_VALUE ? nullptr : owned)
		{}
		scoped_handle(const scoped_handle&)            = delete;
		scoped_handle& operator=(const scoped_handle&) = delete;
		scoped_handle(scoped_handle&&)                 = delete;
		scoped_handle& operator=(scoped_handle&&)      = delete;

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

	/**
	 * The server end of a named pipe the test writes into. The backend opens the client end for
	 * reading, through its own open, so a read on it is overlapped and port-associated, and goes
	 * genuinely pending until the server writes: a regular file's read usually completes
	 * synchronously and would leave the packet path untested.
	 */
	struct served_pipe
	{
		std::wstring  name;
		scoped_handle server;

		served_pipe()
		{
			static std::atomic<int> counter{0};
			name         = L"\\\\.\\pipe\\alt_iocp_backend_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(counter.fetch_add(1));
			server.value = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 0, 0, nullptr);
			if(server.value == INVALID_HANDLE_VALUE)
			{
				server.value = nullptr;
			}
		}

		[[nodiscard]] bool valid() const
		{
			return server.value != nullptr;
		}

		/// Settles the server's side of the handshake once the client has opened its end.
		void accept() const
		{
			// The client connected by opening, so this reports ERROR_PIPE_CONNECTED; nothing waits.
			static_cast<void>(::ConnectNamedPipe(server.value, nullptr));
		}

		bool write(std::string_view bytes) const
		{
			DWORD written = 0;
			return ::WriteFile(server.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
		}
	};

	/// Frees a read still pending on the pipe when a failed assertion returns early: closing the
	/// server end fails it with ERROR_BROKEN_PIPE, so its callback runs and the backend can be
	/// destroyed without the in-flight assert. Does nothing once the test saw the completion.
	struct pipe_releaser
	{
		served_pipe& pipe;
		waiter&      job;
		bool         completed = false;

		pipe_releaser(served_pipe& served, waiter& pending): pipe(served), job(pending)
		{}
		pipe_releaser(const pipe_releaser&)            = delete;
		pipe_releaser& operator=(const pipe_releaser&) = delete;
		pipe_releaser(pipe_releaser&&)                 = delete;
		pipe_releaser& operator=(pipe_releaser&&)      = delete;

		~pipe_releaser()
		{
			if(!completed)
			{
				pipe.server.reset();
				job.done.acquire();
			}
		}
	};

	class IocpBackendTest: public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<int> counter{0};
			m_dir.path = std::filesystem::temp_directory_path() /
			             ("alt_iocp_backend_" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(::GetCurrentProcessId()));
			std::filesystem::create_directories(m_dir.path);
			ASSERT_TRUE(m_pool.has_value());
			auto made = alt::detail::make_iocp_backend(*m_pool);
			ASSERT_TRUE(made.has_value()) << made.error().message();
			m_backend = std::move(*made);
		}

		void TearDown() override
		{
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
			return *m_backend;
		}

		void destroy_backend()
		{
			m_backend.reset();
		}

		[[nodiscard]] std::size_t ignored_packets()
		{
			return alt::detail::iocp_ignored_packets_for_testing(*m_backend);
		}

		/// Waits until the ignored-packet count has held still for 100 ms, and returns it.
		std::size_t settled_ignored_packets()
		{
			std::size_t last = ignored_packets();
			for(int round = 0; round < 50; ++round)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const std::size_t now = ignored_packets();
				if(now == last)
				{
					return now;
				}
				last = now;
			}
			return last;
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
			w.done.acquire();
			EXPECT_EQ(w.calls.load(), 1);
			return w.result;
		}

		alt::io_completion run(const alt::io_request& request)
		{
			waiter w;
			return run(request, w);
		}

		/// Opens @p path through the backend, so every handle a test uses is overlapped and port-associated.
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

		/// The id of the pool's only worker, learned by running a job on it.
		std::thread::id worker_id()
		{
			struct probe
			{
				std::binary_semaphore done{0};
				std::thread::id       id;
			} p;
			pool().post(alt::work_item{.invoke = +[](void* state) noexcept {
				                           auto& seen = *static_cast<probe*>(state);
				                           seen.id    = std::this_thread::get_id();
				                           seen.done.release();
			                           },
			                           .state = &p});
			p.done.acquire();
			return p.id;
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

	alt::io_request positional(alt::io_op op, alt::native_handle handle, alt::io_buffer buffer, std::uint64_t offset, std::size_t size)
	{
		alt::io_request request;
		request.op     = op;
		request.handle = handle;
		request.buffer = std::move(buffer);
		request.offset = offset;
		request.size   = size;
		return request;
	}

	alt::io_request positional(alt::io_op op, const alt::detail::io_handle& file, alt::io_buffer buffer, std::uint64_t offset, std::size_t size)
	{
		return positional(op, file.native(), std::move(buffer), offset, size);
	}

	std::string text_of(const alt::io_buffer& buffer, std::size_t count)
	{
		return {reinterpret_cast<const char*>(buffer.bytes().data()), count};
	}

	HANDLE win32(const alt::detail::io_handle& file)
	{
		return reinterpret_cast<HANDLE>(file.native());
	}
} // namespace

TEST_F(IocpBackendTest, ReportsIocpAsItsKind)
{
	EXPECT_EQ(backend().kind(), alt::io_backend_kind::iocp);
}

TEST_F(IocpBackendTest, AReadAtReturnsTheFilesBytes)
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

TEST_F(IocpBackendTest, AWriteAtPutsBytesAtTheOffset)
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

TEST_F(IocpBackendTest, AnAppendWriteLandsAtTheEnd)
{
	const auto path = write_file("data.txt", "start");
	auto       file = open_file(path, alt::flags(alt::open_flag::write) | alt::flags(alt::open_flag::append));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	// Both requests name offset 0: an append handle must ignore it.
	for(const char* const text: {"-one", "-two"})
	{
		auto buffer = buffer_of(4);
		std::memcpy(buffer.bytes().data(), text, 4);
		const auto done = run(positional(alt::io_op::write, **file, std::move(buffer), 0, 4));
		EXPECT_FALSE(done.error) << done.error.message();
		EXPECT_EQ(done.bytes, 4U);
	}
	file->reset();
	EXPECT_EQ(read_whole(path), "start-one-two");
}

TEST_F(IocpBackendTest, AReadPastTheEndReturnsAShortCount)
{
	const auto path = write_file("data.txt", "short");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	const auto partial = run(positional(alt::io_op::read, **file, buffer_of(16), 2, 16));
	EXPECT_FALSE(partial.error) << partial.error.message();
	EXPECT_EQ(partial.bytes, 3U);

	const auto beyond = run(positional(alt::io_op::read, **file, buffer_of(16), 100, 16));
	EXPECT_FALSE(beyond.error) << beyond.error.message();
	EXPECT_EQ(beyond.bytes, 0U);
}

TEST_F(IocpBackendTest, AReadAtOrPastTheEndIsAZeroByteSuccess)
{
	// An overlapped read at the end may report ERROR_HANDLE_EOF from ReadFile itself or in the
	// completion; either way it is a short count of zero, as on the pool arm.
	auto file = open_file(write_file("data.txt", "hello world"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	for(const std::uint64_t offset: {std::uint64_t{11}, std::uint64_t{4096}})
	{
		const auto at_end = run(positional(alt::io_op::read, **file, buffer_of(4), offset, 4));
		EXPECT_FALSE(at_end.error) << "offset " << offset << ": " << at_end.error.message();
		EXPECT_EQ(at_end.bytes, 0U) << "offset " << offset;
	}
}

TEST_F(IocpBackendTest, AZeroLengthTransferCompletesWithNoBytesAndNoError)
{
	// A transfer that moves nothing must still complete, never wait for a packet that is not coming.
	const auto path = write_file("data.txt", "0123456789");
	auto       file = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	for(const auto op: {alt::io_op::read, alt::io_op::write})
	{
		const auto zero_size = run(positional(op, **file, buffer_of(4), 2, 0));
		EXPECT_FALSE(zero_size.error) << "size 0: " << zero_size.error.message();
		EXPECT_EQ(zero_size.bytes, 0U) << "size 0";

		const auto empty_buffer = run(positional(op, **file, alt::io_buffer{}, 2, 4));
		EXPECT_FALSE(empty_buffer.error) << "empty buffer: " << empty_buffer.error.message();
		EXPECT_EQ(empty_buffer.bytes, 0U) << "empty buffer";
	}
	file->reset();
	EXPECT_EQ(read_whole(path), "0123456789");
}

TEST_F(IocpBackendTest, ARequestedSizeIsClampedToTheBuffer)
{
	auto file = open_file(write_file("data.txt", "0123456789"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto       buffer = buffer_of(4);
	const auto view   = buffer;
	const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 0, std::numeric_limits<std::size_t>::max()));

	EXPECT_FALSE(done.error) << done.error.message();
	EXPECT_EQ(done.bytes, 4U);
	EXPECT_EQ(text_of(view, 4), "0123");
}

TEST_F(IocpBackendTest, AnOffsetAboveFourGibReachesTheFile)
{
	// Sparse, so the file costs nothing on disk. Marked through a handle of the test's own: the
	// backend's handle is overlapped, and DeviceIoControl on it would need an OVERLAPPED too.
	const auto path = write_file("far.bin", "");
	{
		const scoped_handle plain(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		ASSERT_NE(plain.value, nullptr) << ::GetLastError();
		DWORD returned = 0;
		ASSERT_NE(::DeviceIoControl(plain.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr), FALSE) << ::GetLastError();
	}
	auto file = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	// 5 GiB is 0x1'4000'0000: OffsetHigh 1, Offset 0x4000'0000. A dropped high word would land
	// the write at 1 GiB, and reading back at the same offset would not notice: the size does.
	constexpr std::uint64_t far_offset = std::uint64_t{5} << 30U;
	auto                    out        = buffer_of(3);
	std::memcpy(out.bytes().data(), "far", 3);
	const auto written = run(positional(alt::io_op::write, **file, std::move(out), far_offset, 3));
	ASSERT_FALSE(written.error) << written.error.message();
	EXPECT_EQ(written.bytes, 3U);

	auto       in   = buffer_of(3);
	const auto view = in;
	const auto read = run(positional(alt::io_op::read, **file, std::move(in), far_offset, 3));
	ASSERT_FALSE(read.error) << read.error.message();
	EXPECT_EQ(read.bytes, 3U);
	EXPECT_EQ(text_of(view, 3), "far");

	file->reset();
	EXPECT_EQ(std::filesystem::file_size(path), far_offset + 3);
}

TEST_F(IocpBackendTest, ASynchronousSuccessQueuesNoPacket)
{
	// With FILE_SKIP_COMPLETION_PORT_ON_SUCCESS a synchronous success queues nothing, so the
	// inline completion is the only one. Without it, a packet would follow for a record already
	// completed and freed, and the completion thread would have to ignore it.
	//
	// A cached file read is not guaranteed to complete synchronously, and on the machine this was
	// written on none did: every overlapped ReadFile of a freshly written file went pending, even
	// on a handle with no port at all. So the file reads the brief asks for are joined by reads on
	// a pipe whose data is already waiting, which the pipe driver completes on the spot. Each
	// phase records how many of its reads ran inline; the assertion is on the ignored count.
	constexpr std::size_t reads = 50;

	const std::string payload = "a small, freshly written and therefore cached file";
	auto              file    = open_file(write_file("data.txt", payload), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	std::array<waiter, reads> file_reads;
	int                       file_inline = 0;
	for(auto& w: file_reads)
	{
		auto       buffer = buffer_of(payload.size());
		const auto view   = buffer;
		const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 0, payload.size()), w);
		EXPECT_FALSE(done.error) << done.error.message();
		EXPECT_EQ(done.bytes, payload.size());
		EXPECT_EQ(text_of(view, payload.size()), payload);
		file_inline += w.ran_inline.load() ? 1 : 0;
	}

	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto client = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(client.has_value()) << client.error().message();
	pipe.accept();
	constexpr std::string_view chunk = "waiting";
	std::array<waiter, reads>  pipe_reads;
	int                        pipe_inline = 0;
	for(auto& w: pipe_reads)
	{
		ASSERT_TRUE(pipe.write(chunk));
		auto       buffer = buffer_of(chunk.size());
		const auto view   = buffer;
		const auto done   = run(positional(alt::io_op::read, **client, std::move(buffer), 0, chunk.size()), w);
		EXPECT_FALSE(done.error) << done.error.message();
		EXPECT_EQ(done.bytes, chunk.size());
		EXPECT_EQ(text_of(view, chunk.size()), chunk);
		pipe_inline += w.ran_inline.load() ? 1 : 0;
	}

	RecordProperty("synchronous_file_reads", file_inline);
	RecordProperty("synchronous_pipe_reads", pipe_inline);
	std::cout << "[          ] completed synchronously: " << file_inline << " of " << reads << " file reads, " << pipe_inline << " of " << reads << " pipe reads\n";

	EXPECT_EQ(settled_ignored_packets(), 0U);
	for(const auto& w: file_reads)
	{
		EXPECT_EQ(w.calls.load(), 1);
	}
	for(const auto& w: pipe_reads)
	{
		EXPECT_EQ(w.calls.load(), 1);
	}
}

TEST_F(IocpBackendTest, AReadOnAPipeIsPendingUntilDataArrives)
{
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	constexpr std::string_view payload = "pending";
	auto                       buffer  = buffer_of(payload.size());
	const auto                 view    = buffer;
	waiter                     w;
	ASSERT_TRUE(backend().submit(positional(alt::io_op::read, **file, std::move(buffer), 0, payload.size()), deliver, &w).has_value());
	w.submitted.store(true);
	pipe_releaser releaser(pipe, w);

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(w.calls.load(), 0) << "the read completed before anything was written to the pipe";

	ASSERT_TRUE(pipe.write(payload));
	w.done.acquire();
	releaser.completed = true;

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.ran_inline.load());
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);
}

TEST_F(IocpBackendTest, AMessageModePipeReadLargerThanItsBufferCompletesOnce)
{
	// A message-mode read shorter than the message fails with ERROR_MORE_DATA, a warning-class
	// status: unlike an error, it still queues a packet. Completing it inline as well would run
	// the callback here and leave the packet to be ignored, or worse, to match a newer record at
	// a reused address. The result must be the pool arm's: the error, and no bytes.
	static std::atomic<int> counter{0};
	const std::wstring      name = L"\\\\.\\pipe\\alt_iocp_message_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(counter.fetch_add(1));
	// Duplex, so the client can be opened with the write access SetNamedPipeHandleState needs.
	scoped_handle server(::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
	ASSERT_NE(server.value, nullptr) << ::GetLastError();
	auto client = open_file(std::filesystem::path(name), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(client.has_value()) << client.error().message();
	static_cast<void>(::ConnectNamedPipe(server.value, nullptr));
	DWORD mode = PIPE_READMODE_MESSAGE;
	ASSERT_NE(::SetNamedPipeHandleState(win32(**client), &mode, nullptr, nullptr), FALSE) << ::GetLastError();

	constexpr std::string_view message = "sixteen byte msg";
	DWORD                      written = 0;
	ASSERT_NE(::WriteFile(server.value, message.data(), static_cast<DWORD>(message.size()), &written, nullptr), FALSE) << ::GetLastError();

	const std::size_t before = ignored_packets();
	waiter            w;
	ASSERT_TRUE(backend().submit(positional(alt::io_op::read, **client, buffer_of(4), 0, 4), deliver, &w).has_value());
	w.submitted.store(true);
	ASSERT_TRUE(w.done.try_acquire_for(std::chrono::seconds(10))) << "the read never completed";

	EXPECT_EQ(settled_ignored_packets(), before) << "a packet came for a read already completed inline";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_EQ(w.result.error, std::error_code(ERROR_MORE_DATA, std::system_category())) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 0U);
	RecordProperty("ran_inline", w.ran_inline.load() ? 1 : 0);
}

TEST_F(IocpBackendTest, AnImmediateFailureCompletesInline)
{
	auto file = open_file(write_file("data.txt", "read only"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto buffer = buffer_of(4);
	std::memcpy(buffer.bytes().data(), "nope", 4);
	waiter     w;
	const auto done = run(positional(alt::io_op::write, **file, std::move(buffer), 0, 4), w);

	EXPECT_TRUE(done.error) << "a write on a read-only handle must fail";
	EXPECT_EQ(done.bytes, 0U);
	EXPECT_TRUE(w.ran_inline.load()) << "an immediate failure queues no packet, so only submit() can complete it";
}

TEST_F(IocpBackendTest, ATransferOnADirectoryReportsIsADirectory)
{
	auto directory = open_file(dir(), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(directory.has_value()) << directory.error().message();

	const auto done = run(positional(alt::io_op::read, **directory, buffer_of(4), 0, 4));

	EXPECT_EQ(done.error, std::errc::is_a_directory) << done.error.message();
	EXPECT_EQ(done.bytes, 0U);
}

TEST_F(IocpBackendTest, ATransferOnAClosedHandleFails)
{
	auto file = open_file(write_file("data.txt", "x"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	const auto stale = (*file)->native();
	file->reset(); // closes the handle; the value now names nothing

	for(const auto op: {alt::io_op::read, alt::io_op::write})
	{
		const auto done = run(positional(op, stale, buffer_of(1), 0, 1));
		EXPECT_EQ(done.bytes, 0U);
		EXPECT_TRUE(done.error) << "a transfer on a closed handle must fail";
	}
}

TEST_F(IocpBackendTest, NonTransferOperationsRunOnThePool)
{
	const auto worker = worker_id();
	const auto path   = write_file("data.txt", "twelve bytes");
	auto       file   = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	const auto expect_on_pool = [&](const alt::io_request& request, const char* what) {
		waiter     w;
		const auto done = run(request, w);
		EXPECT_FALSE(done.error) << what << ": " << done.error.message();
		EXPECT_EQ(w.thread, worker) << what << " did not run on the pool's worker";
	};

	alt::file_status status;
	alt::io_request  stat;
	stat.op     = alt::io_op::stat_handle;
	stat.handle = (*file)->native();
	stat.out    = &status;
	expect_on_pool(stat, "stat_handle");
	EXPECT_EQ(status.size, 12U);
	EXPECT_EQ(status.kind, alt::file_kind::regular);

	for(const auto op: {alt::io_op::sync_full, alt::io_op::sync_data})
	{
		alt::io_request sync;
		sync.op     = op;
		sync.handle = (*file)->native();
		expect_on_pool(sync, "sync");
	}
	file->reset();

	const auto          renamed = dir() / "renamed.txt";
	alt::io_rename_args names{.from = path, .to = renamed};
	alt::io_request     rename;
	rename.op = alt::io_op::rename;
	rename.in = &names;
	expect_on_pool(rename, "rename");
	EXPECT_TRUE(std::filesystem::exists(renamed));

	std::vector<alt::directory_entry> entries;
	alt::io_request                   list;
	list.op  = alt::io_op::list_directory;
	list.in  = &dir();
	list.out = &entries;
	expect_on_pool(list, "list_directory");
	ASSERT_EQ(entries.size(), 1U);
	EXPECT_EQ(entries[0].name, "renamed.txt");

	alt::io_request unlink;
	unlink.op = alt::io_op::unlink;
	unlink.in = &renamed;
	expect_on_pool(unlink, "unlink");
	EXPECT_FALSE(std::filesystem::exists(renamed));
}

namespace
{
	/// Issues the test's own overlapped read of @p count bytes on @p handle, as a consumer using
	/// async_file::handle() might, waits for it, and returns what it read. @p tag sets the low bit
	/// of hEvent, Windows' documented way of keeping the completion off the handle's port.
	std::string foreign_read(HANDLE handle, const served_pipe& pipe, std::string_view payload, bool tag)
	{
		const scoped_handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
		EXPECT_NE(event.value, nullptr);
		OVERLAPPED position{};
		position.hEvent = tag ? reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event.value) | 1U) : event.value;
		std::string bytes(payload.size(), '\0');
		const BOOL  ok = ::ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &position);
		EXPECT_EQ(ok, FALSE) << "the foreign read should have been pending on an empty pipe";
		EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
		EXPECT_TRUE(pipe.write(payload));
		EXPECT_EQ(::WaitForSingleObject(event.value, 10'000), WAIT_OBJECT_0);
		DWORD moved = 0;
		EXPECT_NE(::GetOverlappedResult(handle, &position, &moved, FALSE), FALSE);
		bytes.resize(moved);
		return bytes;
	}
} // namespace

TEST_F(IocpBackendTest, AForeignPacketIsIgnored)
{
	// Untagged, the foreign read's completion reaches the backend's port naming an OVERLAPPED the
	// backend never registered. Treating it as a record would call through this frame's memory.
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	const std::size_t before = ignored_packets();
	EXPECT_EQ(foreign_read(win32(**file), pipe, "foreign", false), "foreign");
	EXPECT_TRUE(wait_until([&] { return ignored_packets() == before + 1; })) << "ignored packets: " << ignored_packets() - before;

	// The backend's own transfers on the same handle keep completing normally.
	constexpr std::string_view payload = "ours";
	auto                       buffer  = buffer_of(payload.size());
	const auto                 view    = buffer;
	waiter                     w;
	ASSERT_TRUE(backend().submit(positional(alt::io_op::read, **file, std::move(buffer), 0, payload.size()), deliver, &w).has_value());
	w.submitted.store(true);
	pipe_releaser releaser(pipe, w);
	ASSERT_TRUE(pipe.write(payload));
	w.done.acquire();
	releaser.completed = true;

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);
	EXPECT_EQ(settled_ignored_packets(), before + 1);
}

TEST_F(IocpBackendTest, AStalePacketAtAPendingRecordsAddressIsIgnored)
{
	// The registry finds a record by OVERLAPPED address alone. A consumer's own untagged call on
	// the handle, whose heap OVERLAPPED is freed before its packet is dequeued, leaves a packet
	// naming an address a newer record may occupy while still pending. Completing that record
	// would report it failed and free it while the kernel still owns its OVERLAPPED and buffer.
	// The stale packet is forged here by posting one that names the pending record's address.
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	constexpr std::string_view payload = "genuine";
	auto                       buffer  = buffer_of(payload.size());
	const auto                 view    = buffer;
	waiter                     w;
	const auto                 token = backend().submit(positional(alt::io_op::read, **file, std::move(buffer), 0, payload.size()), deliver, &w);
	w.submitted.store(true);
	ASSERT_TRUE(token.has_value()) << token.error().message();
	pipe_releaser releaser(pipe, w);

	const OVERLAPPED* const pending = alt::detail::iocp_pending_overlapped_for_testing(backend(), *token);
	ASSERT_NE(pending, nullptr) << "the read completed before anything was written to the pipe";
	const std::size_t before = ignored_packets();
	// Completion key 0 is the one every handle the backend opens carries, as the stale packet would.
	ASSERT_NE(::PostQueuedCompletionStatus(alt::detail::iocp_port_for_testing(backend()), 0, 0, const_cast<OVERLAPPED*>(pending)), FALSE) << ::GetLastError();

	EXPECT_TRUE(wait_until([&] { return ignored_packets() == before + 1; })) << "ignored packets: " << ignored_packets() - before;
	EXPECT_EQ(w.calls.load(), 0) << "the stale packet completed the pending read: " << w.result.error.message();

	ASSERT_TRUE(pipe.write(payload));
	ASSERT_TRUE(w.done.try_acquire_for(std::chrono::seconds(10))) << "the read never completed";
	releaser.completed = true;

	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);
	EXPECT_EQ(settled_ignored_packets(), before + 1);
	EXPECT_EQ(w.calls.load(), 1);
}

TEST_F(IocpBackendTest, ATaggedOverlappedCallQueuesNothing)
{
	// The documented way for a consumer to use handle() itself: tag hEvent, and alt never sees it.
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	const std::size_t before = ignored_packets();
	EXPECT_EQ(foreign_read(win32(**file), pipe, "tagged", true), "tagged");

	// The settled ignored count is the real discriminator: a packet the call queued would reach
	// the completion thread, which waits on the port throughout, and be counted there. The
	// port-emptiness check below is near-vacuous for the same reason, since that thread takes
	// any packet before this one could look; it stays as a cheap second witness.
	EXPECT_EQ(settled_ignored_packets(), before);
	DWORD       bytes = 0;
	ULONG_PTR   key   = 0;
	OVERLAPPED* entry = nullptr;
	EXPECT_EQ(::GetQueuedCompletionStatus(alt::detail::iocp_port_for_testing(backend()), &bytes, &key, &entry, 0), FALSE);
	EXPECT_EQ(entry, nullptr) << "the tagged call queued a packet to the backend's port";
}

TEST_F(IocpBackendTest, APoolTransferOnAPortAssociatedHandleQueuesNothing)
{
	// The pool arm's transfer tags its event (issue #52), so on a handle associated with the
	// backend's port it posts nothing there: its OVERLAPPED is on a worker's stack.
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	const auto                 pool_backend = alt::make_thread_pool_backend(pool());
	const std::size_t          before       = ignored_packets();
	constexpr std::string_view payload      = "pooled";
	auto                       buffer       = buffer_of(payload.size());
	const auto                 view         = buffer;
	waiter                     w;
	ASSERT_TRUE(pool_backend->submit(positional(alt::io_op::read, **file, std::move(buffer), 0, payload.size()), deliver, &w).has_value());
	w.submitted.store(true);
	pipe_releaser releaser(pipe, w);
	// Give the worker time to have the read outstanding, so it completes from pending.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(w.calls.load(), 0) << "the pool read finished before anything was written";
	ASSERT_TRUE(pipe.write(payload));
	w.done.acquire();
	releaser.completed = true;

	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, payload.size());
	EXPECT_EQ(text_of(view, payload.size()), payload);
	// As in the tagged-call test: the settled ignored count is what would catch a packet, since
	// the completion thread takes any packet off the port before the check below could see it.
	EXPECT_EQ(settled_ignored_packets(), before);
	DWORD       bytes = 0;
	ULONG_PTR   key   = 0;
	OVERLAPPED* entry = nullptr;
	EXPECT_EQ(::GetQueuedCompletionStatus(alt::detail::iocp_port_for_testing(backend()), &bytes, &key, &entry, 0), FALSE);
	EXPECT_EQ(entry, nullptr) << "the pool transfer queued a packet to the backend's port";
}

TEST_F(IocpBackendTest, DestroyingTheBackendWithNothingInFlightReturns)
{
	auto file = open_file(write_file("data.txt", "x"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	ASSERT_FALSE(run(positional(alt::io_op::read, **file, buffer_of(1), 0, 1)).error);
	const HANDLE port = alt::detail::iocp_port_for_testing(backend());

	destroy_backend(); // joins the completion thread: a hang here is the failure

	DWORD flags = 0;
	EXPECT_EQ(::GetHandleInformation(port, &flags), FALSE) << "the completion port is still open";
}

namespace
{
	/// The death test's body: a read left pending on an empty pipe when the backend is destroyed.
	/// Any setup failure aborts too, but with no message, so it cannot pass for the assert.
	void destroy_the_backend_with_a_read_in_flight()
	{
		auto pool = alt::thread_pool::create(1);
		if(!pool)
		{
			std::abort();
		}
		auto made = alt::detail::make_iocp_backend(*pool);
		if(!made)
		{
			std::abort();
		}
		std::unique_ptr<alt::io_backend> backend = std::move(*made);
		const served_pipe                pipe;
		if(!pipe.valid())
		{
			std::abort();
		}
		alt::io_open_args  args{.path = std::filesystem::path(pipe.name), .open_flags = alt::flags(alt::open_flag::read), .mode = {}};
		alt::native_handle handle = alt::invalid_native_handle;
		alt::io_request    open;
		open.op  = alt::io_op::open;
		open.in  = &args;
		open.out = &handle;
		waiter opened;
		if(!backend->submit(open, deliver, &opened))
		{
			std::abort();
		}
		opened.done.acquire();
		if(opened.result.error)
		{
			std::abort();
		}
		const auto file   = alt::detail::io_handle::adopt(handle);
		auto       buffer = alt::io_buffer::allocate(4);
		waiter     pending;
		if(!buffer || !backend->submit(positional(alt::io_op::read, *file, std::move(*buffer), 0, 4), deliver, &pending))
		{
			std::abort();
		}
		backend.reset(); // the read is still pending: nothing was written to the pipe
	}
} // namespace

TEST(IocpBackendDeathTest, DestroyingTheBackendWithATransferInFlightAborts)
{
	if(!alt_test::asserts_are_compiled_in)
	{
		GTEST_SKIP() << "the in-flight check is an assert in debug builds and std::terminate() under NDEBUG; "
		                "the death test pins the assert's message, so it runs only where the assert is compiled in.";
	}

	const alt_test::scoped_death_test_style style{"threadsafe"};
	EXPECT_DEATH(destroy_the_backend_with_a_read_in_flight(), "IOCP backend destroyed with transfers in flight");
}

namespace
{
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

	/// The second death test's body: a stat forwarded to the pool, still queued behind a blocked
	/// worker, when the backend is destroyed. A queued open would run its prepare hook against the
	/// closed port later, so any forwarded operation in flight is a precondition violation.
	void destroy_the_backend_with_a_forwarded_operation_in_flight()
	{
		auto pool = alt::thread_pool::create(1);
		if(!pool)
		{
			std::abort();
		}
		auto made = alt::detail::make_iocp_backend(*pool);
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

TEST(IocpBackendDeathTest, DestroyingTheBackendWithAForwardedOperationInFlightAborts)
{
	if(!alt_test::asserts_are_compiled_in)
	{
		GTEST_SKIP() << "the in-flight check is an assert in debug builds and std::terminate() under NDEBUG; "
		                "the death test pins the assert's message, so it runs only where the assert is compiled in.";
	}

	const alt_test::scoped_death_test_style style{"threadsafe"};
	EXPECT_DEATH(destroy_the_backend_with_a_forwarded_operation_in_flight(), "IOCP backend destroyed with forwarded operations in flight");
}

namespace
{
	/// How long a cancel test waits for a callback before it fails rather than hangs.
	constexpr auto cancel_guard = std::chrono::seconds(5);

	/// One read submitted by a cancel test: its callback state, and a view of its buffer.
	struct pipe_read
	{
		alt::io_buffer view;
		waiter         w;
	};

	/// Submits a read of @p size bytes on @p file into a fresh buffer, and marks it submitted.
	std::expected<alt::io_token, std::error_code> submit_read(alt::io_backend& backend, const alt::detail::io_handle& file, pipe_read& read, std::size_t size)
	{
		auto buffer = alt::io_buffer::allocate(size);
		if(!buffer)
		{
			return std::unexpected(buffer.error());
		}
		read.view        = *buffer;
		const auto token = backend.submit(positional(alt::io_op::read, file, std::move(*buffer), 0, size), deliver, &read.w);
		read.w.submitted.store(true);
		return token;
	}
} // namespace

TEST_F(IocpBackendTest, CancellingAPendingReadCompletesItOnceAsCancelled)
{
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	pipe_read  read;
	const auto token = submit_read(backend(), **file, read, 8);
	ASSERT_TRUE(token.has_value()) << token.error().message();
	pipe_releaser releaser(pipe, read.w);
	EXPECT_EQ(read.w.calls.load(), 0) << "the read completed before anything was written to the pipe";

	backend().cancel(*token);
	ASSERT_TRUE(read.w.done.try_acquire_for(cancel_guard)) << "the cancelled read never completed";
	releaser.completed = true;

	EXPECT_EQ(read.w.result.error, std::errc::operation_canceled) << read.w.result.error.message();
	EXPECT_EQ(read.w.result.bytes, 0U);
	EXPECT_FALSE(read.w.ran_inline.load());
	// A second callback, had there been one, would come from the completion thread soon after.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(read.w.calls.load(), 1);
}

TEST_F(IocpBackendTest, CancellingAfterCompletionIsANoOp)
{
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	constexpr std::string_view payload = "finished";
	pipe_read                  read;
	const auto                 token = submit_read(backend(), **file, read, payload.size());
	ASSERT_TRUE(token.has_value()) << token.error().message();
	pipe_releaser releaser(pipe, read.w);
	ASSERT_TRUE(pipe.write(payload));
	ASSERT_TRUE(read.w.done.try_acquire_for(cancel_guard)) << "the read never completed";
	releaser.completed       = true;
	const std::size_t before = ignored_packets();

	// The record is freed: a cancel that still reached it would touch freed memory, which is
	// debug-asan's report. Twice, so a cancel that changed some state on the first is seen too.
	backend().cancel(*token);
	backend().cancel(*token);

	EXPECT_EQ(settled_ignored_packets(), before);
	EXPECT_EQ(read.w.calls.load(), 1);
	EXPECT_FALSE(read.w.result.error) << read.w.result.error.message();
	EXPECT_EQ(read.w.result.bytes, payload.size());
	EXPECT_EQ(text_of(read.view, payload.size()), payload);
}

TEST_F(IocpBackendTest, ACancelForAFinishedTokenDoesNotCancelANewTransferOnTheSameHandle)
{
	// CancelIoEx matches by handle and OVERLAPPED address. A's record is freed when A completes,
	// and B's, allocated next and of the same size, may well land at the same address: a cancel
	// that kept A's address, or reached it after the free, would cancel B.
	//
	// What this guards, and what it does not: it catches a cancel that reaches a transfer by
	// anything other than a live registry entry. Measured: a mutant that kept every record's
	// handle and OVERLAPPED address in a side map never cleared, and cancelled through it,
	// failed this test in 20 of 20 runs under debug, B reporting operation_canceled. It does
	// NOT catch CancelIoEx being called
	// outside the registry lock (looked up under it, called after releasing it). That mutant
	// misfires only if A completes, is freed, and B is submitted at A's address inside the window
	// between the lookup and the call; here A is cancelled only after it has completed, so the
	// lookup finds nothing. Measured: with that mutant applied, this test and every other Cancel
	// test passed 50 of 50 repeats under both debug and debug-asan. The lock is argued by the
	// design's Cancellation section, not proven by a test.
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	constexpr std::string_view first  = "first";
	constexpr std::string_view second = "later";
	for(int round = 0; round < 50; ++round)
	{
		SCOPED_TRACE("round " + std::to_string(round));
		pipe_read  a;
		const auto a_token = submit_read(backend(), **file, a, first.size());
		ASSERT_TRUE(a_token.has_value()) << a_token.error().message();
		{
			pipe_releaser releaser(pipe, a.w);
			ASSERT_TRUE(pipe.write(first));
			ASSERT_TRUE(a.w.done.try_acquire_for(cancel_guard)) << "read A never completed";
			releaser.completed = true;
		}
		ASSERT_FALSE(a.w.result.error) << a.w.result.error.message();

		pipe_read  b;
		const auto b_token = submit_read(backend(), **file, b, second.size());
		ASSERT_TRUE(b_token.has_value()) << b_token.error().message();
		pipe_releaser releaser(pipe, b.w);
		EXPECT_EQ(b.w.calls.load(), 0) << "read B completed before anything was written to the pipe";

		backend().cancel(*a_token);
		ASSERT_TRUE(pipe.write(second));
		ASSERT_TRUE(b.w.done.try_acquire_for(cancel_guard)) << "read B never completed";
		releaser.completed = true;

		EXPECT_EQ(b.w.calls.load(), 1);
		ASSERT_FALSE(b.w.result.error) << "A's cancel reached B: " << b.w.result.error.message();
		EXPECT_EQ(b.w.result.bytes, second.size());
		EXPECT_EQ(text_of(b.view, second.size()), second);
		EXPECT_EQ(a.w.calls.load(), 1);
	}
}

TEST_F(IocpBackendTest, CancelRacingCompletionDeliversExactlyOnce)
{
	served_pipe pipe;
	ASSERT_TRUE(pipe.valid()) << ::GetLastError();
	auto file = open_file(std::filesystem::path(pipe.name), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	pipe.accept();

	constexpr std::string_view payload   = "raced";
	int                        delivered = 0;
	int                        cancelled = 0;
	for(int round = 0; round < 200; ++round)
	{
		SCOPED_TRACE("round " + std::to_string(round));
		pipe_read  read;
		const auto token = submit_read(backend(), **file, read, payload.size());
		ASSERT_TRUE(token.has_value()) << token.error().message();
		pipe_releaser releaser(pipe, read.w);

		// Released together, so the cancel and the data arrive in either order, or at once.
		std::latch        start(2);
		std::atomic<bool> wrote{false};
		{
			const std::jthread canceller([&] {
				start.arrive_and_wait();
				backend().cancel(*token);
			});
			const std::jthread writer([&] {
				start.arrive_and_wait();
				wrote.store(pipe.write(payload));
			});
		}
		ASSERT_TRUE(wrote.load());
		ASSERT_TRUE(read.w.done.try_acquire_for(cancel_guard)) << "the read never completed";
		releaser.completed = true;

		std::size_t consumed = 0;
		if(read.w.result.error)
		{
			EXPECT_EQ(read.w.result.error, std::errc::operation_canceled) << read.w.result.error.message();
			EXPECT_EQ(read.w.result.bytes, 0U);
			++cancelled;
		}
		else
		{
			ASSERT_GT(read.w.result.bytes, 0U);
			ASSERT_LE(read.w.result.bytes, payload.size());
			EXPECT_EQ(text_of(read.view, read.w.result.bytes), payload.substr(0, read.w.result.bytes));
			consumed = read.w.result.bytes;
			++delivered;
		}
		// Whatever the read did not take is still in the pipe. Taking it leaves the pipe empty, so
		// the next round's read starts pending.
		while(consumed < payload.size())
		{
			const auto rest = run(positional(alt::io_op::read, **file, buffer_of(payload.size() - consumed), 0, payload.size() - consumed));
			ASSERT_FALSE(rest.error) << rest.error.message();
			ASSERT_GT(rest.bytes, 0U);
			consumed += rest.bytes;
		}
		EXPECT_EQ(read.w.calls.load(), 1);
	}
	RecordProperty("delivered", delivered);
	RecordProperty("cancelled", cancelled);
	std::cout << "[          ] of 200 raced reads: " << delivered << " delivered, " << cancelled << " cancelled\n";
	// Every read's packet found its record; a second completion of any of them would have been
	// a packet for a withdrawn record, counted here.
	EXPECT_EQ(settled_ignored_packets(), 0U);
}

TEST_F(IocpBackendTest, CancellingAPoolTokenReachesThePool)
{
	// Occupy the pool's only thread so the forwarded stat stays queued behind it.
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
	EXPECT_EQ(token->value() & alt::detail::iocp_token_tag, 0U) << "a forwarded operation's token carries the IOCP tag";

	backend().cancel(*token);
	gate.release.release();
	ASSERT_TRUE(w.done.try_acquire_for(cancel_guard)) << "the cancelled stat never completed";

	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_EQ(w.result.error, std::errc::operation_canceled) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 0U);
}
#endif

// The io_uring backend exists only on Linux, so this whole file does too.
#ifdef __linux__

#	include <gtest/gtest.h>

#	include <alt/detail/file_types.hpp>
#	include <alt/detail/io_handle.hpp>
#	include <alt/io_backend.hpp>
#	include <alt/io_buffer.hpp>
#	include <alt/thread_pool.hpp>
#	include <liburing/io_uring.h>
#	include <atomic>
#	include <chrono>
#	include <cstddef>
#	include <cstdint>
#	include <array>
#	include <cstdlib>
#	include <cstring>
#	include <expected>
#	include <filesystem>
#	include <fstream>
#	include <deque>
#	include <iterator>
#	include <limits>
#	include <memory>
#	include <semaphore>
#	include <string>
#	include <string_view>
#	include <vector>
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

	/// The most completion entries io_uring_setup accepts (IORING_MAX_CQ_ENTRIES). Same reason for the literal.
	constexpr std::size_t max_completion_entries = 2 * max_submission_entries;

	/// The lead of the skip a large-ring test takes when the kernel cannot spare the memory.
	constexpr std::string_view largest_ring_skip_reason = "the largest ring the kernel allows does not fit this environment's budget: ";

	/**
	 * A probe built by hand, so that "an opcode is missing" is a case a test can produce.
	 *
	 * io_uring_probe is a kernel ABI struct with a flexible array of per-opcode entries, declared
	 * in <liburing/io_uring.h>, which is the header this file includes: the ABI definitions
	 * alone, not liburing's own API. Building one needs no library call and nothing newer than
	 * the project's liburing floor, so this is portable rather than a trick that works here.
	 *
	 * @p missing is the one opcode whose IO_URING_OP_SUPPORTED bit is cleared, or -1 for a probe
	 * that reports everything. @p last_op is the highest opcode the probe claims to describe; an
	 * opcode above it reads as unsupported, which is how a kernel too old to know an opcode
	 * answers.
	 */
	class synthetic_probe
	{
	public:
		synthetic_probe(const int last_op, const int missing)
		{
			const std::size_t described = static_cast<std::size_t>(last_op) + 1;
			m_bytes.assign(sizeof(io_uring_probe) + (described * sizeof(io_uring_probe_op)), 0);
			io_uring_probe& probe = get();
			probe.last_op         = static_cast<unsigned char>(last_op);
			probe.ops_len         = static_cast<unsigned char>(described);
			for(int op = 0; op <= last_op; ++op)
			{
				probe.ops[op].op    = static_cast<unsigned char>(op);
				probe.ops[op].flags = op == missing ? 0 : static_cast<unsigned short>(IO_URING_OP_SUPPORTED);
			}
		}

		[[nodiscard]] io_uring_probe& get()
		{
			return *reinterpret_cast<io_uring_probe*>(m_bytes.data());
		}

	private:
		std::vector<unsigned char> m_bytes;
	};

	/// The highest opcode this suite's synthetic probes describe: above every opcode the backend needs.
	constexpr int highest_probed_opcode = IORING_OP_WRITE + 1;

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

		/// Runs @p request and expects a completion of zero bytes and no error. @p what names the case.
		void expect_moves_nothing(const alt::io_request& request, const std::string& what)
		{
			const auto done = run(request);
			EXPECT_FALSE(done.error) << what << ": " << done.error.message();
			EXPECT_EQ(done.bytes, 0U) << what;
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

		/**
		 * Whether @p made failed only because the ring did not fit this environment's memory
		 * budget, rather than because of anything this code decided.
		 *
		 * The largest ring the kernel allows is a few megabytes of kernel memory, which a
		 * constrained container may refuse. A sizing or clamping regression reports EINVAL rather
		 * than ENOMEM, so a test that skips on this cannot be hiding one.
		 */
		static bool ring_did_not_fit(const std::expected<std::unique_ptr<alt::io_backend>, std::error_code>& made)
		{
			return !made && made.error() == std::errc::not_enough_memory;
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
	// unreachable at any queue_depth rather than only at a generous one. The full bound is
	// asserted here, reserved internal completions included, rather than a weaker
	// `>= max_in_flight` that the kernel's own default of twice the submission ring could satisfy
	// by accident at a larger depth.
	EXPECT_GE(alt::detail::io_uring_completion_depth_for_testing(*built), default_max_in_flight + default_max_in_flight + 1);
}

TEST_F(IoUringBackendTest, HonorsAnExplicitQueueDepth)
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
	if(ring_did_not_fit(made))
	{
		GTEST_SKIP() << largest_ring_skip_reason << made.error().message();
	}
	ASSERT_TRUE(made.has_value()) << "an out-of-range queue_depth must be clamped, not refused: " << made.error().message();
	EXPECT_EQ(alt::detail::io_uring_submission_depth_for_testing(**made), max_submission_entries);
}

/**
 * The upper edge of the completion ring's documented guarantee: the largest max_in_flight it
 * covers.
 *
 * Covering max_in_flight costs `2 * max_in_flight + 1` completion entries -- one per operation,
 * one cancel per operation, and the wake-up NOP -- against a kernel ceiling of 65536. 32767 needs
 * 65535 and is the last value that fits.
 */
TEST_F(IoUringBackendTest, CoversTheLargestMaxInFlightItDocuments)
{
	constexpr std::size_t in_flight = 32767;
	auto                  made      = alt::detail::make_io_uring_backend(pool(), 0, in_flight);
	if(ring_did_not_fit(made))
	{
		GTEST_SKIP() << largest_ring_skip_reason << made.error().message();
	}
	ASSERT_TRUE(made.has_value()) << made.error().message();
	EXPECT_GE(alt::detail::io_uring_completion_depth_for_testing(**made), in_flight + in_flight + 1);
}

/**
 * And the first max_in_flight above that edge, which the documentation says is accepted rather
 * than refused, with the completion ring clamped to the kernel's ceiling.
 *
 * 32768 would need 65537 entries and gets 65536, so overflow stops being unreachable. That is
 * deliberate and safe: every kernel past the capability check sets IORING_FEAT_NODROP, so an
 * overflowing completion goes to a kernel-side backlog that the reaper's wait drains, rather than
 * being dropped. What a submission attempted while the ring is overflown does is
 * kernel-dependent -- an older kernel refuses it with EBUSY, the kernel this was measured on
 * accepts it and backlogs its completion -- and the backend treats the refusal as a real,
 * reachable case rather than a curiosity: see submit_to_ring()'s quarantine. What must not happen
 * is the factory REFUSING the value, which is what this pins.
 */
TEST_F(IoUringBackendTest, AcceptsAMaxInFlightAboveTheBoundAndClampsTheCompletionRing)
{
	constexpr std::size_t in_flight = 32768;
	auto                  made      = alt::detail::make_io_uring_backend(pool(), 0, in_flight);
	if(ring_did_not_fit(made))
	{
		GTEST_SKIP() << largest_ring_skip_reason << made.error().message();
	}
	ASSERT_TRUE(made.has_value()) << "a max_in_flight past the covered bound must still build a ring: " << made.error().message();
	EXPECT_EQ(alt::detail::io_uring_completion_depth_for_testing(**made), max_completion_entries);
	// One short of what covering 32768 would take, which is the whole point of the bound.
	EXPECT_LT(alt::detail::io_uring_completion_depth_for_testing(**made), in_flight + in_flight + 1);
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

TEST_F(IoUringBackendTest, ARingOperationsTokenCarriesTheTag)
{
	const auto path = write_file("data.txt", "tagged");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	waiter     w;
	const auto token = backend().submit(positional(alt::io_op::read, **file, buffer_of(6), 0, 6), deliver, &w);
	w.submitted.store(true);
	ASSERT_TRUE(token.has_value()) << token.error().message();

	// The tag is what tells a ring operation's token from a forwarded one's, and it is what
	// routes a cancel. ForwardsAnOperationItDoesNotHandleToThePool pins the other half.
	EXPECT_NE(token->value() & alt::detail::io_uring_token_tag, 0U) << "a ring operation's token does not carry the tag";
	// And neither reserved value: the wake-up NOP's user_data is the tag with an id of zero, and
	// the id above it is held for a cancel SQE, so an operation may take neither.
	EXPECT_NE(token->value(), alt::detail::io_uring_token_tag);
	EXPECT_NE(token->value(), alt::detail::io_uring_token_tag | 1U);

	ASSERT_TRUE(w.done.try_acquire_for(completion_guard)) << "the read never completed";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 6U);
	// A ring completion comes from the reaper thread: the submission never completes it inline,
	// unlike the IOCP arm, where a synchronous transfer does. Asserted on the completing THREAD
	// rather than on waiter::ran_inline, which this test cannot read without a race -- ran_inline
	// is derived from a flag the test sets after submit() returns, and a ring completion can beat
	// that store. The thread identity is the same property with no race in it.
	EXPECT_NE(w.thread, std::this_thread::get_id());
	EXPECT_EQ(alt::detail::io_uring_ignored_completions_for_testing(backend()), 0U);
}

TEST_F(IoUringBackendTest, AnAppendWriteLandsAtTheEndWhateverOffsetIsAsked)
{
	// An O_APPEND descriptor appends whatever offset the submission entry names, which is the
	// property phase 7c pinned for IOCP. Neither offset below is the end of the file: honoring
	// the first would overwrite the start, and honoring the second would leave a hole.
	const auto path = write_file("data.txt", "start");
	auto       file = open_file(path, alt::flags(alt::open_flag::write) | alt::flags(alt::open_flag::append));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	constexpr std::array<std::pair<std::string_view, std::uint64_t>, 2> writes{{{"-one", 0}, {"-two", 1000}}};
	for(const auto& [text, offset]: writes)
	{
		auto buffer = buffer_of(text.size());
		std::memcpy(buffer.bytes().data(), text.data(), text.size());
		const auto done = run(positional(alt::io_op::write, **file, std::move(buffer), offset, text.size()));
		EXPECT_FALSE(done.error) << "offset " << offset << ": " << done.error.message();
		EXPECT_EQ(done.bytes, text.size()) << "offset " << offset;
	}
	file->reset();
	EXPECT_EQ(read_whole(path), "start-one-two");
	// The content check alone would pass on a file with a hole after it; the size is what says
	// the offset of 1000 moved nothing.
	EXPECT_EQ(std::filesystem::file_size(path), 13U);
}

TEST_F(IoUringBackendTest, AShortReadReportsTheShortCountAndNoError)
{
	auto file = open_file(write_file("short.txt", "short"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto       buffer = buffer_of(16);
	const auto view   = buffer;
	const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 2, 16));

	// Three bytes left in the file and sixteen asked for: a short count is a real result, not an
	// error, and it is returned as-is rather than looped over.
	EXPECT_FALSE(done.error) << done.error.message();
	EXPECT_EQ(done.bytes, 3U);
	EXPECT_EQ(text_of(view, 3), "ort");
}

TEST_F(IoUringBackendTest, AReadAtEndOfFileReportsZeroBytesAndNoError)
{
	// "hello world" is eleven bytes, so offset eleven is the first position past the last byte.
	auto file = open_file(write_file("data.txt", "hello world"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	expect_moves_nothing(positional(alt::io_op::read, **file, buffer_of(4), 11, 4), "at the end");
}

TEST_F(IoUringBackendTest, AReadPastEndOfFileReportsZeroBytesAndNoError)
{
	// Well past it, rather than at it: the kernel reports both as a zero-byte success, which is
	// what the pool arm's pread already reports and what the file layer treats as a real result.
	auto file = open_file(write_file("data.txt", "hello world"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	expect_moves_nothing(positional(alt::io_op::read, **file, buffer_of(4), 4096, 4), "past the end");
}

TEST_F(IoUringBackendTest, ARequestLargerThanItsBufferIsClampedToTheBuffer)
{
	auto file = open_file(write_file("data.txt", "0123456789"), alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	auto       buffer = buffer_of(4);
	const auto view   = buffer;
	const auto done   = run(positional(alt::io_op::read, **file, std::move(buffer), 0, std::numeric_limits<std::size_t>::max()));

	// The clamp itself is what is asserted, not that the call succeeded: a request past the end
	// of the buffer must report at most what the buffer holds, and here exactly that, since the
	// file is longer than the buffer.
	EXPECT_FALSE(done.error) << done.error.message();
	EXPECT_LE(done.bytes, 4U);
	EXPECT_EQ(done.bytes, 4U);
	EXPECT_EQ(text_of(view, 4), "0123");
}

TEST_F(IoUringBackendTest, AZeroSizeTransferCompletesWithZeroBytes)
{
	// A transfer that moves nothing must still complete exactly once, rather than be skipped or
	// wait for a completion the kernel is not going to post.
	const auto path = write_file("data.txt", "0123456789");
	auto       file = open_file(path, alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	for(const auto op: {alt::io_op::read, alt::io_op::write})
	{
		const std::string what = op == alt::io_op::read ? "read" : "write";
		expect_moves_nothing(positional(op, **file, buffer_of(4), 2, 0), what + " of size 0");
		// The other way to ask for nothing: a request whose buffer is empty, which the shared
		// clamp turns into a count of zero over a null address.
		expect_moves_nothing(positional(op, **file, alt::io_buffer{}, 2, 4), what + " with an empty buffer");
	}
	file->reset();
	EXPECT_EQ(read_whole(path), "0123456789");
}

namespace
{
	/// A request for @p op on @p handle with no buffer: the shape both fsync forms take.
	alt::io_request sync_request(const alt::io_op op, const alt::native_handle handle)
	{
		alt::io_request request;
		request.op     = op;
		request.handle = handle;
		return request;
	}
} // namespace

TEST_F(IoUringBackendTest, SyncFullReachesTheRingAndSucceeds)
{
	auto file = open_file(write_file("data.txt", "durable"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	waiter     w;
	const auto token = backend().submit(sync_request(alt::io_op::sync_full, (*file)->native()), deliver, &w);
	w.submitted.store(true);
	ASSERT_TRUE(token.has_value()) << token.error().message();
	EXPECT_NE(token->value() & alt::detail::io_uring_token_tag, 0U) << "sync_full was forwarded to the pool, not put on the ring";

	ASSERT_TRUE(w.done.try_acquire_for(completion_guard)) << "the fsync never completed";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 0U);
	EXPECT_NE(w.thread, std::this_thread::get_id());
	EXPECT_EQ(alt::detail::io_uring_ignored_completions_for_testing(backend()), 0U);
}

TEST_F(IoUringBackendTest, SyncDataReachesTheRingAndSucceeds)
{
	auto file = open_file(write_file("data.txt", "durable"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	waiter     w;
	const auto token = backend().submit(sync_request(alt::io_op::sync_data, (*file)->native()), deliver, &w);
	w.submitted.store(true);
	ASSERT_TRUE(token.has_value()) << token.error().message();
	EXPECT_NE(token->value() & alt::detail::io_uring_token_tag, 0U) << "sync_data was forwarded to the pool, not put on the ring";

	ASSERT_TRUE(w.done.try_acquire_for(completion_guard)) << "the fdatasync never completed";
	EXPECT_EQ(w.calls.load(), 1);
	EXPECT_FALSE(w.result.error) << w.result.error.message();
	EXPECT_EQ(w.result.bytes, 0U);
	EXPECT_NE(w.thread, std::this_thread::get_id());
	EXPECT_EQ(alt::detail::io_uring_ignored_completions_for_testing(backend()), 0U);
}

TEST(IoUringOperationTest, EachOfTheFourRingOpsMapsToItsOwnOperation)
{
	using alt::detail::io_uring_operation;
	EXPECT_EQ(alt::detail::io_uring_operation_of(alt::io_op::read), io_uring_operation::read);
	EXPECT_EQ(alt::detail::io_uring_operation_of(alt::io_op::write), io_uring_operation::write);
	EXPECT_EQ(alt::detail::io_uring_operation_of(alt::io_op::sync_full), io_uring_operation::fsync);
	EXPECT_EQ(alt::detail::io_uring_operation_of(alt::io_op::sync_data), io_uring_operation::fdatasync);
}

TEST(IoUringOperationTest, OnlyTheDataSyncFormCarriesTheDataSyncFlag)
{
	using alt::detail::io_uring_operation;
	// sync_full and sync_data differ in this field alone and the kernel answers both with a zero,
	// so no completion can show a swapped flag: the mapping is asserted here instead.
	EXPECT_EQ(alt::detail::io_uring_fsync_flags_of(io_uring_operation::fsync), 0U);
	EXPECT_EQ(alt::detail::io_uring_fsync_flags_of(io_uring_operation::fdatasync), static_cast<unsigned>(IORING_FSYNC_DATASYNC));
	// The transfers prepare no fsync entry; the function answers zero rather than a stray flag.
	EXPECT_EQ(alt::detail::io_uring_fsync_flags_of(io_uring_operation::read), 0U);
	EXPECT_EQ(alt::detail::io_uring_fsync_flags_of(io_uring_operation::write), 0U);
}

TEST_F(IoUringBackendTest, AnOperationOnAClosedDescriptorReportsBadFileDescriptor)
{
	auto file = open_file(write_file("data.txt", "x"), alt::flags(alt::open_flag::read_write));
	ASSERT_TRUE(file.has_value()) << file.error().message();
	const auto stale = (*file)->native();
	file->reset(); // closes the descriptor; the value now names nothing

	for(const auto op: {alt::io_op::read, alt::io_op::write})
	{
		const auto done = run(positional(op, stale, buffer_of(1), 0, 1));
		// A completion entry carries a NEGATED errno. The named condition is what pins the sign:
		// handing the raw res to the errno mapping would produce an error_code holding a negative
		// number, which is still an error and would pass a bare "it failed" assertion.
		EXPECT_EQ(done.error, std::errc::bad_file_descriptor) << done.error.message();
		EXPECT_EQ(done.bytes, 0U);
	}
}

namespace
{
	/// One read of ManyConcurrentTransfersAllComplete: its callback state, and a view of its buffer.
	struct concurrent_read
	{
		alt::io_buffer view;
		waiter         w;
	};

	/// Submits one chunk read of @p read, and returns whether it was accepted with a ring token.
	bool submit_chunk_read(alt::io_backend& ring, const alt::detail::io_handle& file, concurrent_read& read, std::size_t index, std::size_t chunk)
	{
		auto buffer = alt::io_buffer::allocate(chunk);
		EXPECT_TRUE(buffer.has_value());
		if(!buffer)
		{
			return false;
		}
		read.view        = *buffer;
		const auto token = ring.submit(positional(alt::io_op::read, file, std::move(*buffer), index * chunk, chunk), deliver, &read.w);
		read.w.submitted.store(true);
		if(!token)
		{
			ADD_FAILURE() << "submission " << index << ": " << token.error().message();
			return false;
		}
		EXPECT_NE(token->value() & alt::detail::io_uring_token_tag, 0U) << "submission " << index;
		return true;
	}

	/// Waits for @p read and checks that it moved exactly @p expected.
	void expect_chunk_read(concurrent_read& read, std::size_t index, const std::string& expected)
	{
		ASSERT_TRUE(read.w.done.try_acquire_for(completion_guard)) << "transfer " << index << " never completed";
		EXPECT_EQ(read.w.calls.load(), 1) << "transfer " << index;
		EXPECT_FALSE(read.w.result.error) << "transfer " << index << ": " << read.w.result.error.message();
		EXPECT_EQ(read.w.result.bytes, expected.size()) << "transfer " << index;
		EXPECT_EQ(text_of(read.view, expected.size()), expected) << "transfer " << index;
	}
} // namespace

TEST_F(IoUringBackendTest, ManyConcurrentTransfersAllComplete)
{
	// Many times more operations outstanding at once than the submission ring has entries, which
	// is the sizing's own claim: an io_uring_submit() the kernel accepts empties the queue inside
	// the submission lock, so the depth bounds a burst and not the work in flight. The completion
	// ring, which does bound it, is sized from max_in_flight. (Every submission here is accepted;
	// a refused one would leave its entry unconsumed, which is submit_to_ring()'s subject and not
	// something this test can produce.)
	constexpr std::size_t transfers = 256;
	constexpr std::size_t chunk     = 4;
	constexpr std::size_t depth     = 8;

	std::string content(transfers * chunk, '\0');
	for(std::size_t i = 0; i < content.size(); ++i)
	{
		content[i] = static_cast<char>('a' + (i % 26));
	}
	const auto path = write_file("many.bin", content);

	const auto ring = build(depth, transfers);
	ASSERT_NE(ring, nullptr);
	ASSERT_EQ(alt::detail::io_uring_submission_depth_for_testing(*ring), depth);
	// The descriptor is an ordinary one, so the fixture's backend opens it and the ring under
	// test performs the transfers; a ring needs nothing special of a descriptor.
	auto file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	std::deque<concurrent_read> reads(transfers); // deque: waiter holds semaphores and cannot move
	std::size_t                 submitted = 0;
	while(submitted < transfers && submit_chunk_read(*ring, **file, reads[submitted], submitted, chunk))
	{
		++submitted;
	}
	for(std::size_t i = 0; i < submitted; ++i)
	{
		expect_chunk_read(reads[i], i, content.substr(i * chunk, chunk));
	}
	EXPECT_EQ(submitted, transfers);
	// Every completion found its record: a user_data that did not match its token would leave the
	// callbacks unrun and this count equal to the number of transfers.
	EXPECT_EQ(alt::detail::io_uring_ignored_completions_for_testing(*ring), 0U);
}

namespace
{
	/// State for ACompletionCallbackMaySubmitAnotherOperation: the outcome of the first read, and
	/// the second read the callback submits from inside it.
	struct nested_submission
	{
		alt::io_backend*      backend = nullptr;
		alt::io_request       next;
		waiter                inner;
		alt::io_completion    outer{};
		std::binary_semaphore outer_done{0};
		std::atomic<bool>     inner_submitted{false};
	};

	void submit_from_callback(void* state, alt::io_completion result) noexcept
	{
		auto& nested     = *static_cast<nested_submission*>(state);
		nested.outer     = result;
		const auto token = nested.backend->submit(nested.next, deliver, &nested.inner);
		nested.inner.submitted.store(true);
		nested.inner_submitted.store(token.has_value());
		nested.outer_done.release();
	}
} // namespace

TEST_F(IoUringBackendTest, ACompletionCallbackMaySubmitAnotherOperation)
{
	// No callback runs under a lock of this backend's. A callback is the file layer's operation
	// block's entry point, takes locks of its own, and may call straight back into submit() --
	// which this one does, from the reaper thread. A reaper that held the registry lock across
	// the callback would deadlock in the nested enroll and this test would never return.
	const auto path = write_file("nested.txt", "0123456789");
	auto       file = open_file(path, alt::flags(alt::open_flag::read));
	ASSERT_TRUE(file.has_value()) << file.error().message();

	nested_submission nested;
	nested.backend        = &backend();
	auto       inner_data = buffer_of(3);
	const auto inner_view = inner_data;
	nested.next           = positional(alt::io_op::read, **file, std::move(inner_data), 5, 3);

	auto       outer_data = buffer_of(4);
	const auto outer_view = outer_data;
	const auto token      = backend().submit(positional(alt::io_op::read, **file, std::move(outer_data), 0, 4), submit_from_callback, &nested);
	ASSERT_TRUE(token.has_value()) << token.error().message();

	ASSERT_TRUE(nested.outer_done.try_acquire_for(completion_guard)) << "the first read never completed";
	EXPECT_FALSE(nested.outer.error) << nested.outer.error.message();
	EXPECT_EQ(nested.outer.bytes, 4U);
	EXPECT_EQ(text_of(outer_view, 4), "0123");

	ASSERT_TRUE(nested.inner_submitted.load()) << "the callback could not submit from the reaper thread";
	ASSERT_TRUE(nested.inner.done.try_acquire_for(completion_guard)) << "the read submitted from the callback never completed";
	EXPECT_EQ(nested.inner.calls.load(), 1);
	EXPECT_FALSE(nested.inner.result.error) << nested.inner.result.error.message();
	EXPECT_EQ(nested.inner.result.bytes, 3U);
	EXPECT_EQ(text_of(inner_view, 3), "567");
}

/**
 * An ENVIRONMENT canary, and nothing more: this runner can build a backend at all.
 *
 * It says nothing about whether the factory consults its capability check, and cannot: on a
 * kernel with all four opcodes -- every kernel this project builds on -- the check's refusal is
 * unreachable, so deleting the check outright leaves this test green. The decision the check
 * makes is pinned by AcceptsAProbeReportingEveryOpcodeItNeeds and the two refusal cases below,
 * which put a synthetic probe through the real predicate. That the factory CALLS the predicate is
 * untested, and recorded as untested.
 *
 * Its entire job is to FAIL, never to skip, when no usable ring can be created: a runner that
 * stops permitting io_uring must turn this suite red rather than leave the whole backend
 * unexercised behind a green sweep. Do not "fix" it into a GTEST_SKIP.
 */
TEST_F(IoUringBackendTest, TheRunnerProvidesARingThisBackendCanUse)
{
	auto made = alt::detail::make_io_uring_backend(pool(), 0, default_max_in_flight);
	ASSERT_TRUE(made.has_value()) << "io_uring is unavailable or lacks an opcode this backend needs: " << (made ? std::string{} : made.error().message());
	EXPECT_EQ((*made)->kind(), alt::io_backend_kind::io_uring);
}

TEST(IoUringCapabilityTest, AcceptsAProbeReportingEveryOpcodeItNeeds)
{
	synthetic_probe complete(highest_probed_opcode, -1);
	EXPECT_TRUE(alt::detail::io_uring_probe_has_required_opcodes(complete.get()));
}

TEST(IoUringCapabilityTest, RefusesAProbeMissingAnyOneOfTheFour)
{
	// One case per opcode, so that dropping any single one of the four from the check is a named
	// failure rather than a gap another case happens to cover.
	for(const int opcode: {IORING_OP_READ, IORING_OP_WRITE, IORING_OP_FSYNC, IORING_OP_ASYNC_CANCEL})
	{
		synthetic_probe without_one(highest_probed_opcode, opcode);
		EXPECT_FALSE(alt::detail::io_uring_probe_has_required_opcodes(without_one.get())) << "opcode " << opcode << " is not required by the check";
	}
}

/**
 * A kernel too old to know an opcode reports a last_op below it, rather than a cleared bit.
 *
 * This is the 5.1-to-5.5 shape the check exists for: ASYNC_CANCEL is opcode 14 and READ is 22, so
 * a probe whose last_op is 14 describes a kernel that has cancel and not reads -- exactly the
 * kernel that creates a ring and then fails every transfer with EINVAL.
 */
TEST(IoUringCapabilityTest, RefusesAProbeWhoseHighestOpcodeIsBelowRead)
{
	synthetic_probe old_kernel(IORING_OP_ASYNC_CANCEL, -1);
	EXPECT_FALSE(alt::detail::io_uring_probe_has_required_opcodes(old_kernel.get()));
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

namespace
{
	/// A pipe nothing is ever written to, so a read submitted on its read end stays in flight.
	class empty_pipe
	{
	public:
		empty_pipe()
		{
			if(::pipe(m_fds.data()) != 0)
			{
				m_fds = {-1, -1};
			}
		}

		~empty_pipe()
		{
			for(const int fd: m_fds)
			{
				if(fd >= 0)
				{
					::close(fd);
				}
			}
		}

		empty_pipe(const empty_pipe&)            = delete;
		empty_pipe& operator=(const empty_pipe&) = delete;
		empty_pipe(empty_pipe&&)                 = delete;
		empty_pipe& operator=(empty_pipe&&)      = delete;

		[[nodiscard]] bool valid() const
		{
			return m_fds[0] >= 0;
		}

		[[nodiscard]] alt::native_handle read_end() const
		{
			return m_fds[0];
		}

	private:
		std::array<int, 2> m_fds{-1, -1};
	};

	/**
	 * The third death test's body: a read submitted to the RING, left pending on an empty pipe,
	 * when the backend is destroyed.
	 *
	 * This is the ring arm's half of the precondition the other two death tests cover for the
	 * pool arm. Freeing the record would hand the kernel a buffer that is gone to write into,
	 * which no sanitizer reliably reports, so the destructor stops the process instead.
	 *
	 * Any setup failure aborts too, but with no message, so it cannot pass for the assert.
	 */
	void destroy_the_backend_with_a_ring_operation_in_flight()
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
		const empty_pipe                 pipe;
		auto                             buffer = alt::io_buffer::allocate(4);
		if(!pipe.valid() || !buffer)
		{
			std::abort();
		}
		waiter          pending;
		alt::io_request read;
		read.op          = alt::io_op::read;
		read.handle      = pipe.read_end();
		read.buffer      = std::move(*buffer);
		read.size        = 4;
		const auto token = backend->submit(read, deliver, &pending);
		// A token without the tag would mean the read went to the pool, which would make this the
		// forwarded case over again rather than the ring's.
		if(!token || (token->value() & alt::detail::io_uring_token_tag) == 0)
		{
			std::abort();
		}
		backend.reset(); // the read is still pending: nothing was written to the pipe
	}
} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): as the death test above, whose note explains the measurement.
TEST(IoUringBackendDeathTest, DestroyingItWithARingOperationInFlightTerminates)
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
	EXPECT_DEATH(destroy_the_backend_with_a_ring_operation_in_flight(), "io_uring backend destroyed with ring operations in flight");
}

#endif

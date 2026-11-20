#include <gtest/gtest.h>

#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <system_error>
#include <thread>

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

TEST(IoBackendTest, CompletingFromAnotherThreadRunsTheCallbackExactlyOnce)
{
	alt::manual_io_backend backend;
	recorder               rec;
	ASSERT_TRUE(backend.submit(read_request(), record, &rec).has_value());

	{
		std::jthread completer([&backend] { backend.complete(0, done(5)); });
	}

	EXPECT_EQ(rec.calls, 1);
	EXPECT_EQ(rec.last.bytes, 5U);
	EXPECT_EQ(backend.pending(), 0U);
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

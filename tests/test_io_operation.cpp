#include <gtest/gtest.h>

#include <algorithm>
#include <alt/cancellable_task.hpp>
#include <alt/cancellation_ref.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_operation.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/io_backend.hpp>
#include <alt/io_buffer.hpp>
#include <alt/scheduler.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "coroutine_test_support.hpp"

namespace
{

	using alt::detail::io_awaiter;
	using alt::detail::io_handle;
	using alt::detail::io_shared;

	/**
	 * Like alt_test::frame_tracker, but logging through the mutex-guarded pair rather
	 * than through the unsynchronized destruction_order vector. Every chain in this
	 * suite is torn down by an awaiter that registers a std::stop_callback on its
	 * chain's token, so one branch of a teardown can run on the thread that requested
	 * the stop while the other runs on the thread that completed the operation. That is
	 * the shape coroutine_test_support.hpp's own comment names as racy for the plain
	 * vector.
	 */
	struct logged_frame
	{
		explicit logged_frame(int tag): m_tag(tag)
		{
			++alt_test::live_frames;
		}

		~logged_frame()
		{
			--alt_test::live_frames;
			alt_test::record_destruction(m_tag);
		}

		logged_frame(const logged_frame&)            = delete;
		logged_frame& operator=(const logged_frame&) = delete;
		logged_frame(logged_frame&&)                 = delete;
		logged_frame& operator=(logged_frame&&)      = delete;

	private:
		int m_tag;
	};

	/** What one awaiting chain reports back to the test that started it. */
	struct op_record
	{
		bool                    slot_acquired = false;
		bool                    body_ran      = false;
		bool                    threw         = false;
		alt::io_completion      result{};
		std::coroutine_handle<> self;
	};

	/**
	 * Hands the awaiting coroutine its own handle without suspending, so a test that
	 * deliberately leaks a chain can still reclaim the frame at the end.
	 */
	struct capture_self
	{
		std::coroutine_handle<>* out;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		bool await_suspend(std::coroutine_handle<> h) const noexcept
		{
			*out = h;
			return false;
		}

		void await_resume() const noexcept
		{}
	};

	/**
	 * Hands the awaiting coroutine a view of its own chain without suspending.
	 * Templated over the promise for the reason alt::get_cancellation_ref documents:
	 * an erased handle yields an empty view.
	 */
	struct capture_chain
	{
		alt::cancellation_ref* out;

		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h) const noexcept
		{
			*out = alt::get_cancellation_ref(h);
			return false;
		}

		void await_resume() const noexcept
		{}
	};

	/** A read request over @p buffer against @p file, with every field spelled out. */
	alt::io_request read_request(const alt::io_buffer& buffer, const std::shared_ptr<io_handle>& file)
	{
		return alt::io_request{
		  .op     = alt::io_op::read,
		  .handle = file->native(),
		  .buffer = buffer,
		  .offset = 0,
		  .size   = buffer.size(),
		  .in     = nullptr,
		  .out    = nullptr,
		  .state  = nullptr,
		};
	}

	std::shared_ptr<io_shared> make_shared_state(alt::io_backend& backend, const alt::scheduler_ref completion, std::ptrdiff_t max_in_flight = 4)
	{
		return std::make_shared<io_shared>(backend, backend, completion, max_in_flight);
	}

	/**
	 * The two-phase operation the design describes: a backpressure permit, then the
	 * submitted phase. The io_awaiter takes the permit over and releases it, so the
	 * body does not.
	 */
	alt::cancellable_task awaiting_chain(std::shared_ptr<io_shared> shared, alt::io_request request, std::shared_ptr<io_handle> file, std::shared_ptr<op_record> record)
	{
		const logged_frame tracker{1};
		co_await capture_self{&record->self};

		auto permit = co_await alt::detail::acquire_permit(shared);
		if(!permit)
		{
			co_return;
		}
		record->slot_acquired = true;

		record->result   = co_await io_awaiter{shared, std::move(request), std::move(file), std::move(*permit)};
		record->body_ran = true;
	}

	/**
	 * The same chain, with a second std::stop_callback registered on the chain's own
	 * token before the operation starts. See the test that uses it for why that second
	 * registration is what makes the teardown's hazard visible on libstdc++ at all.
	 */
	alt::cancellable_task chain_with_a_second_stop_callback(std::shared_ptr<io_shared> shared,
	                                                        alt::io_request            request,
	                                                        std::shared_ptr<io_handle> file,
	                                                        std::shared_ptr<op_record> record)
	{
		const logged_frame tracker{1};

		alt::cancellation_ref view;
		co_await capture_chain{&view};

		// Registered FIRST, which puts it at the TAIL of the walk: both libstdc++ and
		// MSVC's STL attach at the head of the callback list and pop the head, so the
		// awaiter's own callback -- registered second, inside its await_suspend below --
		// runs first and tears this chain down, leaving this one for the walk to reach
		// afterwards.
		std::optional<std::stop_callback<std::function<void()>>> noop;
		noop.emplace(view.get_stop_token(), std::function<void()>{[] {}});

		auto permit = co_await alt::detail::acquire_permit(shared);
		if(!permit)
		{
			co_return;
		}
		record->slot_acquired = true;

		record->result   = co_await io_awaiter{shared, std::move(request), std::move(file), std::move(*permit)};
		record->body_ran = true;
	}

	/** A backend that records which thread each submission arrived on, then forwards. */
	class recording_backend final: public alt::io_backend
	{
	public:
		explicit recording_backend(alt::io_backend& inner) noexcept: m_inner(&inner)
		{}

		recording_backend(const recording_backend&)            = delete;
		recording_backend& operator=(const recording_backend&) = delete;
		recording_backend(recording_backend&&)                 = delete;
		recording_backend& operator=(recording_backend&&)      = delete;
		~recording_backend() override                          = default;

		[[nodiscard]] std::expected<alt::io_token, std::error_code> submit(const alt::io_request& request,
		                                                                   alt::io_completion_fn  on_done,
		                                                                   void*                  state) override
		{
			{
				const std::scoped_lock lock{m_mutex};
				m_threads.push_back(std::this_thread::get_id());
			}
			return m_inner->submit(request, on_done, state);
		}

		void cancel(alt::io_token token) noexcept override
		{
			m_inner->cancel(token);
		}

		[[nodiscard]] alt::io_backend_kind kind() const noexcept override
		{
			return m_inner->kind();
		}

		[[nodiscard]] std::vector<std::thread::id> threads() const
		{
			const std::scoped_lock lock{m_mutex};
			return m_threads;
		}

	private:
		alt::io_backend*             m_inner;
		mutable std::mutex           m_mutex;
		std::vector<std::thread::id> m_threads;
	};

	/**
	 * Completes @p index from another thread and reports which thread that was. Lifted
	 * out of the test body it serves, where the nested lambda is what pushes that body
	 * past clang-tidy's cognitive-complexity threshold.
	 */
	std::thread::id complete_on_another_thread(alt::manual_io_backend& backend, const std::size_t index, const alt::io_completion result)
	{
		std::thread::id used{};
		{
			const std::jthread completer{[&backend, index, result, &used] {
				used = std::this_thread::get_id();
				backend.complete(index, result);
			}};
		}
		return used;
	}

	/**
	 * A backend whose submit() runs a hook and then throws, which the seam permits
	 * nothing about: a throwing submit is neither of the two outcomes it specifies, so
	 * the awaiter treats it as a submission that never happened. The hook is what makes
	 * the detached arm of that path reachable, by requesting a stop from inside submit()
	 * while the awaiter's gate is still up.
	 */
	class throwing_backend final: public alt::io_backend
	{
	public:
		explicit throwing_backend(std::function<void()> before_throw) noexcept: m_before_throw(std::move(before_throw))
		{}

		throwing_backend(const throwing_backend&)            = delete;
		throwing_backend& operator=(const throwing_backend&) = delete;
		throwing_backend(throwing_backend&&)                 = delete;
		throwing_backend& operator=(throwing_backend&&)      = delete;
		~throwing_backend() override                         = default;

		[[nodiscard]] std::expected<alt::io_token, std::error_code> submit(const alt::io_request& /*request*/,
		                                                                   alt::io_completion_fn /*on_done*/,
		                                                                   void* /*state*/) override
		{
			++m_submissions;

			if(m_before_throw)
			{
				m_before_throw();
			}

			throw std::bad_alloc{};
		}

		void cancel(alt::io_token /*token*/) noexcept override
		{
			++m_cancels;
		}

		[[nodiscard]] alt::io_backend_kind kind() const noexcept override
		{
			return alt::io_backend_kind::manual;
		}

		[[nodiscard]] int submissions() const noexcept
		{
			return m_submissions;
		}

		[[nodiscard]] int cancels() const noexcept
		{
			return m_cancels;
		}

	private:
		std::function<void()> m_before_throw;
		int                   m_submissions = 0;
		int                   m_cancels     = 0;
	};

	/**
	 * A throwing_backend hook that requests a stop on @p source from inside submit(), so
	 * the block is detached while the awaiter's gate is still up. Lifted out of the test
	 * bodies it serves, where the lambda's nesting is what pushes them past clang-tidy's
	 * cognitive-complexity threshold.
	 */
	std::function<void()> stop_from_inside_submit(std::stop_source& source)
	{
		return [&source] { static_cast<void>(source.request_stop()); };
	}

	/** The same chain, but reporting what the co_await threw instead of letting it escape. */
	alt::cancellable_task chain_catching_the_await(std::shared_ptr<io_shared> shared,
	                                               alt::io_request            request,
	                                               std::shared_ptr<io_handle> file,
	                                               std::shared_ptr<op_record> record)
	{
		const logged_frame tracker{1};

		auto permit = co_await alt::detail::acquire_permit(shared);
		if(!permit)
		{
			co_return;
		}
		record->slot_acquired = true;

		try
		{
			record->result = co_await io_awaiter{shared, std::move(request), std::move(file), std::move(*permit)};
		}
		catch(const std::bad_alloc&)
		{
			record->threw = true;
			co_return;
		}

		record->body_ran = true;
	}

	/** A completion scheduler with nowhere to post. See the test that uses it. */
	struct throwing_scheduler
	{
		static void post(alt::work_item /*item*/)
		{
			throw std::bad_alloc{};
		}
	};

} // namespace

TEST(IoOperationTest, ACompletionBeforeAnyStopRunsTheAwaitingBody)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_TRUE(record->slot_acquired);
	ASSERT_EQ(backend.pending(), 1U);
	ASSERT_FALSE(record->body_ran);

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});

	// The callback ran on this thread inside complete(); the awaiting body must not
	// have, because the resumption is posted to the completion scheduler.
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(record->body_ran);
	EXPECT_EQ(record->result.bytes, 16U);
	EXPECT_FALSE(record->result.error);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(chain.stop_requested());
}

TEST(IoOperationTest, AStopBeforeTheCompletionAbandonsTheChainAndTheBodyNeverRuns)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);
	ASSERT_EQ(alt_test::live_frames, 1);

	EXPECT_TRUE(chain.request_stop());

	// The stop callback only posts the teardown; it does not tear down where it runs.
	EXPECT_EQ(alt_test::live_frames, 1);
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::take_destruction_order(), std::vector<int>{1});

	// The operation is still the backend's, and completing it must stay safe.
	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(home.drain(), 0U);
}

TEST(IoOperationTest, ACompletionAfterAStopFindsTheBlockDetachedAndTouchesNoFrame)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);

	EXPECT_TRUE(chain.request_stop());
	EXPECT_EQ(home.drain(), 1U);
	ASSERT_EQ(alt_test::live_frames, 0);

	// Detaching asks the backend to stop early. The request is a hint; the completion
	// still arrives exactly once.
	EXPECT_TRUE(backend.was_cancelled(0));

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});

	// Nothing was posted, which is how "touches no frame" is observable from outside:
	// a completion that found the block detached has no handle and no chain to use.
	EXPECT_EQ(home.drain(), 0U);
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(backend.pending(), 0U);
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(IoOperationTest, AStopAfterTheCompletionFindsTheBlockDoneAndDoesNothing)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
	EXPECT_TRUE(chain.request_stop());

	// Exactly one queued item: the resumption the completion posted. The stop callback
	// found the block already done and posted nothing of its own.
	EXPECT_EQ(home.drain(), 1U);

	// It also asked the backend for nothing, a completed operation having nothing left
	// to cancel.
	EXPECT_FALSE(backend.was_cancelled(0));

	// resume_or_abandon observes the stop that arrived while the resumption was queued,
	// so the body still does not run.
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(IoOperationTest, AnInlineCompletionInsideSubmitIsStillPostedNotRunInline)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	backend.complete_inline_next(alt::io_completion{.bytes = 8, .error = {}});

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_TRUE(record->slot_acquired);
	ASSERT_EQ(backend.pending(), 0U);

	// The callback fired inside submit(), on this thread. The body must still wait for
	// the completion scheduler.
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 1);

	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(record->body_ran);
	EXPECT_EQ(record->result.bytes, 8U);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(chain.stop_requested());
}

TEST(IoOperationTest, ACancelWhileParkedOnASlotNeverReachesTheBackend)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home}, 1);
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> first  = std::make_shared<op_record>();
	const std::shared_ptr<op_record> second = std::make_shared<op_record>();

	const alt::cancellation_handle chain_a = alt::start(awaiting_chain(shared, read_request(buffer, file), file, first));
	alt::cancellation_handle       chain_b = alt::start(awaiting_chain(shared, read_request(buffer, file), file, second));

	ASSERT_TRUE(first->slot_acquired);
	ASSERT_FALSE(second->slot_acquired);
	ASSERT_EQ(backend.pending(), 1U);
	ASSERT_EQ(alt_test::live_frames, 2);

	EXPECT_TRUE(chain_b.request_stop());

	// Released without a permit, so the second chain ended before it built a request.
	EXPECT_FALSE(second->slot_acquired);
	EXPECT_FALSE(second->body_ran);
	EXPECT_EQ(backend.pending(), 1U);
	EXPECT_EQ(alt_test::live_frames, 1);

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(first->body_ran);
	EXPECT_EQ(backend.pending(), 0U);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(chain_a.stop_requested());
}

TEST(IoOperationTest, TheBufferSurvivesTheChainsTeardownAndTheBackendsLateWrite)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);

	// io_buffer::borrow with the test holding only a weak reference, rather than
	// io_buffer::allocate with the test holding a copy. A copy kept to read the pattern
	// back is itself a keepalive, so with allocate() this test could not tell a block
	// that co-owns the buffer from one that holds a bare span: the storage would be
	// alive either way and the pattern would read back either way. The weak_ptr
	// separates reading the bytes from owning them, so the survival claim is asserted
	// rather than assumed.
	//
	// WHAT THIS TEST CANNOT SEPARATE, stated so phases 7b and 7c do not assume it covers
	// them: the block's co-ownership and the backend's own are satisfied by the SAME
	// io_buffer, because manual_io_backend copies the request the block hands it. The
	// mutation that proves this test bites -- the block holding a bare span -- therefore
	// strips both at once. A real backend that does not retain the io_buffer across its
	// completion would leave the block's half of the property unverified by this case, so
	// each backend owes a test of its own that the storage outlives its late write.
	constexpr std::size_t                       size    = 32;
	auto                                        storage = std::make_shared<std::vector<std::byte>>(size, std::byte{0});
	const std::weak_ptr<std::vector<std::byte>> watch   = storage;
	alt::io_buffer                              buffer  = alt::io_buffer::borrow(std::span<std::byte>{*storage}, storage);

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);

	// Every owner the test and the frame had is dropped here. The frame's own copy was
	// moved into the operation block when the awaiter was built.
	storage.reset();
	buffer = alt::io_buffer{};
	ASSERT_FALSE(watch.expired());

	EXPECT_TRUE(chain.request_stop());
	EXPECT_EQ(home.drain(), 1U);
	ASSERT_EQ(alt_test::live_frames, 0);

	// The chain and its frame are gone. The storage is not, because the operation block
	// co-owns the io_buffer.
	ASSERT_FALSE(watch.expired());
	const std::shared_ptr<std::vector<std::byte>> survivor = watch.lock();
	ASSERT_NE(survivor, nullptr);

	// The backend's late write, after the teardown, through the buffer it was handed.
	const std::span<std::byte> bytes = backend.request(0).buffer.bytes();
	ASSERT_EQ(bytes.size(), size);
	std::fill(bytes.begin(), bytes.end(), std::byte{0xA5});

	// The assertion is on the bytes, read through an owner that is not the backend's.
	EXPECT_EQ(std::count(survivor->begin(), survivor->end(), std::byte{0xA5}), static_cast<std::ptrdiff_t>(size));

	backend.complete(0, alt::io_completion{.bytes = size, .error = {}});
	EXPECT_EQ(home.drain(), 0U);
	EXPECT_FALSE(record->body_ran);
}

TEST(IoOperationTest, TheDescriptorSurvivesTheChainsTeardown)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	std::shared_ptr<io_handle>     file  = io_handle::adopt(alt::invalid_native_handle);
	const std::weak_ptr<io_handle> watch = file;

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);

	// The reference an async_file would have held is dropped here, as a cancelled
	// frame's would be. The frame's own copy was moved into the operation block.
	file.reset();
	ASSERT_FALSE(watch.expired());

	EXPECT_TRUE(chain.request_stop());
	EXPECT_EQ(home.drain(), 1U);
	ASSERT_EQ(alt_test::live_frames, 0);

	{
		// Still the same live object, not merely a non-null pointer.
		const std::shared_ptr<io_handle> survivor = watch.lock();
		ASSERT_NE(survivor, nullptr);
		EXPECT_EQ(survivor->native(), alt::invalid_native_handle);
		EXPECT_FALSE(survivor->close_requested());
		survivor->request_close();
		EXPECT_TRUE(survivor->close_requested());
	}

	// The operation block is now the only owner, which is what keeps a backend from
	// reading through a closed -- or recycled -- descriptor number.
	ASSERT_FALSE(watch.expired());

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});

	// The block dropped the last reference once the completion found it detached, so
	// the close happens on the operation's side rather than on the cancelled frame's.
	EXPECT_TRUE(watch.expired());
	EXPECT_EQ(home.drain(), 0U);
	EXPECT_FALSE(record->body_ran);
}

TEST(IoOperationTest, ThePermitIsReleasedOnAHomeThreadSoTheNextOperationSubmitsThere)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend inner;
	recording_backend      backend{inner};
	alt::manual_scheduler  home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home}, 1);
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> first  = std::make_shared<op_record>();
	const std::shared_ptr<op_record> second = std::make_shared<op_record>();

	const alt::cancellation_handle chain_a = alt::start(awaiting_chain(shared, read_request(buffer, file), file, first));
	alt::cancellation_handle       chain_b = alt::start(awaiting_chain(shared, read_request(buffer, file), file, second));

	ASSERT_TRUE(first->slot_acquired);
	ASSERT_FALSE(second->slot_acquired);
	ASSERT_EQ(inner.pending(), 1U);

	const std::thread::id test_thread      = std::this_thread::get_id();
	const std::thread::id completer_thread = complete_on_another_thread(inner, 0, alt::io_completion{.bytes = 16, .error = {}});

	ASSERT_NE(completer_thread, test_thread);
	ASSERT_EQ(inner.pending(), 0U);
	ASSERT_FALSE(second->slot_acquired);

	// The permit is released inside the posted resumption, so the parked chain is woken
	// here and submits from here.
	EXPECT_EQ(home.drain(), 1U);
	ASSERT_TRUE(second->slot_acquired);
	ASSERT_EQ(inner.pending(), 1U);

	const std::vector<std::thread::id> threads = backend.threads();
	ASSERT_EQ(threads.size(), 2U);
	EXPECT_EQ(threads[0], test_thread);
	EXPECT_EQ(threads[1], test_thread);
	EXPECT_NE(threads[1], completer_thread);

	inner.complete(1, alt::io_completion{.bytes = 16, .error = {}});
	EXPECT_EQ(home.drain(), 1U);
	EXPECT_TRUE(first->body_ran);
	EXPECT_TRUE(second->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(chain_a.stop_requested());
	EXPECT_FALSE(chain_b.stop_requested());
}

TEST(IoOperationTest, AThrowingHomePostLeavesTheChainLeakedRatherThanResumedElsewhere)
{
	alt_test::reset_coroutine_test_state();

	alt::manual_io_backend backend;
	throwing_scheduler     home;
	const auto             shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto             file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer   buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(awaiting_chain(shared, read_request(buffer, file), file, record));

	ASSERT_EQ(backend.pending(), 1U);
	ASSERT_EQ(alt_test::live_frames, 1);

	// The completion has nowhere to post. Leaking a suspended frame beats running its
	// body on an arbitrary thread, so the chain is left where it is.
	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});

	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 1);
	EXPECT_EQ(backend.pending(), 0U);
	EXPECT_FALSE(chain.stop_requested());

	// Reclaimed by hand, because nothing else can: the resumption that would have freed
	// it is the one that could not be posted. Without this the suite's own
	// AddressSanitizer run would report the leak this test asserts.
	ASSERT_TRUE(record->self);
	record->self.destroy();
	EXPECT_EQ(alt_test::live_frames, 0);
}

TEST(IoOperationTest, ATeardownWithASecondCallbackOnTheChainsTokenIsSafe)
{
	alt_test::reset_coroutine_test_state();

	// CLAUDE.md records that libstdc++'s stop_source::request_stop() skips relocking the
	// callback list after the LAST callback, which kept a use-after-free in
	// detail::stop_forwarder invisible on every Linux preset, AddressSanitizer included.
	// This awaiter registers a std::stop_callback on the chain's token and tears the
	// chain down from it, so it sits in that blind spot, and the counter-measure is the
	// one CancellationRefTest.ATeardownWithASecondCallbackOnTheChainsTokenIsSafe uses: a
	// SECOND registration on the same token, so the walk still has somewhere to go after
	// the abandoning callback has freed the frames that own the chain's stop state.
	//
	// Three things make the shape reach the hazard, and all three are deliberate:
	//
	//  - the stop arrives through start_options::token and the chain's own
	//    cancellation_handle is discarded, so every owner of the chain's stop state
	//    lives in the frames the teardown destroys;
	//  - the completion scheduler is a trampoline_scheduler, which runs a posted item
	//    where it is posted, so the teardown runs inside the stop walk rather than on a
	//    later drain;
	//  - the second callback is registered first, which puts the awaiter's own at the
	//    head of the list and therefore first in the walk.
	alt::manual_io_backend    backend;
	alt::trampoline_scheduler home;
	const auto                shared = make_shared_state(backend, alt::scheduler_ref{home});
	const auto                file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer      buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	std::stop_source                 source;

	static_cast<void>(alt::start(chain_with_a_second_stop_callback(shared, read_request(buffer, file), file, record), {.token = source.get_token()}));

	ASSERT_TRUE(record->slot_acquired);
	ASSERT_EQ(backend.pending(), 1U);
	ASSERT_EQ(alt_test::live_frames, 1);

	EXPECT_TRUE(source.request_stop());

	// The whole teardown ran inside request_stop(), on this thread.
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::take_destruction_order(), std::vector<int>{1});
	EXPECT_TRUE(backend.was_cancelled(0));

	backend.complete(0, alt::io_completion{.bytes = 16, .error = {}});
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(backend.pending(), 0U);
	EXPECT_TRUE(source.stop_requested());
	EXPECT_FALSE(source.request_stop());
}

TEST(IoOperationTest, AThrowingSubmitPropagatesOutOfTheAwaitAndHandsBackThePermit)
{
	alt_test::reset_coroutine_test_state();

	// A throwing submit is outside the seam's two specified outcomes, so the awaiter
	// treats it as a submission that never happened: no callback is coming, the block's
	// own reference for the backend is dropped, the permit goes back, and the exception
	// travels out of await_suspend into the awaiting body.
	throwing_backend      backend{nullptr};
	alt::manual_scheduler home;
	const auto            shared = make_shared_state(backend, alt::scheduler_ref{home}, 1);
	const auto            file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer  buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();
	alt::cancellation_handle         chain  = alt::start(chain_catching_the_await(shared, read_request(buffer, file), file, record));

	ASSERT_TRUE(record->slot_acquired);
	EXPECT_EQ(backend.submissions(), 1);
	EXPECT_TRUE(record->threw);
	EXPECT_FALSE(record->body_ran);

	// Nothing was posted, the backend count is back to zero, and the permit is available
	// again rather than lost with the unwound frame.
	EXPECT_EQ(home.drain(), 0U);
	EXPECT_EQ(shared->at_backend.load(), 0U);
	EXPECT_EQ(shared->slots.available(), 1);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(chain.stop_requested());
}

TEST(IoOperationTest, AThrowingSubmitOnADetachedBlockPostsTheTeardownAndSwallowsTheThrow)
{
	alt_test::reset_coroutine_test_state();

	// The other arm of the same path, and the most intricate branch in this component: the
	// chain is stopped from inside submit(), while the awaiter's gate is still up, and
	// submit() then throws. A detached chain must not resume, with or without an exception,
	// so the throw dies with the block and the teardown is posted instead.
	std::stop_source source;
	throwing_backend backend{stop_from_inside_submit(source)};

	alt::manual_scheduler home;
	const auto            shared = make_shared_state(backend, alt::scheduler_ref{home}, 1);
	const auto            file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer  buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> record = std::make_shared<op_record>();

	static_cast<void>(alt::start(chain_catching_the_await(shared, read_request(buffer, file), file, record), {.token = source.get_token()}));

	ASSERT_TRUE(record->slot_acquired);
	EXPECT_EQ(backend.submissions(), 1);

	// Swallowed, not propagated: the awaiting body saw neither the exception nor another
	// statement of its own.
	EXPECT_FALSE(record->threw);
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::live_frames, 1);

	EXPECT_EQ(home.drain(), 1U);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_FALSE(record->threw);
	EXPECT_FALSE(record->body_ran);
	EXPECT_EQ(alt_test::take_destruction_order(), std::vector<int>{1});

	// The permit came back through the resumption's own release, and nothing is left at
	// the backend.
	EXPECT_EQ(shared->slots.available(), 1);
	EXPECT_EQ(shared->at_backend.load(), 0U);
}

TEST(IoOperationTest, AChainStoppedBeforeTheAwaitNeverReachesTheBackend)
{
	alt_test::reset_coroutine_test_state();

	// What this pins, and what it does not, because the distinction is the whole value of
	// the case. A chain whose stop is already requested never reaches the backend. The
	// layer that refuses it is alt's own, one above this component:
	// detail::cancellation_checked::await_ready returns false for a stopped chain and its
	// await_suspend abandons WITHOUT ENTERING THE INNER AWAITER AT ALL, so io_awaiter::start
	// never runs and no io_operation block is ever submitted. The property is worth a
	// regression test here because this component is the first in-library consumer of that
	// layer and silently losing the pre-check would turn every stopped chain into a
	// submission.
	//
	// It is NOT a test of start()'s own already-detached check, and no test in this file is.
	// That check fires only for a stop that lands between cancellation_checked's pre-check
	// and the std::stop_callback registration a few statements later, and nothing
	// single-threaded can land there: the only code between those two points is the
	// awaiter's own prologue, which calls out to nothing, so a stop would have to arrive
	// from another thread inside that window. The check is therefore guarded but
	// unexercised, and it stays because the window exists rather than because anything here
	// reaches it. Same for the `starting` gate's stop-side half, which that window is also
	// the whole reason for.
	//
	// The throwing backend is used only because it is the one backend here whose submit()
	// can arm the stop from inside itself; the assertion is about the SECOND operation,
	// which never reaches submit() and so never throws.
	std::stop_source source;
	throwing_backend backend{stop_from_inside_submit(source)};

	alt::manual_scheduler home;
	const auto            shared = make_shared_state(backend, alt::scheduler_ref{home}, 2);
	const auto            file   = io_handle::adopt(alt::invalid_native_handle);
	const alt::io_buffer  buffer = alt::io_buffer::allocate(16).value();

	const std::shared_ptr<op_record> first  = std::make_shared<op_record>();
	const std::shared_ptr<op_record> second = std::make_shared<op_record>();

	// The first chain arms the stop from inside submit() and is detached by it.
	static_cast<void>(alt::start(chain_catching_the_await(shared, read_request(buffer, file), file, first), {.token = source.get_token()}));
	ASSERT_EQ(backend.submissions(), 1);
	ASSERT_TRUE(source.stop_requested());

	// The second starts on a token that is already stopped.
	static_cast<void>(alt::start(chain_catching_the_await(shared, read_request(buffer, file), file, second), {.token = source.get_token()}));

	// Still 1. The second chain was torn down at its first suspension point, so it never
	// took a permit either.
	EXPECT_EQ(backend.submissions(), 1);
	EXPECT_EQ(backend.cancels(), 0);
	EXPECT_FALSE(second->slot_acquired);
	EXPECT_FALSE(second->body_ran);
	EXPECT_FALSE(second->threw);

	EXPECT_EQ(home.drain(), 1U);
	EXPECT_FALSE(first->body_ran);
	EXPECT_EQ(alt_test::live_frames, 0);
	EXPECT_EQ(shared->slots.available(), 2);
	EXPECT_EQ(shared->at_backend.load(), 0U);
}

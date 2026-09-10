#pragma once

#include <alt/detail/cancellation.hpp>
#include <alt/scheduler.hpp>
#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace alt
{

	/**
	 * @brief Where an alt::cancellable_task sends an exception that escaped its body.
	 *
	 * A function pointer plus an opaque state pointer, erased without allocating,
	 * in the same shape as alt::work_item. An empty handler means std::terminate.
	 */
	struct exception_handler
	{
		void (*invoke)(std::exception_ptr error, void* state) = nullptr;
		void* state                                           = nullptr;
	};

	/**
	 * @brief An owner's handle to a running alt::cancellable_task.
	 *
	 * Safe to use for as long as the handle exists, including after the task has
	 * finished or destroyed itself, because the underlying stop state is reference
	 * counted by the standard library rather than living in the coroutine frame.
	 *
	 * Copyable, so a window can keep one and hand copies elsewhere.
	 */
	class cancellation_handle
	{
	public:
		/** Constructs an empty handle that refers to no task. */
		cancellation_handle() noexcept = default;

		/** Constructs a handle over an existing stop source. */
		explicit cancellation_handle(std::stop_source source) noexcept: m_source(std::move(source))
		{}

		/**
		 * @brief Asks the task to stop.
		 *
		 * Returns immediately. The task stops at its next suspension point, so
		 * nothing about the task's frames is guaranteed to have happened yet when
		 * this returns. Returns true when this call was the one that requested it.
		 */
		bool request_stop() noexcept
		{
			return m_source.request_stop();
		}

		/** True once a stop has been requested, by this handle or by a linked token. */
		[[nodiscard]] bool stop_requested() const noexcept
		{
			return m_source.stop_requested();
		}

		/** A token for the same stop state, for onward propagation. */
		[[nodiscard]] std::stop_token get_token() const noexcept
		{
			return m_source.get_token();
		}

		/** A copy of the stop source, for an owner that wants to share the signal. */
		[[nodiscard]] std::stop_source get_source() const noexcept
		{
			return m_source;
		}

	private:
		std::stop_source m_source{std::nostopstate};
	};

	/** @brief Configuration for alt::start. Every field is optional. */
	struct start_options
	{
		/**
		 * An external cancellation signal to link in, so that a whole window's
		 * worth of tasks can be cancelled from one source. Empty means the task
		 * relies solely on the handle alt::start returns.
		 */
		// NOLINTNEXTLINE(readability-redundant-member-init): the braces are load-bearing here, not redundant. start_options is aggregate-initialized with designated initializers at call sites (e.g. {.token = ...}), and without this default member initializer GCC's -Wmissing-field-initializers fires on every partial designated-init because std::stop_token would then have no in-class default to fall back on.
		std::stop_token token{};

		/**
		 * Where to destroy the task's frames once cancellation is observed. Empty
		 * means destroy them in place, on whichever thread observed it. Supply a
		 * scheduler when the frames hold state that must not be destroyed off its
		 * own thread, such as anything owned by a UI toolkit.
		 */
		// NOLINTNEXTLINE(readability-redundant-member-init): same reasoning as start_options::token above; required for -Wmissing-field-initializers under designated-initializer construction.
		scheduler_ref home{};

		/** Where an escaped exception goes. Empty means std::terminate. */
		exception_handler on_exception{};
	};

	class cancellable_task;

	namespace detail
	{

		/** Forwards an external stop request into the task's own source. */
		struct stop_forwarder
		{
			std::stop_source* target;

			void operator()() const noexcept
			{
				target->request_stop();
			}
		};

		class cancellable_task_promise
		{
		public:
			alt::cancellable_task get_return_object() noexcept;

			// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the compiler calls promise.initial_suspend() through a promise instance for every coroutine using this promise_type; making it static would trip readability-static-accessed-through-instance at each of those call sites instead.
			std::suspend_always initial_suspend() noexcept
			{
				return {};
			}

			/** Self-owning: the frame is destroyed once the body finishes. */
			struct final_awaiter
			{
				// NOLINTNEXTLINE(readability-convert-member-functions-to-static): await_ready is called on a temporary the compiler constructs directly in the coroutine body (co_await final_suspend()); static here would move the finding to every coroutine using this promise_type instead of fixing it.
				[[nodiscard]] bool await_ready() const noexcept
				{
					return false;
				}

				// NOLINTNEXTLINE(readability-convert-member-functions-to-static): same reasoning as await_ready above.
				void await_suspend(std::coroutine_handle<cancellable_task_promise> h) const noexcept
				{
					h.destroy();
				}

				void await_resume() const noexcept
				{}
			};

			// NOLINTNEXTLINE(readability-convert-member-functions-to-static): same reasoning as initial_suspend above; the compiler calls promise.final_suspend() through a promise instance.
			final_awaiter final_suspend() noexcept
			{
				return {};
			}

			void return_void() noexcept
			{}

			/**
			 * A root has nobody to propagate to, so an escaped exception goes to the
			 * configured handler, or terminates the process. Marked noexcept, so a
			 * handler that throws terminates too.
			 */
			void unhandled_exception() const noexcept
			{
				if(m_handler.invoke == nullptr)
				{
					std::terminate();
				}
				m_handler.invoke(std::current_exception(), m_handler.state);
			}

			/** Injects the cancellation check ahead of every suspension point. */
			template<typename A>
			auto await_transform(A&& a)
			{
				return make_cancellation_checked(std::forward<A>(a), &m_context);
			}

			/** Lets an awaited alt::task relay this context to its own children. */
			[[nodiscard]] cancellation_context* cancellation_context_pointer() noexcept
			{
				return &m_context;
			}

			/** Installs the root handle and the options. Must run before the body starts. */
			void configure(std::coroutine_handle<> root, const alt::start_options& options)
			{
				m_context.root  = root;
				m_context.home  = options.home;
				m_context.token = m_source.get_token();
				m_handler       = options.on_exception;

				// The external token is linked rather than adopted, so that the handle
				// returned by alt::start keeps working in every case. Emplacing this
				// callback over an already-stopped token invokes it at once.
				if(options.token.stop_possible())
				{
					m_external.emplace(options.token, stop_forwarder{&m_source});
				}
			}

			[[nodiscard]] alt::cancellation_handle make_handle() const
			{
				return alt::cancellation_handle{m_source};
			}

		private:
			cancellation_context                              m_context{};
			std::stop_source                                  m_source;
			exception_handler                                 m_handler{};
			std::optional<std::stop_callback<stop_forwarder>> m_external;
		};

	}

	/**
	 * @brief A self-owning root coroutine that destroys itself when cancelled.
	 *
	 * Lazy, like alt::task: the body does not run until alt::start is called, and
	 * from that point the coroutine owns its own frame. It returns void and is not
	 * awaitable, because a root has nobody to return a value to.
	 *
	 * Once a stop has been requested, the coroutine destroys its entire await
	 * chain at the next suspension point any frame in that chain reaches. No
	 * statement of any coroutine body runs after that, so a window that has closed
	 * cannot be touched. Only destructors of live locals and of coroutine
	 * parameters run, and by default they run on the thread that observed the
	 * cancellation. Pass a home scheduler in alt::start_options to move that
	 * teardown onto a thread of your choosing.
	 *
	 * A coroutine whose body reaches no further suspension point runs to
	 * completion regardless. Insert co_await alt::cancellation_point{} into a long
	 * computation to give it somewhere to stop.
	 */
	class [[nodiscard]] cancellable_task
	{
	public:
		using promise_type = detail::cancellable_task_promise;

		/** Adopts a coroutine frame. Called by the promise, not by users. */
		explicit cancellable_task(std::coroutine_handle<promise_type> handle) noexcept: m_handle(handle)
		{}

		cancellable_task(cancellable_task&& other) noexcept: m_handle(std::exchange(other.m_handle, {}))
		{}

		cancellable_task& operator=(cancellable_task&&)      = delete;
		cancellable_task(const cancellable_task&)            = delete;
		cancellable_task& operator=(const cancellable_task&) = delete;

		/** Destroys the frame if the task was never started. */
		~cancellable_task()
		{
			if(m_handle)
			{
				m_handle.destroy();
			}
		}

	private:
		friend cancellation_handle start(cancellable_task t, const start_options& options);

		std::coroutine_handle<promise_type> m_handle;
	};

	/**
	 * @brief Starts a cancellable task and returns the owner's handle to it.
	 *
	 * The task owns its own frame from here on, so the returned handle is the only
	 * thing the caller needs to keep, and it is safe to keep for as long as the
	 * caller likes.
	 */
	[[nodiscard]] inline cancellation_handle start(cancellable_task t, const start_options& options = {})
	{
		const auto handle = std::exchange(t.m_handle, {});
		assert(handle && "starting an empty alt::cancellable_task");

		handle.promise().configure(handle, options);

		// The handle must be built before the resume. A body that completes without
		// suspending destroys its own frame inside resume(), and reading the promise
		// afterwards would be a use-after-free.
		cancellation_handle result = handle.promise().make_handle();

		handle.resume();
		return result;
	}

	/**
	 * @brief A suspension point for a computation that has none of its own.
	 *
	 * Awaiting this inside an alt::cancellable_task or alt::task costs one atomic
	 * load and never suspends unless cancellation has been requested, in which
	 * case the chain is torn down there. Awaited in any other coroutine it is an
	 * inert no-op, so a helper carrying one is safe to call from anywhere.
	 */
	struct cancellation_point
	{
		[[nodiscard]] static bool await_ready() noexcept
		{
			return true;
		}

		static void await_suspend(std::coroutine_handle<> /*h*/) noexcept
		{}

		static void await_resume() noexcept
		{}
	};

	namespace detail
	{

		inline alt::cancellable_task cancellable_task_promise::get_return_object() noexcept
		{
			return alt::cancellable_task{std::coroutine_handle<cancellable_task_promise>::from_promise(*this)};
		}

	}

}

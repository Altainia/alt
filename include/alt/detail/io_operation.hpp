#pragma once

#include <alt/cancellation_ref.hpp>
#include <alt/detail/io_handle.hpp>
#include <alt/detail/io_shared.hpp>
#include <alt/io_backend.hpp>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

namespace alt::detail
{

	/**
	 * @brief One in-flight file I/O operation. Refcounted, and NEVER a coroutine-frame local.
	 *
	 * This is where this component's driving constraint is answered, so it is worth stating
	 * the constraint before the members.
	 *
	 * `alt`'s cancellation tears a chain down BY DESTROYING ITS FRAMES:
	 * alt::cancellation_ref::abandon() destroys the root frame and every frame below it.
	 * io_uring and IOCP both deliver a completion AFTER a cancel request rather than at it,
	 * and a thread-pool job that has already issued its `pread` runs to the end. So a
	 * cancelled read can still be in the kernel's hands when the frame that asked for it is
	 * gone. Operation state therefore cannot live in the coroutine frame, and this block is
	 * where it lives instead: a std::shared_ptr that the backend's submission keeps alive,
	 * co-owning the three things a backend might still touch.
	 *
	 * **Three things are co-owned, not one.** The buffer is the obvious one; the other two
	 * are the same bug class.
	 *
	 *  - The BUFFER (`request.buffer`), because the kernel may still be writing into it.
	 *  - The DESCRIPTOR (`file`), because an alt::async_file dying with the cancelled frame
	 *    would close it, and a closed descriptor NUMBER can be reassigned to another file
	 *    before the backend's read issues. The read then succeeds against someone else's
	 *    data, which is worse than the benign EBADF a merely closed descriptor gives.
	 *  - The SERVICE's shared state (`owner`), because the resumption releases a
	 *    backpressure permit that lives there, and shutdown deliberately allows a
	 *    resumption to still be queued when alt::file_io is gone.
	 *
	 * **Three phases, and which party may leave `waiting` is the whole protocol.**
	 *
	 *  - `waiting` -> `done` is the BACKEND's, when the operation really completed. It
	 *    fills `result` and posts the resumption.
	 *  - `waiting` -> `detached` is the STOP CALLBACK's, when the chain was stopped. It
	 *    posts the teardown, and the resumption there takes resume_or_abandon's abandon
	 *    branch.
	 *  - Whichever party loses the race finds a state other than `waiting`, UNDER THE
	 *    MUTEX, and does NOTHING AT ALL. It does not touch `handle`, `chain` or the frame.
	 *    That rule is the only thing standing between a late completion and a frame that
	 *    no longer exists, so it is not a fast path to be optimized away.
	 *
	 * **The posted resumption does exactly three things, in this order:** read what it
	 * needs out of the block under the mutex; release the permit; call
	 * resume_or_abandon LAST, because either branch may free the frame. See resume().
	 *
	 * The shape is alt's generalization of cpwf's hand-written `Handoff`, which solved the
	 * same problem against alt::cancellation_ref from outside the library: a shared_ptr
	 * control block, a three-state phase machine, a std::stop_callback on the chain's
	 * token, and resume_or_abandon as the last touch. The buffer, the descriptor, the
	 * backend token and the `starting` gate are what this adds.
	 *
	 * Thread safety: `mutex` guards every mutable member but `sentinel`. `owner`, `file`
	 * and the request's shape are fixed once the awaiter has built the block and are read
	 * without the lock on that basis. The data members are public because this is an
	 * engine-internal type driven entirely by alt::detail::io_awaiter and the two static
	 * callbacks below; nothing outside this header and src/io_operation.cpp touches them.
	 */
	struct io_operation
	{
		/** @brief Which party, if any, has already taken the operation out of `waiting`. */
		enum class phase : std::uint8_t
		{
			/** Nobody has. The backend may still complete and the chain may still stop. */
			waiting,
			/** The backend completed it. `result` is filled and the body may run. */
			done,
			/** The chain was stopped. The frame is to be abandoned, not resumed. */
			detached,
		};

		/** @brief What `sentinel` holds for the whole of a block's lifetime. */
		static constexpr std::uint32_t alive_pattern = 0x494F4F50U;

		/** @brief What ~io_operation writes over it, before anything is torn down. */
		static constexpr std::uint32_t dead_pattern = 0xDEAD104FU;

		/**
		 * @brief Builds a block for one operation.
		 *
		 * @param service     The service state the operation co-owns. Must not be null.
		 * @param description The operation, held here rather than in the frame. Its
		 *                    `buffer` is the co-owned buffer, and its `in`/`out` must point
		 *                    into this block or at storage that outlives it -- never into a
		 *                    coroutine frame, which a late completion can outlive.
		 * @param descriptor  The descriptor the operation co-owns, or null for an operation
		 *                    that names no handle (a path operation, or an escape-hatch
		 *                    call).
		 */
		io_operation(std::shared_ptr<io_shared> service, io_request description, std::shared_ptr<io_handle> descriptor) noexcept;

		/** @brief Stores dead_pattern before anything else is torn down. See `sentinel`. */
		~io_operation();

		io_operation(const io_operation&)            = delete;
		io_operation& operator=(const io_operation&) = delete;
		io_operation(io_operation&&)                 = delete;
		io_operation& operator=(io_operation&&)      = delete;

		/**
		 * @brief The std::stop_callback body the awaiter registers on the chain's token.
		 *
		 * Detaches the chain from the operation: it does not stop the operation, which is
		 * not portably possible, and it does not tear the chain down where it runs either.
		 * It moves the block to `detached` under the mutex, asks the backend to stop early
		 * as a hint, and posts the teardown to the completion scheduler.
		 *
		 * Holds a counted reference rather than a pointer, because the callback can be
		 * destroyed from inside its own invocation: on a completion scheduler that runs a
		 * posted item where it is posted, the teardown this posts destroys the awaiting
		 * frame, and the awaiter's std::stop_callback -- this object's home -- with it.
		 * operator() therefore copies the reference to its own stack before doing anything.
		 */
		struct stop_handler
		{
			/** The block. Counted, for the reason the class note gives. */
			std::shared_ptr<io_operation> block;

			/** @brief Detaches the chain. Safe to run on any thread, and reentrantly. */
			void operator()() const noexcept;
		};

		/**
		 * @brief The backend's completion callback. RUNS ON AN I/O THREAD.
		 *
		 * Matches alt::io_completion_fn, and may be called from inside
		 * alt::io_backend::submit before it returns. It never resumes anything itself: it
		 * records the result and posts, so that the awaiting body runs on the completion
		 * scheduler rather than wherever the kernel finished.
		 *
		 * @param state  The block, as the void* the seam carries.
		 * @param result What the operation produced.
		 */
		static void on_backend_complete(void* state, io_completion result) noexcept;

		/**
		 * @brief The posted resumption. Runs on the completion scheduler.
		 *
		 * DELICATE, and the order of its three steps is the design rather than a detail:
		 *
		 *  1. read `keep`, `handle` and `chain` out of the block under the mutex;
		 *  2. release the backpressure permit -- which may hand it straight to a parked
		 *     waiter and resume it INLINE, so that the next operation submits from THIS
		 *     thread. That is the point: every submission then happens on a thread the
		 *     completion scheduler runs on, never on an I/O thread;
		 *  3. alt::cancellation_ref::resume_or_abandon, LAST, because both of its branches
		 *     may free the frame.
		 *
		 * Nothing may be read through the block, the awaiter or the chain after step 3.
		 *
		 * @param state The block, as the void* an alt::work_item carries.
		 */
		static void resume(void* state) noexcept;

		/**
		 * @brief Posts resume() to the completion scheduler. Called WITHOUT `mutex` held.
		 *
		 * The caller has already moved the block out of `waiting` and parked a counted
		 * self-reference in `keep`, under the mutex; this is the part that must happen
		 * outside it, because a completion scheduler is allowed to run a posted item on the
		 * posting thread and the resumption takes the same mutex.
		 *
		 * A THROWING POST LEAVES THE CHAIN DELIBERATELY LEAKED. There is nowhere to run the
		 * resumption, and running a coroutine body on an arbitrary thread is worse than
		 * leaking a suspended frame, so `keep` is dropped and the frame stays where it is.
		 * cpwf's Handoff::post_home_locked settles it the same way.
		 */
		void post_resumption() noexcept;

		/**
		 * @brief Whether this block is still within its own lifetime.
		 *
		 * Tests POSITIVELY for alive_pattern rather than negatively for dead_pattern, which
		 * is the whole value of the check: freed storage that has been handed out again and
		 * written over holds neither pattern, so a test for dead_pattern would call such a
		 * block alive and let the lock through. Only the positive test rejects it, and
		 * while four bytes can be overwritten with alive_pattern by chance it rejects all
		 * but one value in 2^32.
		 *
		 * Relaxed deliberately: this orders nothing, it reads a word to decide whether an
		 * object exists, and if the answer is no there is no happens-before edge left to
		 * establish with anything.
		 */
		[[nodiscard]] bool alive() const noexcept;

		/**
		 * @brief The liveness sentinel. See alive(), and read this before changing it.
		 *
		 * A use-after-free whose only contact with the freed object is a MUTEX LOCK is
		 * invisible to AddressSanitizer AND ThreadSanitizer: ASan does not intercept
		 * glibc's pthread_mutex_lock, libstdc++'s std::mutex destructor is trivial so no
		 * pthread_mutex_destroy is intercepted either, and TSan drops its synchronization
		 * variables on free. Every teardown race in this block is behind its mutex, so they
		 * sit squarely in that hole, and a clean sanitizer run is not sufficient evidence
		 * here. This follows alt::detail::waiter_list's precedent exactly, including the two
		 * decisions a reader would otherwise re-derive:
		 *
		 *  - The MEMBER EXISTS IN EVERY CONFIGURATION. Making it conditional on NDEBUG would
		 *    change this type's layout between translation units compiled with different
		 *    flags, an ODR violation in a library that ships headers and a .deb. The cost of
		 *    not doing that is four bytes.
		 *  - ONLY THE CHECKS ARE assert-GUARDED, and are therefore ABSENT UNDER NDEBUG. That
		 *    leaves a use-after-free reachable only in an optimized build as invisible as it
		 *    is today. That is a stated residual gap, not an oversight.
		 *
		 * Declared first so it is destroyed last, after `mutex` -- the member a late caller
		 * would otherwise lock. That buys nothing on its own, std::atomic's destructor being
		 * trivial, and the position is kept for the reader who expects a guard to precede
		 * what it guards. What makes the sentinel work is the destructor's store, which runs
		 * before any member is destroyed.
		 */
		std::atomic<std::uint32_t> sentinel{alive_pattern};

		/** Guards every mutable member below. Never held across a post or a resumption. */
		std::mutex mutex;

		/** Which party has taken the operation out of `waiting`. See the class note. */
		phase state = phase::waiting;

		/**
		 * True while io_awaiter::await_suspend is still starting the operation.
		 *
		 * DELICATE. This is what keeps the awaiting frame from being destroyed while
		 * await_suspend is on the stack. A party that moves the block out of `waiting`
		 * while this is set records the transition and posts NOTHING; the awaiter posts for
		 * it, as the last thing it does, once it is no longer reading its own frame. Both
		 * parties can reach that window: the stop callback fires inline from its own
		 * registration when the token is already stopped, and a backend completion fires
		 * inline inside submit().
		 *
		 * Without the gate, a completion scheduler that runs a posted item on the posting
		 * thread would run the teardown -- and so free the frame and this awaiter -- from
		 * inside the registration or inside submit(), and await_suspend would carry on
		 * writing to freed storage. It is not a fourth phase: who may leave `waiting` is
		 * unchanged, and so is the order of the resumption's three steps.
		 */
		bool starting = true;

		/** The suspended frame. Written once by await_suspend, before anything can see it. */
		std::coroutine_handle<> handle;

		/** The chain to resume or abandon. Non-empty only for a chain that relays cancellation. */
		cancellation_ref chain;

		/** The service state. Co-owned; see the class note. Null never. */
		std::shared_ptr<io_shared> owner;

		/** The descriptor. Co-owned; see the class note. Null for a handle-less operation. */
		std::shared_ptr<io_handle> file;

		/**
		 * The operation itself, including the co-owned io_buffer.
		 *
		 * Held here rather than in the frame, which is what makes io_request's own
		 * precondition satisfiable: `in` and `out` must point into the operation block,
		 * because a completion can land after the frame is gone and a write through `out`
		 * into a destroyed frame is a use-after-free no sanitizer reliably reports.
		 */
		io_request request;

		/** What the backend gave for this submission, for the cancel hint. Empty until submitted. */
		io_token token;

		/** What the operation produced. Read by await_resume on the resumed path only. */
		io_completion result;

		/**
		 * The reference the backend's `void* state` stands for.
		 *
		 * DELICATE. alt::io_backend::submit carries an opaque pointer, not a counted
		 * handle, so nothing on the backend's side keeps this block alive. This
		 * self-reference is taken before the submission and released by the completion, and
		 * it is the whole reason the block -- and the buffer and descriptor it co-owns --
		 * survives the cancelled frame's destruction.
		 *
		 * The consequence is stated rather than discovered: a backend destroyed with
		 * operations still pending never runs their callbacks, so their blocks are never
		 * released and leak. alt::manual_io_backend's own documentation says a test that
		 * cares completes everything first; this is why it matters.
		 */
		std::shared_ptr<io_operation> backend_ref;

		/**
		 * The reference the posted resumption stands for.
		 *
		 * Parked here by whoever posts and taken back out by resume(), so the block cannot
		 * die between the post and the run. cpwf's Handoff::keep is the precedent.
		 */
		std::shared_ptr<io_operation> keep;
	};

	/**
	 * @brief Awaits one file I/O operation, allocating its block and submitting it.
	 *
	 * The second half of an operation. The first half is the backpressure permit, which the
	 * awaiting coroutine takes itself with `co_await owner->acquire_slot()` -- a hand-written
	 * awaiter cannot co_await anything, which is why the two phases are two awaits rather
	 * than one awaiter with a state machine of its own.
	 *
	 * THIS AWAITER TAKES THE PERMIT OVER. It releases it exactly once, on a thread the
	 * completion scheduler runs on, inside the posted resumption and before the frame is
	 * resumed or abandoned. A caller must therefore hold exactly one permit when it awaits
	 * this and must not release that permit itself. The one case where the permit is not
	 * released is the deliberately leaked chain of a throwing post, which cannot be
	 * otherwise: the thread that would have released it is the one there is nowhere to post
	 * to.
	 *
	 * Why that release happens there and not in the backend's callback is forced rather
	 * than chosen. alt::async_semaphore::release() resumes a parked waiter INLINE on the
	 * releasing thread, so releasing from an I/O thread would resume the next operation's
	 * coroutine there and have it submit from there. Releasing inside the posted resumption
	 * instead buys the invariant this component's top anti-goal needs: EVERY SUBMISSION
	 * HAPPENS ON A THREAD THE COMPLETION SCHEDULER RUNS ON, NEVER ON AN I/O THREAD. It also
	 * makes the backend's reentrancy requirements trivial, since it is never reentered from
	 * its own completion thread. The consequence, stated rather than discovered: slot
	 * handoff is serialized through the completion scheduler, so a loop that stops pumping
	 * stops issuing new I/O. For a backpressure limit that is the right failure mode.
	 *
	 * Preconditions, all three of which the layer above keeps:
	 *
	 *  - the caller holds one permit from `owner->slots`;
	 *  - `owner->accepting_submissions()` is still true, the service being required to
	 *    outlive every operation started on it;
	 *  - the request's `in` and `out`, if any, point at storage that outlives the block
	 *    rather than into the awaiting frame.
	 *
	 * Thread affinity: may be awaited from any thread; in practice the invariant above
	 * means it is awaited on a thread the completion scheduler runs on.
	 */
	class io_awaiter
	{
	public:
		/**
		 * @brief Allocates the operation block. Submits nothing until await_suspend.
		 *
		 * @param owner   The service state the operation co-owns. Must not be null.
		 * @param request The operation. Moved into the block; see io_operation::request.
		 * @param file    The descriptor to co-own, or an explicit nullptr for an operation
		 *                that names no handle (a path operation, or an escape-hatch call).
		 *                An operation against a handle MUST pass the
		 *                alt::detail::io_handle that owns it and not merely set
		 *                `request.handle`, or nothing keeps the descriptor -- or its
		 *                NUMBER -- from being reused under the backend.
		 *
		 *                DELIBERATELY NOT DEFAULTED. A caller that sets `request.handle`
		 *                and omits this argument loses the keepalive silently, leaving the
		 *                descriptor owned by nothing but the frame that cancellation
		 *                destroys -- which is the defect
		 *                IoOperationTest.TheDescriptorSurvivesTheChainsTeardown exists to
		 *                detect, and which it does detect. Requiring the argument turns
		 *                that from a documented hazard into a compile error, at the cost of
		 *                an explicit nullptr at the handle-less call sites -- which reads
		 *                better than an invisible omission anyway.
		 */
		io_awaiter(std::shared_ptr<io_shared> owner, io_request request, std::shared_ptr<io_handle> file);

		~io_awaiter() = default;

		/**
		 * Movable, and it has to be: alt wraps every foreign awaitable in
		 * alt::detail::cancellation_checked, which moves the awaiter before it suspends.
		 * That is also why the std::stop_callback is not a member of this class -- it is
		 * immovable -- and is built inside await_suspend behind a std::unique_ptr instead.
		 */
		io_awaiter(io_awaiter&&) noexcept            = default;
		io_awaiter& operator=(io_awaiter&&) noexcept = default;

		io_awaiter(const io_awaiter&)            = delete;
		io_awaiter& operator=(const io_awaiter&) = delete;

		/** @brief Always suspends: there is nothing to report before the backend has run. */
		// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the compiler calls await_ready() through an awaiter instance at every co_await of this type, so making it static would relocate the diagnostic to each of those call sites as readability-static-accessed-through-instance -- including inside alt::detail::cancellation_checked, which wraps every await in the library and where no suppression of ours belongs. alt::scheduler.hpp and alt::cancellable_task.hpp both carry this suppression for the same reason.
		[[nodiscard]] bool await_ready() const noexcept
		{
			return false;
		}

		/**
		 * @brief Registers the stop callback, submits, and suspends.
		 *
		 * TEMPLATED OVER THE PROMISE ON PURPOSE. An erased std::coroutine_handle<> has no
		 * promise() to ask, so alt::get_cancellation_ref would yield an EMPTY view and
		 * every line of cancellation handling below would silently do nothing --
		 * docs/cancellation_ref.md calls this the erased-handle trap. A plain alt::task
		 * awaited outside a cancellable chain legitimately yields an empty view too, and
		 * then this awaiter degrades to an uncancellable operation, which is correct.
		 *
		 * Returns true on every path, so the awaiting coroutine is resumed only by the
		 * posted resumption. A submission the backend refuses outright is posted like any
		 * other outcome rather than resumed here, which keeps resume() the only place a
		 * frame is ever touched and the three-step order the only resumption path.
		 *
		 * @param h The suspending frame.
		 * @return true, always. See above.
		 */
		template<typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> h)
		{
			// Written without the lock, which is safe for exactly as long as nothing else
			// can reach the block: no stop callback is registered and nothing has been
			// submitted. start() is where that stops being true.
			m_block->handle = h;
			m_block->chain  = alt::get_cancellation_ref(h);

			return start();
		}

		/**
		 * @brief The operation's outcome, as the backend reported it.
		 *
		 * The backend's result shape stops here: the calling coroutine turns this into the
		 * std::expected its own signature promises. Reached on the resumed path only -- an
		 * abandoned chain never runs another statement of its body.
		 */
		io_completion await_resume();

	private:
		/**
		 * The non-template half of await_suspend. See src/io_operation.cpp, where the
		 * ordering argument for every step lives.
		 */
		bool start();

		/** The stop callback's type, spelled once. */
		using stop_registration = std::stop_callback<io_operation::stop_handler>;

		std::shared_ptr<io_operation> m_block;

		/**
		 * Declared after m_block so it is destroyed FIRST: deregistering the callback
		 * before releasing the block's reference means no handler can be mid-flight over a
		 * block this awaiter was the last owner of.
		 *
		 * Behind a pointer because a std::stop_callback does not move and this awaiter
		 * does, and constructed inside await_suspend for the same reason -- after the move,
		 * where the address of the thing the callback refers to is finally stable.
		 */
		std::unique_ptr<stop_registration> m_stop;
	};

}

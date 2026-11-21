#include <alt/detail/io_operation.hpp>
#include <alt/scheduler.hpp>
#include <cassert>
#include <expected>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

namespace alt::detail
{

	io_operation::io_operation(std::shared_ptr<io_shared> service, io_request description, std::shared_ptr<io_handle> descriptor) noexcept:
	  owner(std::move(service)),
	  file(std::move(descriptor)),
	  request(std::move(description))
	{
		assert(owner != nullptr && "an alt file I/O operation with no service state");
	}

	io_operation::~io_operation()
	{
		// DELICATE. This store is what the whole sentinel rests on: it runs before any
		// member of this object is destroyed, which is why alive() can speak for a
		// destroyed block at all. See the `sentinel` member.
		//
		// Stored BEFORE the teardown, not after: a stop callback or a backend completion
		// racing this destructor is the hazard, and a pattern stored afterwards would miss
		// exactly that interleaving while catching only the easier post-destruction one.
		sentinel.store(dead_pattern, std::memory_order_relaxed);
	}

	bool io_operation::alive() const noexcept
	{
		return sentinel.load(std::memory_order_relaxed) == alive_pattern;
	}

	void io_operation::stop_handler::operator()() const noexcept
	{
		// Copied to this stack frame before anything else. On a completion scheduler that
		// runs a posted item where it is posted, the teardown posted below destroys the
		// awaiting frame, and with it the awaiter's std::stop_callback -- which is where
		// *this lives. Nothing may be read through `this` or `block` after the post.
		const std::shared_ptr<io_operation> self = block;

		assert(self->alive() && "detaching an alt file I/O operation on a destroyed block");

		io_token    to_cancel{};
		io_backend* cancel_at = nullptr;
		bool        post_now  = false;
		{
			const std::scoped_lock lock{self->mutex};

			// The backend got there first. It owns the outcome and the resumption, so this
			// callback must touch neither `handle` nor `chain` nor the frame.
			if(self->state != phase::waiting)
			{
				return;
			}

			self->state = phase::detached;
			to_cancel   = self->token;
			// The backend that issued the token; see `submitted_to`. Set before the token is.
			cancel_at = self->submitted_to;

			// Deferred to io_awaiter::start while that function is still on the stack,
			// which is what keeps this from freeing the frame it is still writing to. See
			// the `starting` member.
			if(!self->starting)
			{
				self->keep = self;
				post_now   = true;
			}
		}

		// A HINT, and nothing more: the completion still arrives exactly once, possibly
		// reporting success. Issued outside the mutex, because a backend takes its own lock
		// and runs callbacks under none, so taking the two in the other order here is the
		// only inversion available and it costs nothing to avoid.
		if(to_cancel)
		{
			assert(cancel_at != nullptr && "an alt file I/O operation holds a token but no backend that issued it");
			cancel_at->cancel(to_cancel);
		}

		if(post_now)
		{
			// THE LAST STATEMENT. See the note at the top of this function.
			self->post_resumption();
		}
	}

	void io_operation::on_backend_complete(void* state, io_completion result) noexcept
	{
		auto* const self = static_cast<io_operation*>(state);

		assert(self->alive() && "completing an alt file I/O operation on a destroyed block");

		// The reference the seam's void* stood for, taken back here. It is released when
		// this function returns, which is the moment the backend is finally done with the
		// block and with the buffer and descriptor it co-owns.
		std::shared_ptr<io_operation> submitted;
		bool                          post_now = false;
		{
			const std::scoped_lock lock{self->mutex};

			submitted = std::move(self->backend_ref);

			if(self->state == phase::waiting)
			{
				self->state  = phase::done;
				self->result = result;

				if(!self->starting)
				{
					self->keep = submitted;
					post_now   = true;
				}
			}
			// Otherwise the chain was stopped first. The result is discarded with the block
			// and NOTHING here touches `handle`, `chain` or the frame: the frame may
			// already be gone, which is the whole reason this block exists.
		}

		if(post_now)
		{
			self->post_resumption();
		}

		// Last, so that an alt::file_io destructor waiting on quiescence cannot return
		// while this callback is still reading the block.
		self->owner->leave_backend();
	}

	void io_operation::post_resumption() noexcept
	{
		assert(alive() && "posting an alt file I/O resumption on a destroyed block");

		try
		{
			owner->completion.post(work_item{.invoke = &io_operation::resume, .state = this});
		}
		catch(...)
		{
			// Nowhere to post. The chain is LEFT LEAKED rather than resumed on the wrong
			// thread: a suspended frame nobody frees costs memory, while running a
			// coroutine body on an arbitrary thread breaks the one rule the consumer has.
			// cpwf's Handoff::post_home_locked settles it the same way.
			//
			// Moved to a local rather than reset under the lock, because dropping the last
			// reference to this block while holding its own mutex would destroy that mutex
			// from inside it -- the use-after-free class no sanitizer here reports.
			std::shared_ptr<io_operation> dropped;
			{
				const std::scoped_lock lock{mutex};
				dropped = std::move(keep);
			}
		}
	}

	void io_operation::resume(void* state) noexcept
	{
		auto* const self = static_cast<io_operation*>(state);

		assert(self->alive() && "resuming an alt file I/O operation on a destroyed block");

		// (a) Everything this needs, read out under the mutex and into locals. `keep` is
		// MOVED to a local rather than reset, so the block stays alive across the unlock
		// below and across the resumption, whose teardown branch drops every other
		// reference to it.
		std::shared_ptr<io_operation> keep;
		std::coroutine_handle<>       handle;
		cancellation_ref              chain;
		{
			const std::scoped_lock lock{self->mutex};

			assert(self->state != phase::waiting && "resuming an alt file I/O operation that nobody finished");

			keep   = std::move(self->keep);
			handle = self->handle;
			chain  = self->chain;
		}

		// (b) The permit, on this thread and BEFORE the resumption. release() hands it to a
		// parked waiter and resumes that waiter inline, so the next operation submits from
		// here -- a thread the completion scheduler runs on -- rather than from an I/O
		// thread. See alt::detail::io_awaiter for why that ordering is forced rather than
		// chosen.
		self->owner->slots.release();

		// (c) THE LAST TOUCH. Both branches may free the frame, so nothing may be read
		// through the block, the awaiter or the chain after this. The bool it returns is a
		// value in this stack frame and is safe to drop.
		static_cast<void>(chain.resume_or_abandon(handle));
	}

	io_awaiter::io_awaiter(std::shared_ptr<io_shared> owner, io_request request, std::shared_ptr<io_handle> file, io_permit permit):
	  m_block(std::make_shared<io_operation>(std::move(owner), std::move(request), std::move(file))),
	  m_permit(std::move(permit))
	{
	}

	void io_awaiter::hand_over_permit() noexcept
	{
		assert(m_permit.holds() && "an alt io_awaiter started without a permit");
		m_permit.hand_over();
	}

	io_completion io_awaiter::await_resume()
	{
		// Deregistered here rather than left to the frame's destruction: the operation is
		// over, and ~stop_callback blocks until an invocation already running on another
		// thread returns, so after this no handler can be mid-flight against the block. No
		// deadlock is possible, because this holds no lock such a handler wants.
		m_stop.reset();

		// Under the mutex although nothing else can still be writing it, because "nothing
		// else" is an argument rather than a guarantee and the lock costs one uncontended
		// acquisition per operation.
		const std::scoped_lock lock{m_block->mutex};
		return m_block->result;
	}

	bool io_awaiter::start()
	{
		// A counted local, and the exact invariant matters: FROM THE std::stop_callback
		// REGISTRATION BELOW TO THE END OF THIS FUNCTION, NOTHING READS A MEMBER OF *this.
		// Everything past that one line goes through this local and the io_shared&
		// derived from it. That is where the whole safety argument sits, because from the
		// registration onwards a stop request on another thread can detach the block, and
		// on a completion scheduler that runs a posted item where it is posted the
		// resulting teardown frees this awaiter's frame.
		//
		// `block->starting` is the other half, and only its stop-side half is load-bearing
		// in this body: the one statement that writes a member after the registration is
		// the assignment OF that registration, so a handler which fired inline from its
		// own constructor and posted immediately would free the frame mid-assignment. The
		// gate also defers an inline backend completion's post, which this body does not
		// need -- it reads no member by then -- and is kept because one uniform "who
		// posts" rule is easier to keep true than two.
		const std::shared_ptr<io_operation> block = m_block;
		io_shared&                          owner = *block->owner;

		// From here every path below releases the permit itself, including the throwing
		// ones, so the frame's io_permit must stop doing so.
		hand_over_permit();

		// A precondition rather than a reportable error, and the same one alt::thread_pool
		// and alt::scheduler_ref already state: the service must outlive every operation
		// started on it. There is no legitimate way to observe a submission refused for
		// this reason, which is why no error enumerator exists for it.
		assert(owner.accepting_submissions() && "submitting an alt file I/O operation after its service began shutting down");

		try
		{
			// Constructed HERE rather than in the awaiter's constructor: a
			// std::stop_callback does not move and alt moves a foreign awaiter before it
			// suspends, so this is the first point at which this awaiter's address is
			// final. cpwf's CommitOnWorker::stop_ is the precedent and says the same.
			//
			// The handler can run before this assignment completes, from inside the
			// constructor, when the token is already stopped. `block->starting` is what
			// makes that harmless: the handler records the detach and posts nothing, so the
			// frame this is writing into cannot be freed underneath it.
			m_stop = std::make_unique<stop_registration>(block->chain.get_stop_token(), io_operation::stop_handler{block});
		}
		catch(...)
		{
			// Nothing was registered and nothing submitted, so the only thing this awaiter
			// has taken over is the permit. Handing it back before the exception leaves
			// await_suspend keeps the bound honest.
			owner.slots.release();
			throw;
		}

		// The handler above may have fired inline from its own registration, which is the
		// one window in which a block can be detached before anything has been submitted.
		// Submitting anyway and cancelling afterwards would be within the seam's contract
		// -- the cancel is a hint and the completion still arrives -- but it would mean a
		// write the caller stopped BEFORE IT WAS EVER SUBMITTED still reaching the kernel.
		// Skipping the submission outright is strictly better, and it makes this window
		// behave like the slot window ahead of it, where a stop means the operation never
		// reaches the backend at all.
		{
			bool teardown = false;
			{
				const std::scoped_lock lock{block->mutex};

				if(block->state != io_operation::phase::waiting)
				{
					assert(block->state == io_operation::phase::detached && "an alt file I/O operation was completed before it was submitted");

					block->starting = false;
					block->keep     = block;
					teardown        = true;
				}
			}

			if(teardown)
			{
				// THE LAST STATEMENT on this path: the post may run the teardown here and
				// free this awaiter's frame, so nothing below may touch *this.
				block->post_resumption();
				return true;
			}
		}

		// Chosen once and kept, so every cancel below and in the stop callback goes to the
		// backend this submission does. See `submitted_to`.
		io_backend& target = owner.backend_for(block->file.get());

		// The reference the seam's void* stands for, parked before the submission because
		// the completion may fire inline inside submit() and takes it straight back out.
		{
			const std::scoped_lock lock{block->mutex};
			block->backend_ref  = block;
			block->submitted_to = &target;
		}

		owner.enter_backend();

		std::expected<io_token, std::error_code> submitted;
		try
		{
			submitted = target.submit(block->request, &io_operation::on_backend_complete, block.get());
		}
		catch(...)
		{
			// Treated as a submission that never happened, which the seam's contract
			// allows: only a submission that RETURNS a token runs its callback.
			//
			// The one combination this cannot survive is a backend that runs the
			// completion callback INLINE and then throws: on_backend_complete has already
			// lowered the backend count, so this would lower it a second time and underflow
			// it. The seam forbids that combination -- a submission either runs its
			// callback exactly once and returns a token, or does neither -- so this is a
			// note rather than a guard. A guard here would have to distinguish "the
			// callback ran" from "the chain was detached", and the block's own state cannot:
			// both leave it non-waiting.
			owner.leave_backend();

			bool teardown = false;
			{
				const std::scoped_lock lock{block->mutex};

				block->backend_ref.reset();
				block->starting = false;

				if(block->state == io_operation::phase::detached)
				{
					block->keep = block;
					teardown    = true;
				}
				else
				{
					// Nobody else can finish this operation now, so the block is closed
					// against a stop request that arrives while the exception travels.
					block->state = io_operation::phase::done;
				}
			}

			if(teardown)
			{
				// A detached chain must not resume, with or without an exception, so the
				// throw dies with the block -- the same convention the escape hatch's
				// captured exception follows.
				block->post_resumption();
				return true;
			}

			owner.slots.release();
			throw;
		}

		if(!submitted)
		{
			// A refusal is an outcome like any other and travels the one resumption path,
			// rather than resuming here: that keeps resume() the only place a frame is
			// ever touched, and its three steps the only order in which that happens.
			owner.leave_backend();

			{
				const std::scoped_lock lock{block->mutex};

				block->backend_ref.reset();
				block->starting = false;

				if(block->state == io_operation::phase::waiting)
				{
					block->state  = io_operation::phase::done;
					block->result = io_completion{.bytes = 0, .error = submitted.error()};
				}

				// Either the state just set, or the detached one a stop request left while
				// the gate was down. The resumption tells them apart.
				block->keep = block;
			}

			block->post_resumption();
			return true;
		}

		io_token to_cancel{};
		bool     post_now = false;
		{
			const std::scoped_lock lock{block->mutex};

			block->token = *submitted;

			// The gate comes down here. From this point either of the other two parties may
			// post, and this function may no longer read its own frame.
			block->starting = false;

			if(block->state != io_operation::phase::waiting)
			{
				// Somebody reached the block while the gate was down: the stop callback
				// fired inline from its own registration above, or the backend completed
				// inline inside submit(). Either way the post is this function's to make.
				if(block->state == io_operation::phase::detached)
				{
					to_cancel = block->token;
				}

				block->keep = block;
				post_now    = true;
			}
		}

		if(to_cancel)
		{
			target.cancel(to_cancel);
		}

		if(post_now)
		{
			// THE LAST STATEMENT on this path: the post may run the resumption here and
			// free this awaiter's frame, so nothing below may touch *this.
			block->post_resumption();
		}

		return true;
	}

}

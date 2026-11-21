// The io_uring backend exists only on Linux, so this whole file does too.
#ifdef __linux__

#	include "io_backend_io_uring.hpp"

#	include <algorithm>
#	include <alt/io_backend.hpp>
#	include <alt/thread_pool.hpp>
#	include <atomic>
#	include <bit>
#	include <cassert>
#	include <cerrno>
#	include <cstddef>
#	include <cstdint>
#	include <exception>
#	include <expected>
#	include <liburing.h>
#	include <memory>
#	include <mutex>
#	include <new>
#	include <system_error>
#	include <thread>

#	include "io_backend_internal.hpp"

namespace alt::detail
{
	bool io_uring_probe_has_required_opcodes(const ::io_uring_probe& probe) noexcept
	{
		// Four calls rather than a loop over a table: the list is fixed, short, and every entry is
		// named at the one place a reader will look for it.
		return io_uring_opcode_supported(&probe, IORING_OP_READ) != 0 && io_uring_opcode_supported(&probe, IORING_OP_WRITE) != 0 &&
		       io_uring_opcode_supported(&probe, IORING_OP_FSYNC) != 0 && io_uring_opcode_supported(&probe, IORING_OP_ASYNC_CANCEL) != 0;
	}

	namespace
	{
		/**
		 * The user_data of the NOP the destructor submits to wake the reaper thread.
		 *
		 * It is the ring tag with an operation id of zero, which is a value no token of this
		 * backend takes: alt::io_token reserves zero for the empty token, so an issued id is never
		 * zero. One value of the tagged space is therefore reserved and belongs to no operation,
		 * which is what lets the reaper recognize its own wake-up without consulting anything.
		 */
		constexpr std::uint64_t wake_up_user_data = io_uring_token_tag;

		/// The kernel's ceiling on submission ring entries (IORING_MAX_ENTRIES).
		constexpr std::size_t max_submission_entries = 32768;

		/// The kernel's ceiling on completion ring entries (IORING_MAX_CQ_ENTRIES), twice the above.
		constexpr std::size_t max_completion_entries = 2 * max_submission_entries;

		std::error_code bad_alloc_error() noexcept
		{
			return std::make_error_code(std::errc::not_enough_memory);
		}

		/// @p value rounded up to a power of two; 0 and 1 both give 1, the smallest ring there is.
		std::size_t round_up_to_power_of_two(const std::size_t value) noexcept
		{
			if(value <= 1)
			{
				return 1;
			}
			return std::size_t{1} << std::bit_width(value - 1);
		}

		/// What a ring is asked for: both counts powers of two, and both within the kernel's ceilings.
		struct ring_sizes
		{
			unsigned submission_entries = 1;
			unsigned completion_entries = 1;
		};

		/**
		 * The two bounds the ring sizes are derived from.
		 *
		 * A struct rather than a pair of parameters so that the derivation's one call site names
		 * both values instead of passing them positionally: they are counts of the same type, they
		 * mean entirely different things, and swapping them would produce a working ring of the
		 * wrong shape rather than an error.
		 *
		 * It does NOT remove that hazard from the path as a whole, and should not be read as
		 * having done so. make_io_uring_backend() and create_ring() both still take the pair as
		 * two adjacent std::size_t parameters, and a caller that swaps them there gets no
		 * diagnostic from the compiler or from clang-tidy, whose
		 * bugprone-easily-swappable-parameters suppresses a pair that is passed straight on to one
		 * other call -- which is exactly what each of those two does. The FACTORY'S boundary is
		 * where the hazard is live, so a call site there names both values; the factory's own
		 * documentation says so.
		 */
		struct ring_bounds
		{
			/// The submission ring size the caller asked for, or 0 to derive it from max_in_flight.
			std::size_t queue_depth = 0;
			/// How many operations may be at this backend at once.
			std::size_t max_in_flight = 0;
		};

		/**
		 * The ring sizes for @p bounds.
		 *
		 * The submission ring is what queue_depth names, or max_in_flight when queue_depth is
		 * zero. Without SQPOLL the kernel consumes submission entries synchronously inside
		 * io_uring_submit, and a submission here holds the submission mutex across both acquiring
		 * the entry and submitting it, so the submission queue is drained again before the lock is
		 * released: a full submission queue is all but unreachable at any depth, which is what
		 * makes the depth the caller's to choose freely.
		 *
		 * What actually bounds outstanding work is the completion ring, which defaults to twice
		 * the submission ring and, on overflow, makes io_uring_submit fail or forces the kernel to
		 * hold completions in a backlog. So it is sized from max_in_flight instead, plus the
		 * reserved internal completions -- the wake-up NOP, and one cancel for each operation that
		 * can be in flight at once -- and never below the submission ring, which the kernel
		 * requires. Completion-queue overflow is then unreachable by construction rather than by
		 * assumption, whatever depth the caller passes.
		 *
		 * **That holds for max_in_flight up to and including 32767, and not above it.** Covering
		 * max_in_flight needs `2 * max_in_flight + 1` completion entries, and the kernel's ceiling
		 * is 65536: 32767 needs 65535 and gets 65536, while 32768 needs 65537 and gets 65536, one
		 * entry short. Larger values fall further behind, since in_flight below is clamped to
		 * 32768 before `reserved` is computed, so a max_in_flight of 100000 is permitted against a
		 * completion ring of 65536.
		 *
		 * Above the bound the guarantee weakens from unreachable to merely safe, and an extreme
		 * max_in_flight is accepted rather than refused: refusing a legal std::size_t for it would
		 * be the worse contract, and the degradation is benign. Every kernel that passes the
		 * capability check is 5.6 or newer, hence sets IORING_FEAT_NODROP, under which an
		 * overflowing completion goes to a kernel-side backlog and further submissions are refused
		 * with EBUSY -- which submit() already reports as the errno it is -- rather than a
		 * completion being dropped. No callback is lost at any max_in_flight; only the "cannot
		 * happen" claim is.
		 *
		 * Both counts are clamped to the kernel's ceilings here, which is what turns an
		 * out-of-range request into a working ring instead of the EINVAL io_uring_setup answers it
		 * with. Rounding up to a power of two is something the kernel does to them anyway, so
		 * doing it here as well changes no ring; it costs nothing and makes the counts this code
		 * asks for the counts it gets.
		 */
		ring_sizes derive_ring_sizes(const ring_bounds& bounds) noexcept
		{
			const std::size_t in_flight         = std::min(bounds.max_in_flight, max_submission_entries);
			const std::size_t wanted_submission = bounds.queue_depth != 0 ? bounds.queue_depth : std::max<std::size_t>(in_flight, 1);
			const std::size_t submission        = round_up_to_power_of_two(std::min(wanted_submission, max_submission_entries));
			const std::size_t reserved          = in_flight + 1;
			const std::size_t wanted_completion = std::max(in_flight + reserved, submission);
			const std::size_t completion        = round_up_to_power_of_two(std::min(wanted_completion, max_completion_entries));
			return ring_sizes{.submission_entries = static_cast<unsigned>(submission), .completion_entries = static_cast<unsigned>(completion)};
		}

		class io_uring_backend final: public io_backend
		{
		public:
			/**
			 * Builds the pool backend every operation is performed on. Creates no ring and starts no
			 * thread; see create_ring(), check_capabilities() and start().
			 *
			 * The pool backend is the PLAIN alt::make_thread_pool_backend(), with no
			 * detail::pool_open_policy. io_uring operates on ordinary file descriptors, so there is
			 * nothing an open has to do differently for a ring's sake -- no extra open flags, no
			 * prepare hook, and no reason to keep descriptors from outside away from the ring. The
			 * IOCP backend's open policy exists because an overlapped transfer is valid only on a
			 * handle opened with FILE_FLAG_OVERLAPPED and associated with its port; neither has an
			 * equivalent here, and a reader who comes looking for that machinery should stop here.
			 */
			explicit io_uring_backend(thread_pool& pool): m_pool_backend(alt::make_thread_pool_backend(pool))
			{}

			io_uring_backend(const io_uring_backend&)            = delete;
			io_uring_backend& operator=(const io_uring_backend&) = delete;
			io_uring_backend(io_uring_backend&&)                 = delete;
			io_uring_backend& operator=(io_uring_backend&&)      = delete;

			~io_uring_backend() override
			{
				// file_io waits for every operation before destroying its backend, so an operation
				// still in flight is a caller's bug. Freeing what it needs would hand a pool worker,
				// or the kernel, memory that is gone, and leaving it would leave a callback to report
				// into a destroyed backend, so neither is a way out: debug builds say why, and every
				// build stops.
				//
				// Every operation this backend accepts is forwarded to the pool, so this one count is
				// the whole of the in-flight count. An arm that submits to the ring keeps a registry
				// of its own records and answers the same question from it, under the same rule: ANY
				// operation in flight, the ring's or the pool's, is a precondition violation.
				if(m_forwarded.load() != 0)
				{
					assert(false && "io_uring backend destroyed with forwarded operations in flight");
					std::terminate();
				}
				if(m_thread.joinable())
				{
					// Without the wake-up the join below never returns: the reaper is blocked in
					// io_uring_wait_cqe, which has no timeout. Quiescence is established above, so the
					// NOP is the only completion left for it to see.
					if(!post_wake_up())
					{
						std::terminate();
					}
					m_thread.join();
				}
				// Last, and only now: the reaper is gone, so nothing is reading the rings any more.
				if(m_ring_created)
				{
					io_uring_queue_exit(&m_ring);
				}
			}

			/// Creates the ring. Called once, by make_io_uring_backend(), before anything else.
			std::error_code create_ring(const std::size_t queue_depth, const std::size_t max_in_flight) noexcept
			{
				const ring_sizes sizes = derive_ring_sizes(ring_bounds{.queue_depth = queue_depth, .max_in_flight = max_in_flight});
				io_uring_params  params{};
				params.flags      = IORING_SETUP_CQSIZE;
				params.cq_entries = sizes.completion_entries;
				const int created = io_uring_queue_init_params(sizes.submission_entries, &m_ring, &params);
				if(created < 0)
				{
					// A negated errno, returned rather than left in errno: ENOSYS where the kernel has
					// no io_uring at all, and EPERM where kernel.io_uring_disabled or a seccomp filter
					// forbids the syscall, which is common inside a container. Both are ordinary
					// operating conditions for a caller that can fall back, not defects.
					return errno_error(-created);
				}
				m_ring_created = true;
				// What the kernel allocated, which it writes back into the params it was given. The
				// counts above are already powers of two within its ceilings, so this agrees with
				// them; reading it back is what makes the accessors report the ring in effect rather
				// than the request that produced it.
				m_submission_depth = params.sq_entries;
				m_completion_depth = params.cq_entries;
				return {};
			}

			/**
			 * Probes the running kernel for every opcode this backend needs. Called once, by
			 * make_io_uring_backend(), after create_ring() and before start().
			 *
			 * Creating a ring is not evidence that the ring can do this work. Rings have existed
			 * since kernel 5.1, but IORING_OP_READ and IORING_OP_WRITE arrived in 5.6 and
			 * IORING_OP_ASYNC_CANCEL in 5.5, so on a 5.1 to 5.5 kernel io_uring_queue_init succeeds
			 * and every transfer then fails with EINVAL.
			 *
			 * One all-or-nothing check, not a per-operation matrix: that is what keeps this a single
			 * error path, and it costs one extra syscall once per service. A refused probe counts as
			 * a missing opcode, because IORING_REGISTER_PROBE itself arrived in 5.6 alongside READ
			 * and WRITE, so a kernel that will not answer is a kernel too old to give the answer
			 * this backend needs.
			 */
			std::error_code check_capabilities() noexcept
			{
				const std::error_code not_supported = std::make_error_code(std::errc::not_supported);
				io_uring_probe* const probe         = io_uring_get_probe_ring(&m_ring);
				if(probe == nullptr)
				{
					return not_supported;
				}
				// The decision itself lives in io_uring_probe_has_required_opcodes(), which the
				// header exports so the suite can put a synthetic probe through it. What is left
				// here is the part a test cannot reach: asking the running kernel, and acting on
				// the answer.
				const bool supported = io_uring_probe_has_required_opcodes(*probe);
				io_uring_free_probe(probe);
				return supported ? std::error_code{} : not_supported;
			}

			/// Starts the reaper thread. Called once, by make_io_uring_backend(), last of the three.
			std::error_code start() noexcept
			{
				try
				{
					m_thread = std::thread([this] { reap(); });
				}
				catch(const std::system_error& error)
				{
					return error.code();
				}
				catch(const std::bad_alloc&)
				{
					return bad_alloc_error();
				}
				return {};
			}

			[[nodiscard]] std::expected<io_token, std::error_code> submit(const io_request& request, io_completion_fn on_done, void* state) override
			{
				// Forwarded without exception: nothing is performed on the ring here, and seven of
				// the eleven operations never can be. open, the two stats, rename and unlink have
				// opcodes that arrived across kernels 5.6 to 5.11 and would each need their own probe
				// and fallback for no latency anyone is waiting on; a directory listing has no opcode
				// at all, since getdents is not exposed; and invoke runs an arbitrary callable, which
				// a submission queue has nowhere to run.
				return submit_forwarded(request, on_done, state);
			}

			void cancel(io_token token) noexcept override
			{
				if(!token)
				{
					return;
				}
				// Only the internal pool backend issues tokens here, and a pool token never carries
				// the tag, which submit_forwarded() asserts. So the tag is what routes a cancel, and
				// a tagged token names no operation this backend is holding.
				if((token.value() & io_uring_token_tag) == 0)
				{
					m_pool_backend->cancel(token);
				}
			}

			[[nodiscard]] io_backend_kind kind() const noexcept override
			{
				return io_backend_kind::io_uring;
			}

			[[nodiscard]] std::size_t submission_depth() const noexcept
			{
				return m_submission_depth;
			}

			[[nodiscard]] std::size_t completion_depth() const noexcept
			{
				return m_completion_depth;
			}

			[[nodiscard]] std::size_t ignored_completions() const noexcept
			{
				return m_ignored.load();
			}

		private:
			/// What a forwarded operation's trampoline needs: the caller's callback, and the count it lowers.
			struct forwarded_operation
			{
				io_completion_fn          on_done     = nullptr;
				void*                     state       = nullptr;
				std::atomic<std::size_t>* outstanding = nullptr;
			};

			/**
			 * Runs on the pool worker when a forwarded operation completes.
			 *
			 * The count is lowered BEFORE the caller's callback runs, and that decrement is this
			 * trampoline's last access to the backend. The other order cannot work: file_io's
			 * callback lowers its own quiescence count as its last act, after which ~file_io may
			 * destroy this backend at once, so a decrement after it would write into a destroyed
			 * backend, or race the destructor's check and stop the process for an operation that was
			 * already finished. Lowering it first is enough, because by the time the callback runs
			 * the pool job holds everything it still needs and wants nothing from this backend.
			 *
			 * The callback runs with no lock of this backend's held, as every completion path here
			 * must: it is the file layer's operation block's entry point and takes locks of its own.
			 */
			static void forwarded_done(void* raw, io_completion result) noexcept
			{
				const std::unique_ptr<forwarded_operation> operation(static_cast<forwarded_operation*>(raw));
				operation->outstanding->fetch_sub(1);
				operation->on_done(operation->state, result);
			}

			/// Hands @p request to the pool backend through forwarded_done(), counting it while it is in flight.
			std::expected<io_token, std::error_code> submit_forwarded(const io_request& request, io_completion_fn on_done, void* state)
			{
				std::unique_ptr<forwarded_operation> operation;
				try
				{
					operation = std::make_unique<forwarded_operation>(forwarded_operation{.on_done = on_done, .state = state, .outstanding = &m_forwarded});
				}
				catch(const std::bad_alloc&)
				{
					return std::unexpected(bad_alloc_error());
				}
				// Raised before the submission, which may complete inline and lower it again.
				m_forwarded.fetch_add(1);
				auto token = m_pool_backend->submit(request, forwarded_done, operation.get());
				if(!token)
				{
					// A refused submission never runs its callback, so the trampoline is still ours.
					m_forwarded.fetch_sub(1);
					return token;
				}
				// From here the trampoline owns itself; it may already have run and freed what this
				// hands back, which is exactly why the pointer is dropped rather than kept or read.
				const forwarded_operation* const released = operation.release();
				static_cast<void>(released);
				assert((token->value() & io_uring_token_tag) == 0 && "a pool token collides with the ring tag");
				return token;
			}

			/**
			 * Submits the reserved NOP that returns the reaper from its wait. Called once, by the
			 * destructor, after quiescence is established.
			 *
			 * Holds the submission mutex, which every submission to this ring must hold: liburing's
			 * io_uring_get_sqe() and io_uring_submit() are not safe to call concurrently on a shared
			 * ring, and submit() may be called from any thread. The reaper takes no lock of this
			 * backend's at all, so there is no lock order between the two arms to get wrong.
			 *
			 * @return Whether the NOP reached the ring. False means the reaper will not wake.
			 */
			bool post_wake_up() noexcept
			{
				const std::scoped_lock lock(m_submission_mutex);
				io_uring_sqe* const    sqe = io_uring_get_sqe(&m_ring);
				if(sqe == nullptr)
				{
					return false;
				}
				io_uring_prep_nop(sqe);
				// Assigned directly rather than through io_uring_sqe_set_data64(), which liburing
				// 2.1 -- this project's floor, and the version on Ubuntu 22.04 and Debian 12 -- does
				// not promise. user_data is a field of the kernel's own submission entry and has
				// been one for as long as rings have existed. No gate here exercises that floor:
				// this machine and CI's runner both have a much later liburing, so a helper that
				// arrived after 2.1 would compile and pass everywhere and fail only for a consumer
				// on the oldest supported distribution. Every liburing call in this file is one 2.1
				// is known to have, by care rather than by a check.
				sqe->user_data = wake_up_user_data;
				return io_uring_submit(&m_ring) > 0;
			}

			/// The reaper thread: consumes completions until the destructor's wake-up arrives.
			void reap() noexcept
			{
				for(;;)
				{
					io_uring_cqe* cqe    = nullptr;
					const int     waited = io_uring_wait_cqe(&m_ring, &cqe);
					if(waited < 0)
					{
						// io_uring_enter reports EINTR when a signal arrives during the wait, and the
						// completion it was waiting for is still there to collect. This is the one
						// place EINTR belongs in this backend: a submitted operation cannot return it,
						// so there is no per-operation retry anywhere here, unlike the pool arm's
						// pread. Anything else is a ring that cannot be waited on, which the
						// destructor does not allow while this thread runs.
						if(waited == -EINTR)
						{
							continue;
						}
						std::terminate();
					}
					const std::uint64_t user_data = cqe->user_data;
					io_uring_cqe_seen(&m_ring, cqe);
					if(user_data == wake_up_user_data)
					{
						return;
					}
					// A completion with no operation behind it: counted, never acted on, and above all
					// never dereferenced. user_data carries the token value rather than a record
					// pointer precisely so that a stale or cancelled completion can be looked for and
					// found absent instead of pointing at a record that is gone. The wake-up above is
					// the only thing submitted to this ring, so nothing reaches this line today; the
					// counter is what a completion this backend does not recognize would move, and the
					// tests read it.
					m_ignored.fetch_add(1);
				}
			}

			std::unique_ptr<io_backend> m_pool_backend;

			io_uring m_ring{};
			/// Whether m_ring was created, and so whether the destructor must tear it down.
			bool m_ring_created = false;

			/// Serializes every submission to m_ring; liburing does not make a shared ring thread-safe.
			std::mutex m_submission_mutex;

			/// The ring sizes in effect, as the kernel reported them from create_ring().
			std::size_t m_submission_depth = 0;
			std::size_t m_completion_depth = 0;

			/// Completions the reaper found no operation for.
			std::atomic<std::size_t> m_ignored{0};
			/// Operations forwarded to the pool backend whose trampoline has not yet run.
			std::atomic<std::size_t> m_forwarded{0};
			std::thread              m_thread;
		};
	} // namespace

	std::expected<std::unique_ptr<io_backend>, std::error_code> make_io_uring_backend(thread_pool& pool, const std::size_t queue_depth, const std::size_t max_in_flight)
	{
		std::unique_ptr<io_uring_backend> backend;
		try
		{
			backend = std::make_unique<io_uring_backend>(pool);
		}
		catch(const std::bad_alloc&)
		{
			return std::unexpected(bad_alloc_error());
		}
		// From here the backend owns whatever it has built, and its destructor tears down the ring
		// if a later step fails.
		if(const auto error = backend->create_ring(queue_depth, max_in_flight))
		{
			return std::unexpected(error);
		}
		if(const auto error = backend->check_capabilities())
		{
			return std::unexpected(error);
		}
		if(const auto error = backend->start())
		{
			return std::unexpected(error);
		}
		return std::unique_ptr<io_backend>(std::move(backend));
	}

	std::size_t io_uring_submission_depth_for_testing(const io_backend& ring) noexcept
	{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the declaration's stated precondition is that `ring` was built by make_io_uring_backend(), so it IS an io_uring_backend and the cast asserts that precondition rather than discovering the type. dynamic_cast is not an alternative: the reference form throws, which these noexcept accessors cannot let out, and the pointer form would turn a documented precondition into a silent null for a caller who has already broken it. The IOCP backend's equivalent accessors are written identically; they are simply not seen by this gate, which runs on Linux.
		return static_cast<const io_uring_backend&>(ring).submission_depth();
	}

	std::size_t io_uring_completion_depth_for_testing(const io_backend& ring) noexcept
	{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): same reasoning as io_uring_submission_depth_for_testing above.
		return static_cast<const io_uring_backend&>(ring).completion_depth();
	}

	std::size_t io_uring_ignored_completions_for_testing(const io_backend& ring) noexcept
	{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): same reasoning as io_uring_submission_depth_for_testing above.
		return static_cast<const io_uring_backend&>(ring).ignored_completions();
	}
} // namespace alt::detail

#endif

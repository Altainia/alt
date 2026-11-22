// The io_uring backend exists only on Linux, so this whole file does too.
#ifdef __linux__

#	include "io_backend_io_uring.hpp"

#	include <algorithm>
#	include <alt/io_backend.hpp>
#	include <alt/io_buffer.hpp>
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
#	include <unordered_map>
#	include <utility>

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

		/**
		 * The first operation id this backend issues, which is 2 rather than 1.
		 *
		 * Two values of the tagged space belong to no operation. Zero is the wake-up NOP's, above.
		 * One is held for the user_data of a cancel SQE, whose own completion the reaper must be
		 * able to tell from an operation's; it has no constant of its own yet because nothing
		 * submits a cancel SQE, and a constant nothing reads is a warning under -Werror. Starting
		 * the ids above it is what keeps that value free without defining it, so the arm that
		 * submits cancels does not have to renumber tokens that are already issued.
		 */
		constexpr std::uint64_t first_operation_id = 2;

		/**
		 * The most bytes one read or write SQE may carry: the kernel's own cap, MAX_RW_COUNT.
		 *
		 * `INT_MAX & PAGE_MASK` with 4 KiB pages, which is what the kernel silently truncates a
		 * larger transfer to. Applying it here rather than leaving it to the kernel is what makes
		 * the count this code hands liburing -- an `unsigned` -- the count it means: a
		 * transfer_size() above 2^32 would otherwise be truncated to its low 32 bits, turning a
		 * huge request into an arbitrary small one instead of into the kernel's cap.
		 *
		 * A request above the cap is a SHORT transfer, reported as-is: this backend does not loop.
		 * The file layer already treats a short count as a real result, and looping here would
		 * hide a partial transfer the caller is entitled to see. **Untested, deliberately**: the
		 * case needs a buffer of more than 2 GiB, which is not a reasonable allocation to make in
		 * CI, and a test that asked for the cap with a smaller buffer would be testing the
		 * buffer clamp instead.
		 */
		constexpr std::size_t max_single_transfer = 0x7ffff000;

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
		 * the entry and submitting it, so **a submission the kernel accepts** leaves the queue
		 * empty again before the lock is released: a full submission queue is all but unreachable
		 * at any depth, which is what makes the depth the caller's to choose freely.
		 *
		 * "Accepts" is the load-bearing word, and it is not decoration. A REFUSED io_uring_submit
		 * consumes nothing and leaves its entry published in the ring, so unconsumed entries
		 * accumulate at one per refusal and enough of them would fill the queue after all. That
		 * does not weaken the depth argument much -- the queue then hands back a null SQE, which
		 * publishes nothing and whose record is safe to free, and submit() reports EAGAIN -- but
		 * the claim is "until the first refusal", not "never". submit_to_ring() carries the rest.
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
		 * be the worse contract. Every kernel that passes the
		 * capability check is 5.6 or newer, hence sets IORING_FEAT_NODROP, under which an
		 * overflowing completion goes to a kernel-side backlog rather than being dropped. No
		 * callback is lost at any max_in_flight; only the "cannot happen" claim is.
		 *
		 * That rests on the reaper draining the backlog, which is not automatic -- a backlogged
		 * completion is not in the mapped ring and only an io_uring_enter moves it there -- so it
		 * was MEASURED rather than assumed, against a ring deliberately overflowed. reap() does
		 * drain it: io_uring_wait_cqe() peeks the mapped ring and, finding it empty, enters with
		 * IORING_ENTER_GETEVENTS, and the kernel flushes the overflow list before deciding whether
		 * to sleep. The enter therefore happens exactly when the visible ring is empty, which is
		 * the only state in which the backlog matters, and a submission's own enter flushes what
		 * fits as well. No IORING_SQ_CQ_OVERFLOW handling of this backend's own is needed.
		 *
		 * What a submission attempted while the ring is overflown does is kernel-dependent: older
		 * kernels refuse it with EBUSY, while a current one accepts it and appends its completion to
		 * the backlog (measured on 7.0). No completion is lost either way, which is all this sizing
		 * argument needs -- but the EBUSY arm is NOT inconsequential, and must not be read as such:
		 * it is the one reachable producer of a refused io_uring_submit(), and therefore of
		 * submit_to_ring()'s published-entry path and the quarantine that path exists for. A caller
		 * that exceeds the max_in_flight it declared reaches overflow at any depth, since that bound
		 * is a contract and not something this backend enforces.
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

		/// The file descriptor @p handle carries. A ring operates on ordinary descriptors.
		/// A third copy of the pool backend's own one-line cast; issue #74 moves it to a header.
		int to_fd(const native_handle handle) noexcept
		{
			return static_cast<int>(handle);
		}

		/**
		 * One read or write on the ring, owned by the backend from submission until the reaper has
		 * consumed its completion.
		 *
		 * The buffer copy is what keeps the bytes alive while the kernel owns them; it is released
		 * only after the callback has returned, which is why the record outlives the callback and
		 * the callback does not outlive the record.
		 *
		 * It carries no operation kind, unlike the IOCP backend's transfer_record. There, a read
		 * has to map ERROR_HANDLE_EOF to a zero count while a write must not; here the kernel
		 * reports end of file as a zero-byte success for both, so the mapping from a completion
		 * entry to an io_completion is the same for every operation on this ring and nothing would
		 * read the field.
		 */
		struct transfer_record
		{
			io_buffer        buffer;
			io_completion_fn on_done = nullptr;
			void*            state   = nullptr;

			/**
			 * Whether the caller is still owed this record's callback.
			 *
			 * False for a QUARANTINED record: one whose io_uring_submit() was refused after liburing
			 * had already published its submission entry to the kernel. The caller has been handed an
			 * error, so its callback must never run -- the seam says a submission that returns an
			 * error has not run one and never will -- but the published entry may still be flushed by
			 * a later submission, and the kernel would then write into the storage this record's
			 * io_buffer copy owns. So the record outlives the error: the reaper frees it when the late
			 * completion arrives, and the destructor leaks what is still quarantined. See
			 * submit_to_ring().
			 *
			 * It is also what the destructor's in-flight check counts, which is why it lives here
			 * rather than in a second container: a quarantined record is not an operation in flight,
			 * because nobody is waiting for it.
			 */
			bool callback_is_owed = true;
		};

		/// Everything a submission needs, read from the record before the record can be completed.
		struct transfer_submission
		{
			std::uint64_t user_data = 0;
			int           fd        = -1;
			void*         data      = nullptr;
			unsigned      count     = 0;
			std::uint64_t offset    = 0;
			bool          is_write  = false;
		};

		/**
		 * What a refused submission did before it failed, which is what decides the record's fate.
		 *
		 * The two ways submit_to_ring() can fail are NOT interchangeable, and merging them into one
		 * std::error_code is what made an earlier version of this file unsafe. A null SQE publishes
		 * nothing, so its record can be freed. A refused io_uring_submit() has already had the ring
		 * tail published with a release store, so its entry may still reach the kernel and its record
		 * must not be freed. Only the caller of submit_to_ring() can act on that difference, so it is
		 * carried out rather than reasoned about again at the call site.
		 */
		struct submission_outcome
		{
			std::error_code error;
			/// Whether the submission entry reached the ring's published tail in spite of @ref error.
			bool entry_published = false;
		};

		/// The bytes one SQE may carry for @p request: the shared buffer clamp, then the kernel's cap.
		unsigned transfer_count(const io_request& request) noexcept
		{
			return static_cast<unsigned>(std::min(transfer_size(request), max_single_transfer));
		}

		/**
		 * What a completion entry's @p res means to the caller of submit().
		 *
		 * A non-negative res is a byte count, zero included: a read at or past the end of file is
		 * a short count of zero and no error, which is what the pool arm's pread already reports
		 * and what the file layer already treats as a real result. Short counts are returned
		 * as-is.
		 *
		 * A negative res is a NEGATED errno -- the completion queue carries it there rather than
		 * in errno, which the reaper thread's errno would not hold anyway. detail::errno_error
		 * wants the positive value, so the sign is flipped here; handing it res directly would
		 * produce an error_code in the system category holding a negative number, which compares
		 * equal to no std::errc condition the seam documents and would fail no test that only
		 * asserts "some error".
		 */
		io_completion completion_of(const int res) noexcept
		{
			io_completion result;
			if(res >= 0)
			{
				result.bytes = static_cast<std::size_t>(res);
				return result;
			}
			result.error = errno_error(-res);
			return result;
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
				// ANY operation in flight is a precondition violation, the ring's or the pool's, so
				// both arms are asked and each says which one it was. The ring's answer is read off the
				// registry rather than off a counter, because the registry IS the set of records the
				// kernel may still be writing into -- which is exactly what must not be freed here --
				// and because the question is not whether it is empty. It is whether anything in it
				// still owes a callback.
				//
				// A record is withdrawn before its callback runs, so a registry that owes nothing does
				// not mean that no callback is running. That is the right question anyway, and not
				// because a running callback wants nothing of this backend's -- it may be inside
				// submit() on this very backend, which is a case the suite pins -- but because such a
				// callback runs on the reaper thread, which the join below waits for, and because the
				// caller that owns the completion is the one whose quiescence the precondition is
				// about.
				//
				// What is counted is callbacks still OWED, not records still present. A quarantined
				// record is present and owes nothing: its caller was handed an error and is not
				// waiting for it. Those are dealt with after the join, below.
				bool ring_in_flight = false;
				{
					const std::scoped_lock lock(m_registry_mutex);
					ring_in_flight = std::ranges::any_of(m_records, [](const auto& entry) { return entry.second->callback_is_owed; });
				}
				if(ring_in_flight)
				{
					assert(false && "io_uring backend destroyed with ring operations in flight");
					std::terminate();
				}
				if(m_forwarded.load() != 0)
				{
					assert(false && "io_uring backend destroyed with forwarded operations in flight");
					std::terminate();
				}
				if(m_thread.joinable())
				{
					// Without the wake-up the join below never returns: the reaper is blocked in
					// io_uring_wait_cqe, which has no timeout. The check above establishes that no
					// CALLER'S operation is left, which is not the same as no completion being left:
					// this submission may flush a quarantined entry, and the reaper may see that
					// completion before or after the NOP's. Either order is fine -- a quarantined
					// completion runs no callback and frees its record, and the NOP returns the reaper
					// whenever it arrives -- so what the wake-up guarantees is that the reaper stops,
					// not that it stops with nothing else to see.
					if(!post_wake_up())
					{
						std::terminate();
					}
					m_thread.join();
				}
				// Whatever is still registered is quarantined. The check above says so for the instant
				// it ran, not for this one, and nothing re-establishes it: what makes it true here is
				// that a record can only appear through submit(), which a caller that has gone
				// quiescent is no longer calling. A record that did appear would be that precondition
				// violated past the point of diagnosing it, and releasing it is the safe side of that
				// bug rather than the wrong one. The wake-up
				// NOP's own submission has just flushed any entry still sitting unconsumed in the ring
				// -- so the kernel either holds that read or write or has already completed it. A
				// completion that arrived before the NOP's was reaped and freed normally; these are the
				// ones that did not.
				//
				// They are RELEASED rather than freed: a deliberate, bounded leak of one record and its
				// buffer per refused submission. Freeing them is the use-after-free the quarantine
				// exists to prevent, since the kernel may still write into that storage, and waiting
				// for them is not on offer either -- a quarantined read on a pipe nothing writes to
				// never completes, so the wait would be unbounded and this destructor would hang
				// instead. A leak on a path that should never happen is the mildest of the three.
				{
					const std::scoped_lock lock(m_registry_mutex);
					for(auto& entry: m_records)
					{
						// Spelled through a named pointer, as submit_forwarded() spells its own
						// deliberate release: a bare static_cast<void> does not satisfy
						// bugprone-unused-return-value, which knows what unique_ptr::release() returns.
						const transfer_record* const leaked = entry.second.release();
						static_cast<void>(leaked);
					}
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
				if(request.op == io_op::read || request.op == io_op::write)
				{
					return submit_transfer(request, on_done, state);
				}
				// Everything else is forwarded, and seven of the nine never can be anything else. open,
				// the two stats, rename and unlink have opcodes that arrived across kernels 5.6 to 5.11
				// and would each need their own probe and fallback for no latency anyone is waiting on; a
				// directory listing has no opcode at all, since getdents is not exposed; and invoke runs
				// an arbitrary callable, which a submission queue has nowhere to run.
				return submit_forwarded(request, on_done, state);
			}

			void cancel(io_token token) noexcept override
			{
				if(!token)
				{
					return;
				}
				// The tag is what routes a cancel: a pool token never carries it, which
				// submit_forwarded() asserts, and every token submit_transfer() issues does.
				//
				// A tagged token is accepted and nothing is asked of the kernel for it. That is within
				// the seam's contract -- cancel is a hint, and a ring operation completes on its own
				// whatever happens here -- and it is where an IORING_OP_ASYNC_CANCEL naming the token's
				// user_data belongs once this backend submits one.
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
			/**
			 * Submits @p request to the ring as one read or write SQE.
			 *
			 * The record is registered BEFORE the SQE reaches the kernel, so a completion that lands
			 * while this call is still inside io_uring_submit() finds it. From that moment the record
			 * is the reaper's to complete and free -- possibly before this function's next statement --
			 * so everything the submission still needs is taken out of it first, exactly as the IOCP
			 * backend's submit_transfer() does.
			 */
			std::expected<io_token, std::error_code> submit_transfer(const io_request& request, io_completion_fn on_done, void* state)
			{
				std::unique_ptr<transfer_record> owned;
				try
				{
					owned = std::make_unique<transfer_record>();
				}
				catch(const std::bad_alloc&)
				{
					return std::unexpected(bad_alloc_error());
				}
				// The buffer copy is what keeps the bytes alive while the kernel owns them, and the
				// pointer handed to the SQE below is taken from that copy rather than from the request.
				owned->buffer  = request.buffer;
				owned->on_done = on_done;
				owned->state   = state;

				const transfer_submission submission{.user_data = io_uring_token_tag | m_next_id.fetch_add(1),
				                                     .fd        = to_fd(request.handle),
				                                     .data      = owned->buffer.bytes().data(),
				                                     .count     = transfer_count(request),
				                                     .offset    = request.offset,
				                                     .is_write  = request.op == io_op::write};

				if(const auto error = enroll(submission.user_data, std::move(owned)))
				{
					return std::unexpected(error);
				}
				const submission_outcome outcome = submit_to_ring(submission);
				if(outcome.error)
				{
					// Either way the caller gets an error and no callback ever runs, as the seam
					// requires. What differs is the record's fate. An entry that never reached the ring
					// leaves nothing for the kernel to do, so the record is withdrawn and freed with
					// its buffer copy. A published one may still be flushed to the kernel, and
					// submit_to_ring() has already quarantined it under the submission mutex -- it has
					// to be done there, not here, or another thread's flush can race the flag -- so
					// there is nothing to do for it but report. Freeing a published entry's record is a
					// kernel write into freed heap that no sanitizer in this project's gates can see.
					if(!outcome.entry_published)
					{
						const std::unique_ptr<transfer_record> mine = withdraw(submission.user_data);
						static_cast<void>(mine);
					}
					return std::unexpected(outcome.error);
				}
				return io_token(submission.user_data);
			}

			/**
			 * Registers @p record under @p user_data, which is the token value its SQE will carry.
			 *
			 * On failure the record is freed with its buffer copy and nothing has been submitted, so
			 * the caller's std::unexpected is the whole of the outcome.
			 */
			std::error_code enroll(const std::uint64_t user_data, std::unique_ptr<transfer_record> record) noexcept
			{
				try
				{
					const std::scoped_lock lock(m_registry_mutex);
					const bool             added = m_records.emplace(user_data, std::move(record)).second;
					assert(added && "a token was issued twice");
					static_cast<void>(added);
				}
				catch(const std::bad_alloc&)
				{
					return bad_alloc_error();
				}
				return {};
			}

			/**
			 * Removes and returns the record registered under @p user_data, or null if none is.
			 *
			 * Taking it out under the mutex is what completes a record at most once. A user_data this
			 * backend does not recognize -- a completion for an operation already withdrawn, or one it
			 * never issued -- is a lookup that finds nothing, which is the whole reason the SQE carries
			 * the token value rather than the record's address.
			 */
			std::unique_ptr<transfer_record> withdraw(const std::uint64_t user_data) noexcept
			{
				const std::scoped_lock lock(m_registry_mutex);
				const auto             found = m_records.find(user_data);
				if(found == m_records.end())
				{
					return nullptr;
				}
				std::unique_ptr<transfer_record> record = std::move(found->second);
				m_records.erase(found);
				return record;
			}

			/**
			 * Marks the record @p user_data names as owing no callback, so that it stays registered and
			 * keeps its buffer's storage alive for a kernel write that may still land. The reaper frees
			 * it when the late completion arrives; the destructor leaks it if that has not happened,
			 * since it can neither free what the kernel may hold nor wait for a completion that may
			 * never come.
			 *
			 * **Precondition: m_submission_mutex is held.** This is not tidiness, it is the whole
			 * correctness of the quarantine. A published entry reaches the kernel only through an
			 * io_uring_submit(), every one of which holds that mutex, so holding it across this write
			 * makes installing the flag atomic with respect to any flush. Clearing the flag after
			 * releasing it instead leaves a window in which another thread's submission flushes the
			 * stale entry, the kernel completes it, and the reaper finds a record that still owes a
			 * callback and INVOKES it -- handing the caller an error and a callback both, which is the
			 * seam violation this path exists to avoid.
			 *
			 * The record stays in the SAME registry rather than moving to a container of its own. That
			 * costs a third m_records.find() site, here, and keeps the reaper's lookup to one lookup;
			 * it also makes the destructor's in-flight question ("is any callback still owed?") a
			 * predicate over the records it already walks instead of two emptiness checks whose
			 * difference a reader would have to reconstruct.
			 */
			void quarantine(const std::uint64_t user_data) noexcept
			{
				const std::scoped_lock lock(m_registry_mutex);
				const auto             found = m_records.find(user_data);
				// The record is still here. NOT because a refused submission produces no completion --
				// it may well produce one later, which is the reason this function exists -- but
				// because it cannot have produced one YET. Two facts, and both are needed: an entry
				// reaches the kernel only through an io_uring_submit() under the submission mutex this
				// call holds, and the one call that could have taken this entry consumed NOTHING,
				// because io_uring_submit reports an error only when it submitted zero SQEs (see
				// submit_to_ring()). So no completion for it exists at this instant, nothing can have
				// withdrawn it, and the window closes before the mutex is released.
				assert(found != m_records.end() && "a record was quarantined after it was withdrawn");
				if(found != m_records.end())
				{
					found->second->callback_is_owed = false;
				}
			}

			/**
			 * Puts @p submission on the ring and hands it to the kernel.
			 *
			 * Holds the submission mutex across both liburing calls, as post_wake_up() does and for the
			 * same reason: io_uring_get_sqe() and io_uring_submit() are not safe to call concurrently on
			 * a shared ring, and submit() may be called from any thread. Holding it across both is also
			 * what lets a submission the kernel ACCEPTS leave the queue empty again before the lock is
			 * released, which is why a ring of any depth can carry far more outstanding operations than
			 * it has entries. A refused submission is the exception, and the whole subject of the rest
			 * of this comment: it consumes nothing and its entry stays published, so the queue does not
			 * drain and this function must not be read as guaranteeing that it does.
			 *
			 * It never waits. submit() must not block, so neither failure is retried or waited out.
			 * **THE TWO FAILURES ARE NOT THE SAME, and the difference is memory safety rather than
			 * diagnostics.**
			 *
			 * A null io_uring_get_sqe() means the submission queue has no free entry. Nothing has been
			 * published, the kernel will never see this operation, and its record can be freed at
			 * once. Reported as EAGAIN.
			 *
			 * A refused io_uring_submit() is reported as the errno it returned, and the entry is
			 * ALREADY PUBLISHED: liburing advances the ring tail with a release store before it enters
			 * the kernel, so the entry stays in the ring and the next submission -- any later
			 * operation's, or the destructor's own wake-up NOP -- flushes it. The kernel may then
			 * perform this read or write. Freeing the record here would hand it a buffer that is gone
			 * to write into: a kernel write into freed heap, which this project has already recorded as
			 * invisible to AddressSanitizer AND ThreadSanitizer, so no gate would ever catch it.
			 *
			 * **A refused io_uring_submit() consumed NO entry, this one included**, and every
			 * interleaving argument below rests on that. io_uring_enter reports an error only when it
			 * submitted zero SQEs -- a call that submitted some and then failed returns the count, not
			 * the error -- and the EBUSY that makes this path reachable at all is raised before any
			 * submission is attempted. So at the moment of the refusal the entry is published and
			 * untouched by the kernel: no completion for it can exist, and none can until some later
			 * submission flushes it. (Do not read that back into the success path, whose own caveat is
			 * two paragraphs down: a non-negative return says the kernel took everything queued, not
			 * what was queued.)
			 *
			 * So this function QUARANTINES the record itself, before it returns and while it still
			 * holds the submission mutex, rather than leaving that to its caller. The flush that makes
			 * the entry dangerous needs the same mutex, so doing it here is what makes the quarantine
			 * atomic with respect to the flush; doing it after the return would leave a window in
			 * which the record still owes a callback while the kernel is completing it. entry_published
			 * is reported so the caller knows the record is no longer its to free, and for no other
			 * reason.
			 *
			 * **Neither failure is tested, and the reason differs.** A null SQE is unreachable while
			 * this lock is held across both calls AND every submission is consumed -- which is true
			 * until the first refused submit and false afterwards, since a refused submit leaves its
			 * entry unconsumed, and enough of them would fill the submission queue. That costs nothing:
			 * a null SQE published nothing whatever came before it, so its record is still safe to
			 * free. A
			 * refused submit is reachable in principle -- a kernel old enough to answer EBUSY on
			 * completion-queue overflow, which the capability check admits, reached by a caller that
			 * exceeds the max_in_flight it declared, since that is a contract and not an invariant this
			 * backend enforces -- but it cannot be produced on the kernel this was written against,
			 * which accepts a submission while the ring is overflown instead of refusing it (measured;
			 * see derive_ring_sizes()). So the quarantine is correctness by argument, not by test, and
			 * the argument is the only safety net there is. What that leaves open -- no test, and the
			 * destructor's leak below -- is issue #72.
			 *
			 * One thing a reader should not infer from the success path: a non-negative
			 * io_uring_submit() means the kernel took everything queued, but NOT that what it took was
			 * only this entry. That was true until the first refused submission, and is permanently
			 * false afterwards, because an unconsumed published entry rides along with every later
			 * submission. Nothing here depends on the count, which is why only the sign is read.
			 */
			submission_outcome submit_to_ring(const transfer_submission& submission) noexcept
			{
				const std::scoped_lock lock(m_submission_mutex);
				io_uring_sqe* const    sqe = io_uring_get_sqe(&m_ring);
				if(sqe == nullptr)
				{
					return submission_outcome{.error = errno_error(EAGAIN), .entry_published = false};
				}
				if(submission.is_write)
				{
					io_uring_prep_write(sqe, submission.fd, submission.data, submission.count, submission.offset);
				}
				else
				{
					io_uring_prep_read(sqe, submission.fd, submission.data, submission.count, submission.offset);
				}
				// Assigned directly rather than through io_uring_sqe_set_data64(), for the reason
				// post_wake_up() gives: the 2.1 floor does not promise that helper. io_uring_prep_read,
				// io_uring_prep_write and io_uring_prep_nop are all older than it.
				sqe->user_data      = submission.user_data;
				const int submitted = io_uring_submit(&m_ring);
				if(submitted < 0)
				{
					// Still holding m_submission_mutex, and that is the point: the flag has to be
					// installed before any other thread can submit, because any other thread's
					// submission flushes this entry too. See quarantine().
					quarantine(submission.user_data);
					return submission_outcome{.error = errno_error(-submitted), .entry_published = true};
				}
				return {};
			}

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
			 * ring, and submit() may be called from any thread. This is itself one of the two arms
			 * that take it, and it takes nothing else, so it cannot be a party to a lock-order
			 * problem; the order that does exist, submission then registry, is on m_registry_mutex.
			 * (This comment used to say the reaper takes no lock of this backend's at all. The reaper
			 * takes the registry mutex through withdraw(), and has since records existed.)
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
					// Both fields are read before io_uring_cqe_seen(), which hands the entry back to the
					// kernel: after it, that memory may already hold another completion.
					const std::uint64_t user_data = cqe->user_data;
					const int           res       = cqe->res;
					io_uring_cqe_seen(&m_ring, cqe);
					if(user_data == wake_up_user_data)
					{
						return;
					}
					std::unique_ptr<transfer_record> record = withdraw(user_data);
					if(record && !record->callback_is_owed)
					{
						// A quarantined record's completion: its submission was refused, its caller
						// already has an error, and its callback must never run. All that was owed was
						// the storage, and this scope's end frees it now that the kernel is done with
						// it. Counted with the completions below, under the meaning m_ignored carries
						// -- a completion delivered to nobody -- which this branch is half of. A cancel
						// SQE's own completion is NOT that, and needs a branch above both rather than
						// a share of this counter: issue #73.
						m_ignored.fetch_add(1);
						continue;
					}
					if(!record)
					{
						// A completion with no operation behind it: counted, never acted on, and above all
						// never dereferenced. user_data carries the token value rather than a record
						// pointer precisely so that a stale completion can be looked for and found absent
						// instead of pointing at a record that is gone.
						m_ignored.fetch_add(1);
						continue;
					}
					// Outside withdraw()'s lock, which it has already released, and under no lock of this
					// backend's at all: the callback is the file layer's operation block's entry point and
					// takes locks of its own, and it may call straight back into submit(). The record, and
					// with it the io_buffer copy the kernel has been writing into, is released when this
					// statement's unique_ptr goes out of scope -- after the callback has RETURNED, which is
					// what io_request documents.
					record->on_done(record->state, completion_of(res));
				}
			}

			std::unique_ptr<io_backend> m_pool_backend;

			io_uring m_ring{};
			/// Whether m_ring was created, and so whether the destructor must tear it down.
			bool m_ring_created = false;

			/**
			 * Serializes every submission to m_ring; liburing does not make a shared ring thread-safe.
			 *
			 * The OUTER of the two mutexes. See m_registry_mutex for the order and why it holds.
			 */
			std::mutex m_submission_mutex;

			/**
			 * Serializes the in-flight registry below.
			 *
			 * **Lock order: m_submission_mutex then m_registry_mutex, never the reverse.** One place
			 * nests them -- submit_to_ring()'s refusal path, which quarantines a record while still
			 * holding the submission mutex, because the flush it is racing needs that same mutex and
			 * the flag has to be installed before another thread can perform one. Everything else
			 * takes one at a time: enroll() takes this and releases it before the submission mutex is
			 * taken, withdraw() takes only this, and the reaper's own loop takes only this. The reaper
			 * THREAD does reach the submission mutex -- a completion callback may call submit(), which
			 * the suite pins -- but only once withdraw() has released this one, which is the point: no
			 * path anywhere takes the submission mutex while holding this one, and that is what makes
			 * the nesting deadlock-free rather than merely untested.
			 *
			 * This comment said "never held at the same time" until the quarantine made that false.
			 *
			 * **A warning for the cancellation arm, which is where that invariant was expected to come
			 * due.** The spec and the plan both describe cancel as submitting an ASYNC_CANCEL *under
			 * the registry lock*, which is the OPPOSITE nesting -- registry, then submission -- and
			 * against the order above that is a lock-order inversion, so the two together deadlock:
			 * one thread in submit_to_ring()'s refusal path holding submission and waiting for
			 * registry, one in cancel() holding registry and waiting for submission. Either cancel
			 * takes the submission mutex first, or it copies what it needs out of the registry,
			 * releases this mutex and submits afterwards -- which also keeps it off the path of
			 * holding a lock across a liburing call. The registry lookup's purpose, not racing a
			 * completion that is already withdrawing the record, survives either way. Issue #75
			 * records the discovery; the spec and the plan have since been corrected to this order,
			 * so they and this comment now agree.
			 */
			std::mutex m_registry_mutex;

			/**
			 * Every record the kernel may still be holding, keyed by the token value its SQE carries
			 * as its user_data: an operation in flight, or a quarantined record whose caller already
			 * has an error and whose storage must outlive a write that may still land.
			 *
			 * This is the map the reaper looks a completion up in, and the one the destructor asks its
			 * in-flight question of -- which is transfer_record::callback_is_owed over these entries,
			 * not this map's emptiness, since a quarantined entry is present and owes nothing. Nothing
			 * here is addressed by pointer from outside it.
			 */
			std::unordered_map<std::uint64_t, std::unique_ptr<transfer_record>> m_records;

			/// The next operation id; see first_operation_id for why it does not start at one.
			std::atomic<std::uint64_t> m_next_id{first_operation_id};

			/// The ring sizes in effect, as the kernel reported them from create_ring().
			std::size_t m_submission_depth = 0;
			std::size_t m_completion_depth = 0;

			/**
			 * Completions the reaper delivered to nobody.
			 *
			 * Two kinds reach it, and the second arrived with the quarantine: a completion whose
			 * user_data is in no registry at all -- stale, or never issued here -- and a completion
			 * whose record WAS found but owes no callback, because its submission was refused and its
			 * caller already has an error. What they have in common, and what this counter means, is
			 * that no caller's callback ran for them.
			 *
			 * What it must not grow to include is this backend's own bookkeeping. The wake-up NOP is
			 * excluded by the branch above the lookup, and a cancel SQE's own completion belongs in
			 * that same branch rather than here: it is not a completion delivered to nobody, it is not
			 * a completion of an operation at all. Issue #73.
			 */
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

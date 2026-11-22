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

	io_uring_operation io_uring_operation_of(const io_op op) noexcept
	{
		switch(op)
		{
		case io_op::read:
			return io_uring_operation::read;
		case io_op::write:
			return io_uring_operation::write;
		case io_op::sync_full:
			return io_uring_operation::fsync;
		case io_op::sync_data:
			return io_uring_operation::fdatasync;
		// Listed rather than defaulted, so that a new io_op is a -Wswitch warning here instead of
		// silently becoming a read. None of these is ever submitted to the ring.
		case io_op::stat_handle:
		case io_op::stat_path:
		case io_op::open:
		case io_op::rename:
		case io_op::unlink:
		case io_op::list_directory:
		case io_op::invoke:
			break;
		}
		assert(false && "io_uring_operation_of: an operation this backend does not put on the ring");
		return io_uring_operation::read;
	}

	unsigned io_uring_fsync_flags_of(const io_uring_operation operation) noexcept
	{
		return operation == io_uring_operation::fdatasync ? IORING_FSYNC_DATASYNC : 0U;
	}

	bool io_uring_submission_was_consumed(const io_uring_submit_counts& counts) noexcept
	{
		// `> 0` rather than `>= 0` is what carries the negated-errno case as well: no offered count
		// a negative return could equal, so the comparison below never has to see one. The equality
		// is the real content -- the caller's entry is the last of the offered ones, so it was taken
		// only if all of them were. See the declaration for why a lesser count is not success.
		return counts.submitted > 0 && static_cast<unsigned>(counts.submitted) == counts.offered;
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
		 * The user_data of the IORING_OP_ASYNC_CANCEL entries cancel() submits.
		 *
		 * The ring tag with an operation id of one, which first_operation_id below keeps free. A
		 * cancel entry's own completion is this backend's bookkeeping and not any operation's
		 * outcome: the operation the cancel names has a completion of its own, and that one is what
		 * runs the callback. So the reaper has to be able to tell the two apart, which is what this
		 * reservation is for.
		 *
		 * Every cancel carries the same value. Nothing is ever looked up by it, and a cancel owns no
		 * record and no buffer that a distinct id would have to reach.
		 */
		constexpr std::uint64_t cancel_user_data = io_uring_token_tag | 1;

		/**
		 * The first operation id this backend issues, which is 2 rather than 1.
		 *
		 * Two values of the tagged space belong to no operation: zero is the wake-up NOP's and one
		 * is the cancel entries', both above. Starting the ids past them is what keeps both free.
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
		 * the entry and submitting it, so **a submission the kernel CONSUMES** leaves the queue
		 * empty again before the lock is released: a full submission queue is all but unreachable
		 * at any depth, which is what makes the depth the caller's to choose freely.
		 *
		 * "Consumes" is the load-bearing word, and it is not decoration. io_uring_submit returns a
		 * COUNT of the SQEs the kernel took, which may be an error, a zero, or fewer than were
		 * offered; any of the three leaves this submission's entry published in the ring, so
		 * unconsumed entries
		 * accumulate at one per such call and enough of them would fill the queue after all. That
		 * does not weaken the depth argument much -- the queue then hands back a null SQE, which
		 * publishes nothing and whose record is safe to free, and submit() reports EAGAIN -- but
		 * the claim is "until the first unconsumed submission", not "never". submit_to_ring()
		 * carries the rest, and io_uring_submission_was_consumed() is where the three are made one
		 * case.
		 *
		 * What actually bounds outstanding work is the completion ring, which defaults to twice
		 * the submission ring and, on overflow, makes io_uring_submit fail or forces the kernel to
		 * hold completions in a backlog. So it is sized from max_in_flight instead, plus the
		 * reserved internal completions -- the wake-up NOP, and one cancel for each operation that
		 * can be in flight at once -- and never below the submission ring, which the kernel
		 * requires. Completion-queue overflow is then unreachable by construction rather than by
		 * assumption, whatever depth the caller passes.
		 *
		 * **"One cancel for each operation" is an ASSUMPTION about the caller, and the only one this
		 * arithmetic makes.** The seam's cancel() is a hint with no once-only rule written into it,
		 * and each call on a still-pending token submits another IORING_OP_ASYNC_CANCEL with a
		 * completion of its own, so a caller that cancels the same operation twice spends a
		 * reserved entry this sizing did not reserve. The library's only caller cannot:
		 * src/io_operation.cpp's stop_handler returns without cancelling unless the state is
		 * phase::waiting, sets phase::detached before it submits, and is driven by a
		 * std::stop_callback, which fires once. So the bound holds for this library -- but it holds
		 * BECAUSE of that caller and not by construction, which is why it is stated here rather
		 * than left for a future caller to discover. Breaking it costs what exceeding the 32767
		 * bound below costs and nothing worse: overflow becomes possible, and the NODROP backlog
		 * and the reaper's drain keep it safe.
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
		 * it is the one reachable producer of a NEGATIVE io_uring_submit(), and therefore one of the
		 * two routes to submit_to_ring()'s published-entry path and the quarantine that path exists
		 * for. A caller
		 * that exceeds the max_in_flight it declared reaches overflow at any depth, since that bound
		 * is a contract and not something this backend enforces. The other route is a short submit,
		 * which needs no overflow at all: a kernel that cannot allocate an io_kiocb takes fewer SQEs
		 * than it was offered and reports the count, not an error. See
		 * io_uring_submission_was_consumed().
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
		 * One read, write or fsync on the ring, owned by the backend from submission until the reaper has
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
			 * False for a QUARANTINED record: one whose io_uring_submit() did not take its entry --
			 * refused, or short of what it was offered -- after liburing had already published that
			 * entry to the kernel. The caller has been handed an
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
			std::uint64_t      user_data = 0;
			int                fd        = -1;
			void*              data      = nullptr;
			unsigned           count     = 0;
			std::uint64_t      offset    = 0;
			io_uring_operation kind      = io_uring_operation::read;
		};

		/**
		 * What a failed submission did before it failed, which is what decides the record's fate.
		 *
		 * The two ways submit_to_ring() can fail are NOT interchangeable, and merging them into one
		 * std::error_code is what made an earlier version of this file unsafe. A null SQE publishes
		 * nothing, so its record can be freed. An io_uring_submit() that did not take the entry --
		 * refused, or short of what it was offered -- has already had the ring
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
					// this submission may flush an entry an earlier submission did not take -- a
					// quarantined operation's, or a cancel's -- and the reaper may see that completion
					// before or after the NOP's. Either order is fine: a quarantined completion runs no
					// callback and frees its record, a cancel's is discarded, and the NOP returns the
					// reaper whenever it arrives. So what the wake-up guarantees is that the reaper
					// stops, not that it stops with nothing else to see.
					//
					// The terminate below hinges on post_wake_up()'s answer, so that answer has to
					// be about the NOP and not about the call. It is: post_wake_up() compares the
					// count the kernel took against the count it was offered, so an entry an earlier
					// submission left published cannot satisfy it on the NOP's behalf. Reading only
					// the sign would have made this a HANG rather than a terminate -- the destructor
					// would conclude the reaper was woken and join a thread still blocked in
					// io_uring_wait_cqe.
					if(!post_wake_up())
					{
						assert(false && "the io_uring backend's wake-up NOP was not taken, so the reaper cannot be joined");
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
				// -- so the kernel either holds that operation or has already completed it. A
				// completion that arrived before the NOP's was reaped and freed normally; these are the
				// ones that did not.
				//
				// They are RELEASED rather than freed: a deliberate, bounded leak of one record and its
				// buffer per unconsumed submission. Freeing them is the use-after-free the quarantine
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
				if(request.op == io_op::read || request.op == io_op::write || request.op == io_op::sync_full || request.op == io_op::sync_data)
				{
					return submit_transfer(request, on_done, state);
				}
				// Everything else is forwarded, and none of the seven can be anything else. open,
				// the two stats, rename and unlink have opcodes that arrived across kernels 5.6 to 5.11
				// and would each need their own probe and fallback for no latency anyone is waiting on; a
				// directory listing has no opcode at all, since getdents is not exposed; and invoke runs
				// an arbitrary callable, which a submission queue has nowhere to run.
				return submit_forwarded(request, on_done, state);
			}

			/**
			 * Asks the kernel to abandon @p token's operation. A HINT, as the seam says: the operation
			 * completes either way, and that completion is what runs the callback.
			 *
			 * The tag is what routes it. A pool token never carries it, which submit_forwarded()
			 * asserts, and every token submit_transfer() issues does.
			 *
			 * **The registry lock is released before the entry is submitted, and that order is not
			 * negotiable.** submit_to_ring()'s quarantine path takes the registry lock while HOLDING the
			 * submission mutex, so this backend's one lock order is submission then registry.
			 * Submitting from inside the registry lock would be the reverse, and the two together
			 * deadlock: one thread holding submission and waiting for registry, one holding registry
			 * and waiting for submission. The spec and the plan both used to prescribe that nesting;
			 * issue #75 records the discovery, which was made before any cancel existed.
			 *
			 * Releasing first costs nothing. If the operation completes in the window between the two,
			 * the entry names a user_data the kernel no longer knows and completes with -ENOENT or
			 * -EALREADY, which reap() discards. It cannot name a DIFFERENT, live operation, because ids
			 * come from an ever-increasing counter and are never recycled -- without that, "the kernel
			 * no longer knows that user_data" would be an assumption about id reuse rather than a fact.
			 * A stale cancel is an ineffective cancel, which is precisely what a hint permits, and a
			 * lock held to prevent it would buy nothing at the price of the deadlock above.
			 *
			 * The lookup is still worth its own line: it keeps this from asking the kernel about an
			 * operation that was never pending here at all. Its predicate is callback_is_owed and not
			 * mere presence, because a quarantined record is present and owes nothing -- a cancel for
			 * one would name an entry whose caller already has an error.
			 */
			void cancel(io_token token) noexcept override
			{
				if(!token)
				{
					return;
				}
				if((token.value() & io_uring_token_tag) == 0)
				{
					m_pool_backend->cancel(token);
					return;
				}
				if(!callback_is_owed_for(token.value()))
				{
					return;
				}
				post_cancel(token.value());
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
			 * Submits @p request to the ring as one read, write or fsync SQE.
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

				const io_uring_operation  operation = io_uring_operation_of(request.op);
				const transfer_submission submission{.user_data = io_uring_token_tag | m_next_id.fetch_add(1),
				                                     .fd        = to_fd(request.handle),
				                                     .data      = owned->buffer.bytes().data(),
				                                     .count     = transfer_count(request),
				                                     .offset    = request.offset,
				                                     .kind      = operation};

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
			 * Whether the record @p user_data names is registered AND still owes its callback.
			 *
			 * The pending predicate cancel() asks before it submits anything. Presence alone is the
			 * wrong question: a quarantined record is present and owes nothing, so a bare lookup would
			 * have cancel() submit a pointless entry for one.
			 *
			 * The answer is read out and this lock released, rather than held while the caller acts on
			 * it, because acting on it means a submission and this backend's lock order forbids that
			 * nesting. See cancel(), and m_registry_mutex.
			 *
			 * It is therefore a SNAPSHOT, deliberately: by the time the caller submits, the operation
			 * may have completed. That costs an ineffective cancel and nothing else.
			 *
			 * **Correctness by argument, not by test, and measured as such.** Weakening this to mere
			 * presence passes the whole suite, and so does deleting the call site: telling the two
			 * apart needs a quarantined record, which needs an io_uring_submit() that does not take
			 * its entry and that no kernel this project builds on produces, by either route (issue
			 * #72), and a cancel submitted for a user_data the
			 * kernel does not know is discarded by reap() without a trace. What this buys is a
			 * pointless entry kept off the ring; what it does not buy is a failing test.
			 */
			[[nodiscard]] bool callback_is_owed_for(const std::uint64_t user_data) noexcept
			{
				const std::scoped_lock lock(m_registry_mutex);
				const auto             found = m_records.find(user_data);
				return found != m_records.end() && found->second->callback_is_owed;
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
				// The record is still here. NOT because an unconsumed submission produces no completion
				// -- it may well produce one later, which is the reason this function exists -- but
				// because it cannot have produced one YET. Two facts, and both are needed: an entry
				// reaches the kernel only through an io_uring_submit() under the submission mutex this
				// call holds, and the one call that could have taken this entry DID NOT take it, which
				// is precisely what io_uring_submission_was_consumed() answered false to (see
				// submit_to_ring()). Note what is not claimed: that the call consumed nothing. It may
				// have taken entries ahead of this one -- a short submit -- and that is immaterial,
				// because the kernel submits in ring order and this entry is the last of them. So no
				// completion for it exists at this instant, nothing can have
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
			 * what lets a submission the kernel CONSUMES leave the queue empty again before the lock is
			 * released, which is why a ring of any depth can carry far more outstanding operations than
			 * it has entries. An unconsumed submission is the exception, and the whole subject of the
			 * rest of this comment: its entry stays published, so the queue does not drain and this
			 * function must not be read as guaranteeing that it does.
			 *
			 * It never waits. submit() must not block, so neither failure is retried or waited out.
			 * **THE TWO FAILURES ARE NOT THE SAME, and the difference is memory safety rather than
			 * diagnostics.**
			 *
			 * A null io_uring_get_sqe() means the submission queue has no free entry. Nothing has been
			 * published, the kernel will never see this operation, and its record can be freed at
			 * once. Reported as EAGAIN.
			 *
			 * **A submission the kernel did not TAKE is the second failure, and io_uring_submit()'s
			 * return value is a count and not a verdict.** It reports how many SQEs the kernel
			 * consumed, which may be fewer than were offered -- zero with one entry offered -- so the
			 * decision is io_uring_submission_was_consumed() over that count and the count
			 * io_uring_sq_ready() reported just before the call, never the sign alone. Its declaration
			 * carries the kernel and liburing evidence; what matters here is that a refused submit, a
			 * zero and a short count are ONE case to this function, because in all three the entry
			 * this call prepared was not taken.
			 *
			 * In that case the entry is ALREADY PUBLISHED: liburing advances the ring tail with a
			 * release store before it enters the kernel, so the entry stays in the ring and the next
			 * submission -- any later operation's, any cancel's, or the destructor's own wake-up NOP --
			 * flushes it. The kernel may then perform this operation. For a read or write, freeing the
			 * record here would hand it a
			 * buffer that is gone to write into: a kernel write into freed heap, which this project
			 * has already recorded as invisible to AddressSanitizer AND ThreadSanitizer, so no gate
			 * would ever catch it. An fsync's record holds no buffer, so freeing that one would be
			 * harmless; it is quarantined all the same, because nothing on this path asks which
			 * operation it is and a second rule for one kind would be a second path to get wrong.
			 *
			 * It is reported as the errno io_uring_submit() returned, or as EAGAIN where it returned a
			 * count instead of an error: errno_error(0) is a falsy error_code and would send the
			 * caller down its success arm with no token at all.
			 *
			 * **An unconsumed submission did NOT take this entry**, and every interleaving argument
			 * below rests on that -- on exactly that, and not on the stronger claim that the call
			 * consumed nothing. io_uring_enter submits in ring order, and this entry is the last of
			 * the offered ones because io_uring_get_sqe() takes from the tail, so a count short of
			 * what was offered leaves this entry where it was however many entries ahead of it the
			 * kernel took. So at the moment of the failure the entry is published and untouched by
			 * the kernel: no completion for it can exist, and none can until some later submission
			 * flushes it.
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
			 * until the first unconsumed submit and false afterwards, since such a submit leaves its
			 * entry behind, and enough of them would fill the submission queue. That costs nothing:
			 * a null SQE published nothing whatever came before it, so its record is still safe to
			 * free. An unconsumed submit is reachable in principle, by either of its two routes -- a
			 * kernel old enough to answer EBUSY on
			 * completion-queue overflow, which the capability check admits, reached by a caller that
			 * exceeds the max_in_flight it declared, since that is a contract and not an invariant this
			 * backend enforces; or a kernel that cannot allocate an io_kiocb and takes fewer SQEs than
			 * it was offered -- but neither can be produced on the kernel this was written against,
			 * which accepts a submission while the ring is overflown instead of refusing it (measured;
			 * see derive_ring_sizes()) and whose allocation failure a test cannot arrange. So the
			 * quarantine is correctness by argument, not by test, and the argument is the only safety
			 * net there is. What that leaves open -- no test of this path, and the destructor's leak
			 * below -- is issue #72. The one piece that IS tested is the decision itself:
			 * io_uring_submission_was_consumed() is a pure function and the suite asserts it over
			 * every shape of (submitted, offered), which is why the sign-only reading this comment
			 * used to defend cannot come back unnoticed.
			 *
			 * One thing a reader should not infer from the success path: a count equal to what was
			 * offered means the kernel took everything queued, but NOT that what it took was only this
			 * entry. That was true until the first unconsumed submission, and is permanently false
			 * afterwards, because an entry left published rides along with every later submission.
			 * Nothing here depends on which of them the kernel took, only on its having taken all of
			 * them, which is what makes the equality the right question.
			 */
			submission_outcome submit_to_ring(const transfer_submission& submission) noexcept
			{
				const std::scoped_lock lock(m_submission_mutex);
				io_uring_sqe* const    sqe = io_uring_get_sqe(&m_ring);
				if(sqe == nullptr)
				{
					return submission_outcome{.error = errno_error(EAGAIN), .entry_published = false};
				}
				switch(submission.kind)
				{
				case io_uring_operation::write:
					io_uring_prep_write(sqe, submission.fd, submission.data, submission.count, submission.offset);
					break;
				case io_uring_operation::read:
					io_uring_prep_read(sqe, submission.fd, submission.data, submission.count, submission.offset);
					break;
				case io_uring_operation::fsync:
				case io_uring_operation::fdatasync:
					io_uring_prep_fsync(sqe, submission.fd, io_uring_fsync_flags_of(submission.kind));
					break;
				}
				// Assigned directly rather than through io_uring_sqe_set_data64(), for the reason
				// post_wake_up() gives: the 2.1 floor does not promise that helper. io_uring_prep_read,
				// io_uring_prep_write, io_uring_prep_fsync and io_uring_prep_nop are all older than it.
				sqe->user_data = submission.user_data;
				// Read after the entry is at the tail and with the submission mutex still held, so it
				// counts this entry and nothing can be queued between this call and the submit below.
				// io_uring_sq_ready() is exactly the count liburing will hand io_uring_enter as its
				// to_submit: sqe_tail minus the kernel's real head.
				const unsigned offered   = io_uring_sq_ready(&m_ring);
				const int      submitted = io_uring_submit(&m_ring);
				if(!io_uring_submission_was_consumed(io_uring_submit_counts{.submitted = submitted, .offered = offered}))
				{
					// Still holding m_submission_mutex, and that is the point: the flag has to be
					// installed before any other thread can submit, because any other thread's
					// submission flushes this entry too. See quarantine().
					quarantine(submission.user_data);
					// A refused submit reports the errno it returned. A submit that merely did not
					// take this entry -- a zero, or a short count -- returned no errno at all, and
					// errno_error(0) is a FALSY std::error_code, which would send the caller down its
					// success arm holding no token. EAGAIN stands in: nothing about the condition is
					// permanent and the operation was not performed, which is what the null-SQE arm
					// above already reports EAGAIN for. (EBUSY would read as "the ring is busy", which
					// names a cause this code cannot distinguish from a kernel allocation failure.)
					const std::error_code error = submitted < 0 ? errno_error(-submitted) : errno_error(EAGAIN);
					return submission_outcome{.error = error, .entry_published = true};
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
			 * @return Whether the kernel TOOK the NOP. False means the reaper will not wake, and the
			 *         destructor stops the process rather than hang in join(). The count is compared
			 *         against what was offered rather than merely tested for a positive value: a
			 *         published entry left behind by an earlier unconsumed submission rides along
			 *         with this one, so "something was consumed" does not say the NOP was. See
			 *         io_uring_submission_was_consumed().
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
				// Read before the submit, and with the submission mutex held, so that it counts this
				// NOP along with anything an earlier unconsumed submission left published.
				const unsigned offered   = io_uring_sq_ready(&m_ring);
				const int      submitted = io_uring_submit(&m_ring);
				return io_uring_submission_was_consumed(io_uring_submit_counts{.submitted = submitted, .offered = offered});
			}

			/**
			 * Submits one IORING_OP_ASYNC_CANCEL naming @p user_data. Called by cancel(), with no
			 * registry lock held -- which is the whole subject of cancel()'s own comment.
			 *
			 * Holds the submission mutex across both liburing calls, as every submission to this ring
			 * must: io_uring_get_sqe() and io_uring_submit() are not safe to call concurrently on a
			 * shared ring, and cancel() may be called from any thread.
			 *
			 * **io_uring_prep_cancel(), never io_uring_prep_cancel64().** The 64-bit form is liburing
			 * 2.2 and this project's floor is 2.1. No gate here exercises that floor -- this machine
			 * and CI's runner both carry a much later liburing -- so the newer helper would compile
			 * and pass everywhere and fail only for a consumer on Ubuntu 22.04 or Debian 12.
			 *
			 * 2.1's io_uring_prep_cancel() takes the target as a POINTER, which is what the 64-bit
			 * form was added for. Every user_data of this backend's has bit 63 set, so routing one
			 * through a void* is not an option: where a pointer is narrower than 64 bits that
			 * conversion does not even compile, and nothing here is a pointer to begin with. So the
			 * entry is prepared with a null target and the field is then assigned the way
			 * io_uring_prep_cancel64() assigns it -- sqe->addr IS the target user_data, in every
			 * liburing from 2.1 onward -- rather than by casting an integer to a pointer and back.
			 *
			 * **Failure is silence**, which the seam permits: cancel is a hint and must not block, so
			 * there is nothing to retry and nobody to report to. A null entry publishes nothing. An
			 * io_uring_submit() that does not take the entry -- refused, zero, or short -- leaves it
			 * published, and some later submission may
			 * flush it at an arbitrary time -- which for a cancel is harmless rather than merely
			 * tolerated: the entry owns no record and no buffer, so there is nothing to quarantine
			 * and nothing the kernel can write into after the fact, and the user_data it names cannot
			 * meanwhile have been given to a different operation. That is why submit_to_ring()'s
			 * quarantine has no counterpart here.
			 */
			void post_cancel(const std::uint64_t user_data) noexcept
			{
				const std::scoped_lock lock(m_submission_mutex);
				io_uring_sqe* const    sqe = io_uring_get_sqe(&m_ring);
				if(sqe == nullptr)
				{
					return;
				}
				io_uring_prep_cancel(sqe, nullptr, 0);
				// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): sqe->addr is a member of an anonymous union in the kernel's own submission entry, and for IORING_OP_ASYNC_CANCEL it IS the target user_data. This is the assignment io_uring_prep_cancel64() makes, spelled out here because that helper is liburing 2.2 and this project's floor is 2.1. The check's advice does not apply: there is no variant to use instead of a kernel ABI struct, the only alternative within 2.1 is io_uring_prep_cancel()'s POINTER parameter -- which cannot carry a user_data with bit 63 set where a pointer is narrower than 64 bits -- and every liburing helper this file already calls writes the same union through the same field.
				sqe->addr = user_data;
				// Assigned directly rather than through io_uring_sqe_set_data64(), for the reason
				// post_wake_up() gives at length: the 2.1 floor does not promise that helper.
				sqe->user_data = cancel_user_data;
				// Neither the count nor the sign is read, and unlike every other submission here this
				// one asks io_uring_submission_was_consumed() nothing: see above. An entry this call
				// leaves published is harmless whenever a later submission flushes it, so there is
				// nothing for a verdict to decide.
				static_cast<void>(io_uring_submit(&m_ring));
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
					if(user_data == cancel_user_data)
					{
						// A cancel entry's own completion: 0 where the kernel abandoned the operation
						// the entry named, -ENOENT or -EALREADY where that operation had already
						// completed or was past being stopped. Nothing to deliver either way -- the
						// operation has, or will have, a completion of its own, and that is the one
						// that runs the callback -- and nothing to COUNT: m_ignored means "a completion
						// the reaper delivered to nobody", which is a statement about operations, and
						// this is not the completion of an operation at all. Discarded here, above the
						// registry lookup, exactly as the wake-up NOP is, and for the same reason: it
						// is this backend's own bookkeeping. res is deliberately unread; it says only
						// who won a race the caller cannot observe.
						continue;
					}
					std::unique_ptr<transfer_record> record = withdraw(user_data);
					if(record && !record->callback_is_owed)
					{
						// A quarantined record's completion: the kernel never took its submission, its caller
						// already has an error, and its callback must never run. All that was owed was
						// the storage, and this scope's end frees it now that the kernel is done with
						// it. Counted with the completions below, under the meaning m_ignored carries
						// -- a completion delivered to nobody -- which this branch is half of. A cancel
						// entry's own completion is NOT that, and is discarded in the branch above both
						// rather than given a share of this counter.
						m_ignored.fetch_add(1);
						continue;
					}
					if(!record)
					{
						// A completion with no operation behind it: counted, never acted on, and above all
						// never dereferenced. user_data carries the token value rather than a record
						// pointer precisely so that a stale completion can be looked for and found absent
						// instead of pointing at a record that is gone.
						//
						// **This branch is a MANDATORY null check and not a counter's arrangement.**
						// withdraw() returns a std::unique_ptr and documents null as a result, and the
						// statement below this block dereferences it, so deleting the branch is an
						// unconditional null dereference on every completion. Only the fetch_add is
						// unexercisable -- m_ignored's own comment says why no test can make it
						// nonzero -- and that is a fact about the COUNTER, not about the guard. A
						// reader who concludes from "nothing reaches it" that the `if` can go has
						// confused the two.
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
			 * nests them -- submit_to_ring()'s quarantine path, which marks a record while still
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
			 * **The cancellation arm is where that invariant was expected to come due, and it does
			 * not.** cancel() asks callback_is_owed_for() its question, RELEASES this mutex, and only
			 * then submits its IORING_OP_ASYNC_CANCEL under the submission mutex -- so it takes one at
			 * a time like everything else, and holds no lock across a liburing call. The spec and the
			 * plan both used to describe it as submitting *under the registry lock*, which is the
			 * opposite nesting and deadlocks against the quarantine path above: one thread in
			 * submit_to_ring() holding submission and waiting for registry, one in cancel() holding
			 * registry and waiting for submission. Issue #75 records the discovery, made before any
			 * cancel existed; the spec, the plan, this comment and the code now all agree on the one
			 * order.
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
			 * whose record WAS found but owes no callback, because the kernel did not take its
			 * submission and its caller already has an error. What they have in common, and what this counter means, is
			 * that no caller's callback ran for them.
			 *
			 * What it does not include is this backend's own bookkeeping, and both kinds of that are
			 * excluded by branches above the lookup: the wake-up NOP, and a cancel entry's own
			 * completion, which is not a completion delivered to nobody because it is not the
			 * completion of an operation at all.
			 *
			 * **Nothing in the suite makes it nonzero**, and that is a property of the counter rather
			 * than a gap in the tests. Of its two producers, a quarantined record's late completion
			 * needs an io_uring_submit() that does not take its entry, which cannot be produced on any
			 * kernel this builds on by either of its two routes (issue #72), and a genuinely stale
			 * completion cannot be produced at all: a record is
			 * registered before its entry reaches the kernel and withdrawn only by the completion
			 * itself, so no completion can arrive for a user_data this registry has forgotten. The
			 * tests assert it stays zero, which is what they can assert and what the cancellation arm
			 * would have broken.
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

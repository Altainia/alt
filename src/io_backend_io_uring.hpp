#pragma once

#ifdef __linux__

#	include <alt/io_backend.hpp>
#	include <alt/thread_pool.hpp>
#	include <cstddef>
#	include <cstdint>
#	include <expected>
#	include <memory>
#	include <system_error>

/**
 * The kernel's IORING_REGISTER_PROBE result, as liburing's own headers declare it.
 *
 * Named by forward declaration so this header stays free of liburing.h, exactly as
 * io_backend_iocp.hpp keeps windows.h out of include/alt/. A caller that needs the definition --
 * the suite, which synthesizes one -- includes <liburing/io_uring.h>, the kernel ABI header that
 * declares it and nothing else.
 */
struct io_uring_probe;

/**
 * The io_uring backend. Private to the library: only file_io::create builds one, and the tests
 * include this header to reach it. A consumer has no reason to, and cannot through
 * include/alt/, which never names liburing.h.
 */
namespace alt::detail
{
	/// Set on every token for a ring operation; a token without it came from the internal pool backend.
	inline constexpr std::uint64_t io_uring_token_tag = std::uint64_t{1} << 63U;

	/**
	 * Builds the backend that performs transfers through an io_uring submission ring, and every
	 * other operation on @p pool, which is borrowed.
	 *
	 * It owns the ring and the one thread that reaps completions from it; a completion's callback
	 * runs on that thread, or on a pool worker for an operation forwarded there.
	 *
	 * **Unlike the IOCP backend, this one needs nothing special of a descriptor.** io_uring
	 * operates on ordinary file descriptors, so the internal pool backend is built with the plain
	 * alt::make_thread_pool_backend(): no detail::pool_open_policy, no prepare hook, and no extra
	 * open flags. A reader arriving from src/io_backend_iocp.cpp will look for that machinery and
	 * will not find it, because IOCP needs FILE_FLAG_OVERLAPPED and association with its port
	 * while a ring needs neither. Descriptors that arrived from outside, through
	 * alt::async_file::adopt(), are as usable here as ones this service opened itself.
	 *
	 * @p queue_depth is the submission ring size, or 0 to derive it from @p max_in_flight; it is
	 * rounded up to a power of two and clamped to what the kernel allows. The completion ring is
	 * sized separately, from @p max_in_flight plus the reserved internal completions -- the
	 * wake-up NOP, and **one cancel per in-flight operation, which assumes a caller that does not
	 * cancel the same token twice** -- so
	 * completion-queue overflow is unreachable whatever depth the caller asks for
	 * -- **for @p max_in_flight up to and including 32767.** Above that the bound cannot be met:
	 * covering it needs `2 * max_in_flight + 1` completion entries and the kernel's ceiling is
	 * 65536, so the ring is clamped to that ceiling and overflow becomes possible. It is still
	 * safe. Every kernel that passes the capability check below is 5.6 or newer, hence sets
	 * IORING_FEAT_NODROP, under which the kernel holds overflowing completions in a backlog
	 * rather than dropping them, and the reaper's wait drains that backlog -- measured against a
	 * deliberately overflowed ring, not assumed; see derive_ring_sizes() in the implementation
	 * for how. No callback is lost. An extreme @p max_in_flight is therefore accepted, not
	 * refused -- refusing a legal std::size_t for it would be the worse contract.
	 *
	 * @p queue_depth and @p max_in_flight are **both std::size_t and are not distinguishable by
	 * type**, so a call site that passes them in the wrong order gets a working ring of the wrong
	 * shape and no diagnostic. Name both values at the call site.
	 *
	 * Creating a ring is not evidence that this backend can work: a 5.1 to 5.5 kernel creates one
	 * and then fails every transfer, because IORING_OP_READ and IORING_OP_WRITE arrived in 5.6 and
	 * IORING_OP_ASYNC_CANCEL in 5.5. So the running kernel is probed once, all-or-nothing, through
	 * io_uring_probe_has_required_opcodes() below, and a missing opcode fails this call with
	 * std::errc::not_supported.
	 *
	 * Destroying it with ANY operation in flight is a precondition violation: a ring operation, or
	 * an operation forwarded to the pool whose callback has not yet run. It asserts, and under
	 * NDEBUG terminates, rather than free a record the kernel may still own. Unlike the
	 * thread-pool backend, this one is NOT safe to destroy with operations queued; file_io never
	 * does.
	 *
	 * @return The backend, or why the ring, its capabilities or its thread would not do.
	 */
	[[nodiscard]] std::expected<std::unique_ptr<io_backend>, std::error_code> make_io_uring_backend(thread_pool& pool,
	                                                                                                std::size_t  queue_depth,
	                                                                                                std::size_t  max_in_flight);

	/**
	 * Whether @p probe reports every opcode this backend submits: READ, WRITE, FSYNC and
	 * ASYNC_CANCEL.
	 *
	 * All-or-nothing on purpose. It is what keeps the capability question one check with one error
	 * path rather than a per-operation runtime matrix, and the four are wanted together or not at
	 * all: a backend that cannot cancel cannot honor the seam's cancel, and one that cannot fsync
	 * would have to route two of its four operations elsewhere.
	 *
	 * Declared here rather than kept private to the backend's translation unit so that the suite
	 * can run it against a SYNTHETIC io_uring_probe with one opcode's bit cleared. That is not a
	 * test hook into production behavior: this is a pure function of its argument, with no state
	 * and no side effect, and make_io_uring_backend() decides with this same function. Without it
	 * exported, "a missing opcode is refused" is unobservable on a machine whose kernel has all
	 * four, which is every machine this project builds on.
	 */
	[[nodiscard]] bool io_uring_probe_has_required_opcodes(const ::io_uring_probe& probe) noexcept;

	/// The submission ring entries @p ring was created with. Precondition: @p ring was built by make_io_uring_backend().
	[[nodiscard]] std::size_t io_uring_submission_depth_for_testing(const io_backend& ring) noexcept;

	/// The completion ring entries @p ring was created with. Same precondition.
	[[nodiscard]] std::size_t io_uring_completion_depth_for_testing(const io_backend& ring) noexcept;

	/**
	 * How many completions the reaper delivered to nobody. Same precondition.
	 *
	 * A completion whose user_data is in no registry, and a completion whose record was found but
	 * owes no callback because its submission was refused and its caller already has an error.
	 * Not this backend's own bookkeeping: the wake-up NOP is excluded before the lookup, and a
	 * cancel entry's own completion is excluded there too.
	 *
	 * Neither producer can be reached from the suite, so every test asserts this is still zero
	 * rather than that it counted something; the implementation's own comment on the counter says
	 * why, for each.
	 */
	[[nodiscard]] std::size_t io_uring_ignored_completions_for_testing(const io_backend& ring) noexcept;

	/// The operations this backend puts on the ring. Every other io_op is forwarded to the pool.
	enum class io_uring_operation : std::uint8_t
	{
		read,
		write,
		/// IORING_OP_FSYNC with no flags: data and metadata.
		fsync,
		/// IORING_OP_FSYNC with IORING_FSYNC_DATASYNC: data, and only the metadata needed to read it back.
		fdatasync,
	};

	/**
	 * The ring operation that serves @p op. Precondition: @p op is read, write, sync_full or
	 * sync_data. Any other value is a bug: it asserts in a debug build and maps to read otherwise.
	 *
	 * Declared here, with io_uring_fsync_flags_of(), for the reason io_uring_probe_has_required_opcodes()
	 * is: both are pure functions of their argument that submit() decides with, and exported so the
	 * suite can assert the mapping directly. sync_full and sync_data complete identically, so no
	 * completion can show a wrong mapping.
	 */
	[[nodiscard]] io_uring_operation io_uring_operation_of(io_op op) noexcept;

	/**
	 * The fsync_flags an IORING_OP_FSYNC entry for @p operation carries: IORING_FSYNC_DATASYNC for
	 * fdatasync and zero for everything else. A pure function, exported for the reason above.
	 */
	[[nodiscard]] unsigned io_uring_fsync_flags_of(io_uring_operation operation) noexcept;

	/**
	 * What one io_uring_submit() call offered the kernel, and what the kernel took.
	 *
	 * The pair is carried as a struct rather than as two adjacent integers because an `int` and an
	 * `unsigned` convert to one another implicitly: a call site that transposed them would get no
	 * diagnostic from the compiler, and none from clang-tidy either. derive_ring_sizes()'s
	 * ring_bounds in the implementation is the same device against the same hazard.
	 */
	struct io_uring_submit_counts
	{
		/// What io_uring_submit() returned: how many SQEs the kernel took, or a negated errno.
		int submitted = 0;
		/// What io_uring_sq_ready() reported immediately before that call: the SQEs offered to it.
		unsigned offered = 0;
	};

	/**
	 * Whether the entry at the tail of @p counts.offered reached the kernel.
	 *
	 * **io_uring_submit() returns how many SQEs the kernel CONSUMED, not whether it succeeded.**
	 * io_uring_enter submits in ring order and breaks out of its loop where it cannot allocate an
	 * io_kiocb, returning the count it managed -- which with one entry offered is zero, and with
	 * several offered is any number below the count. liburing says so in its own header, in the
	 * comment on io_uring_sq_ready: "always use real head, to avoid losing sync for short submit".
	 *
	 * So a non-negative return is NOT a successful submission, and a check that reads only the
	 * sign hands the caller a token for an operation the kernel never took: it never completes,
	 * the caller's in-flight permit is never released, and the backend's destructor then stops the
	 * process over a caller that did nothing wrong.
	 *
	 * The entry this backend has just prepared is the LAST of the offered ones -- io_uring_get_sqe()
	 * takes from the tail -- and the kernel consumes from the head in order, so that entry was
	 * taken if and only if every offered entry was. That is the whole of this predicate: a count
	 * equal to what was offered. A genuine short submit (`0 < submitted < offered`), which an entry
	 * left published by an earlier unconsumed submission makes reachable, therefore answers false
	 * exactly as a zero does.
	 *
	 * A negated errno is negative and answers false; there is no offered count it could equal. An
	 * @p offered of zero also answers false, since an entry cannot have been among what was taken
	 * if nothing was offered -- a case neither caller can present, because both read the count
	 * after io_uring_get_sqe() has handed back an entry and while the submission mutex is held.
	 *
	 * **This rests on the ring NOT being IORING_SETUP_SQPOLL**, which create_ring() does not set.
	 * Without it the kernel's head advances only inside an io_uring_enter this backend makes with
	 * the submission mutex held, so the offered count cannot go stale between the two reads. A
	 * polling thread would advance that head asynchronously, letting io_uring_sq_ready() exceed
	 * what the submission went on to offer and turning this equality into a FALSE NEGATIVE -- which
	 * quarantines a record whose completion will still arrive, on the ordinary path rather than an
	 * unreachable one. Anyone adding SQPOLL owes this predicate a different question.
	 *
	 * A pure function of its argument, exported for the reason io_uring_probe_has_required_opcodes()
	 * is: a short submit needs a kernel allocation failure and cannot be produced from a test, so
	 * the decision is asserted here rather than through a submission that cannot be made to fail.
	 */
	[[nodiscard]] bool io_uring_submission_was_consumed(const io_uring_submit_counts& counts) noexcept;
} // namespace alt::detail

#endif

# Design: awaitable file I/O with platform backends

**Date:** 2026-11-20
**Status:** Approved
**Issue:** #49
**Related:** `2026-09-10-coroutines-roadmap.md` (this is a new phase 7),
`2026-11-17-cancellation-ref-design.md` (the mechanism this component is the first
in-library consumer of)

## Intent and the why

`cpwf` is a web framework whose server, sessions and widgets all run on one
event-loop thread. Every disk operation it makes today runs on that thread:

| Where | What runs on the loop thread |
|-------|------------------------------|
| `src/serve/file_response.cpp` | `::pread` of a 64 KiB chunk, once per pull, for every download in flight |
| `src/serve/resource.cpp`, `src/server/server.cpp` | `::open` + `::fstat` per request |
| `src/serve/confined_open.cpp` | `openat2` ×2 + `fstat` ×2 + `close` per served path |
| `src/serve/owner_store.cpp` | `::open`, `::write` per upload chunk |

One cold read or one slow disk therefore stalls every connection the process is
serving. `cpwf` has already moved the worst offender off the loop by hand:
`OwnerFiles::Writer::commit()`'s fsync and rename run on `detail::StoreWorker`, a
hand-rolled single-thread queue, and `CommitOnWorker` is a hand-written awaiter that
hops there and resumes on the application's thread.

That is the shape every one of the sites above wants, and none of it is specific to a
web server. The hard parts — keeping an operation's memory alive across a cancelled
chain's teardown, routing completions off the I/O thread, and the platform layer —
belong next to `task`, `scheduler`, `thread_pool` and `resume_on` rather than being
rebuilt per consumer.

**`cpwf`'s existing `CommitOnWorker` is the prior art for this component, and the
design below is a generalization of it.** Its `Handoff` struct — a `shared_ptr`
control block with a three-state phase machine, a `std::stop_callback` on the chain's
token, and `resume_or_abandon` as the last touch — is the pattern. It was written
against `alt::cancellation_ref` from outside the library; this moves it inside, makes
it cover a whole operation set, and gives it backends.

## Goals

1. **One interface, interchangeable backends.** A thread-pool backend everywhere,
   io_uring on Linux where the kernel has it, IOCP on Windows for reads and writes.
2. **Completion on a scheduler the caller names**, never inline on an I/O thread, so a
   consumer with a single-threaded loop keeps its threading rules.
3. **Cancellation through alt's existing model**, with no use-after-free: a cancelled
   await must not resume, and the operation's memory must stay valid until the backend
   has really finished with it.
4. **Bounded resources** — a configurable in-flight limit, with backpressure rather
   than unbounded queuing.
5. **No event-loop dependency.** The consumer's loop is woken through the
   `alt::scheduler_ref` it passed.
6. Enough of an operation set to replace every site in the table above:
   open, close, read-at, write-at, fsync/fdatasync, fstat/stat, rename, unlink,
   directory listing, and an escape hatch for platform calls the portable set does not
   cover (`openat2` with `RESOLVE_BENEATH`).
7. Sanitizer-clean (ASan/UBSan/TSan) and green on the full matrix: GCC 13+, Clang 19+,
   MSVC 19.50+.

## Non-goals

| Excluded | Reason |
|----------|--------|
| Sockets and network I/O | A different lifetime and cancellation story, and `cpwf` has its own. io_uring and IOCP would both admit it later behind the same backend interface; nothing here precludes it. |
| A path or filesystem library | `std::filesystem::path` is the currency. alt adds no path manipulation. |
| Memory-mapped I/O | No requirement calls for it, and its page-fault stalls are the problem this component exists to remove, not a solution to it. |
| `sendfile`/`splice` zero-copy download | A plausible `cpwf` want later, but it couples file I/O to socket I/O, which is the excluded half above. Revisit when sockets are on the table. |
| Streaming a directory as an `async_generator` | Phase 6 of the coroutines roadmap is unbuilt, so there is no generator to return. v1 lists a directory in one shot; see "What is left to the builder". |
| Aborting an in-flight operation | Not possible to promise portably, and not what cancellation means here. See "Cancellation" — this is a semantic decision, not an omission. |
| A user-supplied backend as a supported extension point | `io_backend` is public because it is the test seam and the callback-level API, but alt makes no compatibility promise about implementing it from outside. |

## Anti-goals

Things that would make this a failure even if every test passed:

- **A completion body running on an I/O thread.** Silent, intermittent, and it breaks
  the one rule the consumer has.
- **A use-after-free reached only by cancelling mid-operation.** This is the whole
  reason the component is hard; shipping it with that hole shipped nothing.
- **`windows.h`, `liburing.h` or `linux/io_uring.h` reaching a consumer's translation
  unit.** alt is header-heavy and `<alt/memory.hpp>`'s `windows.h` include is already
  a known wart. Multiplying it across the I/O surface would be a permanent tax on
  everyone who links alt.
- **A new external dependency.** alt has none, ships a `.deb`, and its package config
  carries only `find_dependency(Threads)`.
- **A silent backend fallback.** A consumer that got the thread-pool backend because a
  seccomp policy filtered `io_uring_setup` must be able to find that out, not discover
  it as a performance mystery. `cpwf`'s own `confined_open_supported()` refuses a mount
  rather than degrading quietly; same instinct.

## Constraints

**alt's cancellation tears down by destruction.** `cancellation_ref::abandon()`
destroys the root frame and every frame below it. A buffer declared as a local in the
awaiting coroutine, and the awaiter holding the operation's state, both die with it —
while io_uring and IOCP are still writing into them, because both complete *after* a
cancel request, not at it. This is the constraint that drives the whole design, and it
is why operation state cannot live in the coroutine frame.

**The home scheduler is reached by posting an `alt::work_item`.** `cpwf` already does
exactly this (`detail::ResourceAccess::post(*app, alt::work_item{...})`), so
`scheduler_ref` is the right currency and nothing new is needed on that side.

**A foreign awaiter is moved before it suspends.** `std::stop_callback` is not
movable, so it must be constructed in `await_suspend`, behind an indirection. `cpwf`
found this the hard way and documents it on `CommitOnWorker::stop_`.

**libstdc++ hides exactly this class of bug.** Per CLAUDE.md: its
`stop_source::request_stop()` skips relocking the callback list after the last
callback, which kept a use-after-free in `detail::stop_forwarder` invisible on Linux,
ASan included. Our awaiter registers a `std::stop_callback` on the chain's token, so
the same blind spot applies to it. The counter-measure is known and is a test
requirement below.

**ASan and TSan have a shared blind spot here.** Per
`project_sanitizer_blind_spot`: a use-after-free reached only through a mutex lock is
invisible to both. The operation block is mutex-guarded, so its teardown races are in
that blind spot and need a liveness sentinel, not just a sanitizer run.

**`alt::thread_pool` cannot be destroyed from its own worker** — it would join itself.
`cpwf` hand-rolled `StoreWorker` for precisely this case: the last reference to the
store can be held by a finishing job, so the store can die on the worker thread. The
same hazard exists for anything a completing operation might hold the last reference
to, and it is why `~file_io` carries a thread precondition below.

**`async_semaphore::release()` resumes parked waiters inline, on the releasing
thread.** Its own header says so: *"Resumption happens on the thread that released the
permit, inside `release()`, which therefore runs waiter bodies."* So where the
backpressure permit is released decides which thread the *next* operation is submitted
from — and releasing it from a backend completion callback would resume a waiter on an
I/O thread and have it submit from there, straight into the top anti-goal. This
dictates the release point in the design below.

**A growing internal type is an ABI break in this library.** The waiter-liveness-guard
plan was written for a patch bump and executed as **1.11.0, a minor**, because
`sizeof(detail::waiter_list)` grew and five `src/*.cpp` compile out-of-line definitions
of types that embed it into `libalt`, which ships as a `.deb`
(`2026-11-15-waiter-liveness-guard.md`, "Superseded"). Every phase here adds compiled
code and embeds new block types, so each phase's bump needs that question asked rather
than assumed from "no public API change".

**`cppcheck` only analyzes `src/*.cpp`,** and this component puts real logic there for
the first time at scale. Expect new findings in the suppression-worthy categories the
root `CMakeLists.txt` already catalogs, and expect them to be about the backends, not
the awaiters.

## Approaches

Three ways to build this were considered.

### A. Templates on the backend, header-only

`file_io<Backend>` with the operation set as templates, backend chosen at compile
time. Zero indirection, consistent with how header-only the rest of alt is.

**Rejected.** The backend choice is a *runtime* probe — the issue says so, and it has
to be: a kernel may have `io_uring` compiled out, or a seccomp policy may filter
`io_uring_setup`, and neither is visible at compile time. A template design therefore
needs a variant or a virtual layer anyway, and gets the worst of it: the platform
headers become visible in public headers (anti-goal), every consumer recompiles alt's
io_uring ring setup, and the fallback path is a second instantiation of every
operation.

### B. Virtual backend interface, operations compiled in `src/` — **chosen**

A public abstract `io_backend`. `file_io` holds one. The platform implementations live
entirely in `src/*.cpp`, so `liburing.h`/`linux/io_uring.h`/`windows.h` never reach a
consumer. One virtual call per operation, which is noise next to a syscall, let alone
a disk seek.

**Chosen** because it is the only one of the three that satisfies the "no platform
headers in consumer TUs" anti-goal while supporting a runtime probe, and because the
virtual interface doubles as the deterministic test seam (a `manual_io_backend`, in
the spirit of `manual_scheduler`) and as the callback-level API that `cpwf`'s
`BodySource::pull`/`kWait`/`wake` path needs without a coroutine per 64 KiB chunk.

The cost, stated plainly: alt grows its first substantial compiled subsystem, and
`file_io` is a runtime object a consumer must create and keep alive, where most of alt
is types you just use.

### C. Extend `task_completion_source` and let consumers drive their own threads

Ship no backends. Document the `CommitOnWorker` pattern, and give consumers a
reusable awaiter plus the existing `task_completion_source`.

**Rejected,** though it is the cheapest by a wide margin and is roughly what `cpwf`
has today. It fails the issue's actual ask: the hard part is the platform layer, and
this leaves all of it with the consumer. It also leaves the use-after-free question —
the part that needs library-grade care and a library-grade test suite — in the hands
of each caller who writes the awaiter again. `cpwf` got it right; a second consumer
would not necessarily.

## The design

### Layering

Three layers, each usable on its own:

1. **`io_backend`** (public, abstract) — submit an operation, get a completion
   callback. Callback-level, no coroutines. The platform seam, the test seam, and what
   a callback-shaped consumer like `cpwf`'s `BodySource` uses directly.
2. **The operation block and its awaiter** (internal, `alt::detail`) — a refcounted
   control block outside the coroutine frame, a `std::stop_callback` on the chain's
   token, and `resume_or_abandon` as the last touch. `CommitOnWorker` generalized.
3. **`file_io` and `async_file`** (public) — the idiomatic surface. Operations return
   `alt::task<std::expected<T, std::error_code>>`, so cancellation checking,
   backpressure and composition all come from machinery that already exists and is
   already tested.

Layer 3 is the headline and what the documentation leads with. Layer 1 exists in the
public headers with a "most callers want `async_file`" note on it.

### Why layer 3 returns `task<T>`

Backpressure has to suspend, and an operation therefore has two suspension points: the
in-flight slot, then the backend. A hand-written awaiter cannot `co_await` anything, so
a two-phase hand-rolled awaiter would have to reimplement what
`async_semaphore::acquire` plus `task`'s `await_transform` already do, including the
cancellation check between the phases.

Making the operation a coroutine instead gets all of that free:

```cpp
alt::task<std::expected<std::size_t, std::error_code>>
async_file::read_at(io_buffer buffer, std::uint64_t offset, std::size_t size)
{
    // SUPERSEDED: acquire_slot() is gone and this leaks the permit if the chain is cancelled; use detail::acquire_permit().
    const auto slot = co_await m_io->acquire_slot();   // backpressure; cancellable
    co_return co_await m_io->submit_read(m_handle, std::move(buffer), offset, size);
}
```

The price is one coroutine frame allocation per operation. Against a 64 KiB disk read
that is free, and the in-flight limit bounds how many exist at once.

### Operation state lives outside the frame

The core of the design, and the answer to the driving constraint.

```cpp
namespace alt::detail
{
    /// One in-flight operation. Refcounted, and NEVER a coroutine-frame local.
    struct io_operation
    {
        enum class phase : std::uint8_t { waiting, done, detached };

        std::mutex              mutex;
        phase                   state = phase::waiting;
        std::coroutine_handle<> handle;
        cancellation_ref        chain;

        // The three co-owned things, and the whole point of the block. Each
        // outlives the coroutine frame by as long as the backend needs it.
        std::shared_ptr<io_shared> owner;    // slots, backend, completion scheduler
        std::shared_ptr<io_handle> file;     // the descriptor: NOT closed under us
        io_buffer                  buffer;   // the memory the kernel is writing into

        io_completion                  result;
        std::shared_ptr<io_operation> keep;  // set by whoever posts the resumption

        std::uint32_t sentinel = alive_pattern;   // see "Testing"
    };
}
```

**Three things are co-owned, not one.** The buffer was the obvious one. The other two
are the same bug class and were missed in the first draft of this design:

- **The descriptor.** `async_file` lives in the cancelled frame. If it closed the
  descriptor on destruction, a pool job that had already issued its `pread` — or that
  issues one and only then observes `state != waiting` — would be reading from a closed
  and possibly *recycled* fd. A recycled descriptor is worse than a closed one: the
  read succeeds, against someone else's file. So the descriptor lives in a refcounted
  `io_handle` that every block co-owns, and it is closed by whichever party drops the
  last reference — which, for a cancelled operation, is the backend.
- **The service's shared state.** A resumption posted to the home scheduler must
  release its backpressure permit, and that permit lives in `file_io`. If `file_io` can
  be destroyed while such a resumption is still queued, the release touches freed
  memory. So the slot accounting, the backend pointer and the completion
  `scheduler_ref` live in a refcounted `io_shared` that every block co-owns too, and
  `file_io` is a handle to it rather than the owner of it.

Three states, and which party may move out of `waiting` is the whole protocol:

- **`waiting` → `done`**: the backend, when the operation really completed. It fills
  `result` and posts the resumption to `home`.
- **`waiting` → `detached`**: the stop callback, when the chain was stopped. It posts
  the teardown to `home`; the resumption there takes the `abandon` branch of
  `resume_or_abandon`.
- Whichever party loses the race finds a state other than `waiting`, under the mutex,
  and **does nothing at all**. It does not touch `handle`, `chain` or the frame.

The block is held by a `shared_ptr`, and it **holds one to itself across the
submission**, released when the completion callback runs or when the submission fails.
That self-reference is what makes the block — and the `io_buffer` and `io_handle` it
co-owns — survive the frame's destruction by an arbitrary margin, which is what makes
"a cancelled await must not resume" and "the buffers stay valid until the backend has
really finished" simultaneously true.

**Corrected 2026-11-20, during phase 7a's implementation.** This paragraph previously
said the backend's submission holds the copy. It cannot: `io_backend::submit` takes a
raw `void* state` that owns nothing (see "The `io_backend` contract"), so there is
nowhere on the backend's side for a reference to live. Keeping the claim would have
left the single most important lifetime guarantee in this design resting on a
mechanism that does not exist. The self-reference is the remedy, and because it is a
deliberate cycle, every exit path must break it — normal completion, detached
teardown, a `submit` that returns an error, and a `submit` that completes inline
before returning. A leak on any one of them is a defect, so that enumeration belongs
in the review checklist for this component rather than in a comment.

This is `cpwf`'s `Handoff` with the buffer added and the result generalized. The
`keep` member is its trick too: the posting party moves its own `shared_ptr` into the
block, and the resumption takes it back out, so the block cannot die between the post
and the run.

### Cancellation means detach, not abort

**A cancelled operation is not stopped. The awaiting chain is detached from it.**

| Backend | What a cancel actually does |
|---------|----------------------------|
| thread pool | A job not yet started finds `state != waiting` and returns without doing the I/O. A running job runs to completion and discards its result. |
| io_uring | `IORING_OP_ASYNC_CANCEL` is submitted as a *hint*. The original CQE still arrives and is still consumed. |
| IOCP | `CancelIoEx` likewise. The completion packet still arrives. |

So an operation may still read, still write, still rename after the chain that asked
for it is gone. This is stated as the contract, not buried:

> Cancellation ends your wait. It does not promise to end the operation. A write that
> had reached the kernel may still land.

This is not a compromise reluctantly accepted — it is the only thing portably true,
and it already matches the consumer's own published contract. `cpwf`'s README says of
a cancelled upload handler: *"Work it started on another thread is not stopped."* The
library's semantics and the consumer's documentation agree without either changing.

### The `io_backend` contract

This is the interface 7b and 7c are forbidden to widen, so it is specified here in
full rather than left to the backends that will implement it.

```cpp
namespace alt
{
    /// Identifies a submitted operation for cancel(). Opaque; backend-defined.
    class io_token { ... };

    /// What an operation produced. `bytes` is meaningful for read/write only.
    struct io_completion
    {
        std::size_t     bytes = 0;
        std::error_code error{};
    };

    using io_completion_fn = void (*)(void* state, io_completion result) noexcept;

    class io_backend
    {
    public:
        virtual ~io_backend() = default;

        /// Submits one operation.
        ///
        /// THE CALLBACK RUNS ON AN I/O THREAD, and MAY run inline inside this call
        /// before it returns -- a thread-pool arm with an idle worker, or a ring
        /// submission the kernel rejects outright, both complete that way. A caller
        /// that needs to be somewhere specific posts; this layer never does it for
        /// you. See `file_io`/`async_file` for the layer that does.
        ///
        /// The callback runs EXACTLY ONCE for every submission that returns a token.
        /// A submission that returns an error has NOT run it and never will.
        [[nodiscard]] virtual std::expected<io_token, std::error_code>
            submit(const io_request& request, io_completion_fn on_done, void* state) = 0;

        /// A HINT, and nothing more. The operation's callback still runs exactly
        /// once, with whatever outcome the kernel settled on -- which may be
        /// success, because io_uring and IOCP both complete after a cancel request
        /// rather than at it. Never blocks.
        virtual void cancel(io_token token) noexcept = 0;

        [[nodiscard]] virtual io_backend_kind kind() const noexcept = 0;
    };
}
```

Two decisions in there are the load-bearing ones:

**The callback fires on an I/O thread, and layer 2 does the posting.** The alternative
— handing the backend a `scheduler_ref` and having it post — would bake goal 2 into
the lowest layer and make it no longer callback-level, which defeats the reason that
layer is public. `cpwf`'s `BodySource` already owns a loop it knows how to post to
(`detail::ResourceAccess::post`), so making it post is asking for nothing it does not
already do. The cost is that layer 1 is sharp, and its documentation says so in
capitals rather than in a footnote.

**`submit` returns a token and `cancel` takes one.** Cancellation has to be
per-operation — the thread-pool job must find its own state, and
`IORING_OP_ASYNC_CANCEL`/`CancelIoEx` are per-operation calls — so there is no version
of this interface without a handle, and inventing it in 7b would be exactly the
widening 7a is supposed to prevent.

**`manual_io_backend` is injected, not selected.** The first draft listed
`io_backend_kind::manual` as if an enumerator could conjure an instance; it cannot, and
the deterministic suite that is the centerpiece of the testing strategy was therefore
unreachable through the stated API. `create` gets an overload:

```cpp
[[nodiscard]] static std::expected<file_io, std::error_code>
    create(scheduler_ref completion, io_backend& backend,
           file_io_options options = {});
```

`io_backend_kind::manual` stays, as what `manual_io_backend::kind()` reports, so
`file_io::backend()` never lies about what it is driving.

**Corrected 2026-11-20, found by the whole-branch review of phase 7a rather than designed
in: the seam has a hole for handle preparation, and `adopt()` is it.** Overlapped I/O
needs a handle that was opened with `FILE_FLAG_OVERLAPPED` and associated with the
completion port before its first read or write. Nothing in the seam's drawing considered
that. A future IOCP backend can prepare a handle it opened itself, because `open` goes
through `submit`. It cannot prepare one that arrived through `async_file::adopt()`: all it
ever receives is an integer it has never seen. 7a's resolution is a stated precondition,
not a mechanism: `adopt()` documents that the handle must be one on which a transfer
blocks until it is done (on Windows, not opened with `FILE_FLAG_OVERLAPPED`; on POSIX, seekable, so
a FIFO, socket or tty reports `std::errc::illegal_seek`), because the thread-pool transfer arm passes an `OVERLAPPED` only to carry
the offset and would otherwise report `ERROR_IO_PENDING` as a failure and release a buffer
the kernel still owns. A more robust arm (a per-transfer event in `position.hEvent`, then
`GetOverlappedResult` waiting on `ERROR_IO_PENDING`) is the right eventual answer and is
tracked as issue #52; it is untested here because no Windows host was available to run it.
**Resolved by issue #52:** the arm now gives each transfer its own event, waits on it when
the call goes pending, and tags the event's low-order bit so a handle the consumer has
associated with a completion port gets no packet naming the worker's stack `OVERLAPPED`.

**Updated by issue #79 (phase 7b):** the POSIX half of that precondition no longer names an
error, so the sentence above records what `adopt()` said in 7a rather than what it says now.
`adopt()` still requires a seekable descriptor, but what a non-seekable one does is
backend-specific and promised by neither arm: the thread-pool arm refuses it, while the io_uring
backend 7b added performs the transfer and discards the offset. Two corrections are folded into
that, both left standing above because they are what 7a wrote: the condition the pool arm reports
is `std::errc::invalid_seek`, not `std::errc::illegal_seek` -- that spelling is ESPIPE's strerror
text and is not a `std::errc` member at all, which only came to light when a test was finally
compiled against the claim.
The Windows half of the synchronous-handle precondition is lifted. The 7c question below
is unchanged: the pool arm can now serve an overlapped handle, but that does not give an
IOCP backend a way to prepare one.
**7c had to choose among three options, each with a cost (issue #53):**

1. *Add a registration call to the seam.* Honest, and gives an IOCP backend its hook, but
   it is the widening this phase exists to prevent, and it lands on every backend,
   including ones with nothing to register.
2. *Route adopted handles to the thread-pool arm.* Keeps the seam as it is, but it is the
   silent backend fallback the anti-goals forbid: a consumer who asked for IOCP would get
   the pool for exactly the files they brought in themselves, with nothing saying so.
3. *Require that adopted handles be prepared for the backend in use.* Cheapest to build,
   but it changes a published public API's requirements in a later minor release, for
   consumers who adopted under the synchronous-handle rule above.

**Decided 2026-11-21 (issue #53): option 2, reported per file, implemented in the file
layer.** The full analysis is on the issue; the reasoning that decided it:

- **The problem was larger than `adopt()`.** An IOCP backend must know, per transfer,
  whether *that* handle is prepared, and the seam cannot tell it: `io_request` carries a
  bare `native_handle`, and a backend never sees a close, because `io_handle`'s
  destructor calls `CloseHandle` itself. A registry of prepared handles could therefore
  never retire an entry, and Windows reuses `HANDLE` values, so a closed prepared value
  can come back as an unprepared adopted one: overlapped I/O on a handle with no port,
  waiting for a packet that never comes. Option 1 done properly therefore needs handle
  *lifetimes* on the seam, not one registration call, and `adopt()` (which returns
  `async_file`, not `expected`) has nowhere to report a preparation that fails.
- **Option 2 is the only one that changes neither the seam nor a published contract.**
  The routing decision belongs to the file layer, which knows a file was adopted:
  `io_shared` holds a second, internal backend pointer to a thread-pool backend, and an
  adopted file's operations are submitted there whenever the primary backend is IOCP.
  On every other backend nothing changes, since only IOCP needs a handle prepared.
- **Since #52 it is correct**, because the pool's transfer arm serves synchronous and
  overlapped handles alike.
- **It is not silent.** Each `async_file` reports which backend serves it. That is a
  public addition, not a break, and it is the one public API change 7c makes, against
  the staging table's "no public API change": the anti-goal outranks that line.
- **Its cost falls on no known consumer.** The only adopter the design names is
  `cpwf`'s `confined_open`, which is `openat2` and so Linux-only, where adopted
  descriptors need no preparation.
- **It upgrades additively.** Should a Windows adopter appear, adopted files can move to
  IOCP by giving the seam handle lifetimes (option 1 done properly, perhaps using
  `ReOpenFile` to obtain an overlapped handle to the same file), without breaking anyone.

**Batching is a trade the seam made, seen and accepted.** The seam has no batch or flush
point, and `submit` gives a backend no way to know whether more submissions are coming.
An io_uring backend therefore has three choices: enter the kernel once per submission,
use `IORING_SETUP_SQPOLL`, or run a submitter thread. Each works, and each forfeits the
batching that is io_uring's main advantage; adding a `flush()` in 7b would be a widening
of the seam. The home-thread submission invariant (see "Backpressure, and where the
permit is released") already serializes submissions on one thread, so batching has a
natural place to happen later.

### Buffers

`alt::io_buffer`: a refcounted handle to a byte range, so the operation block can
co-own what it reads into.

```cpp
class io_buffer
{
public:
    /// alt owns the storage. The safe default, and cancellation-proof by construction.
    [[nodiscard]] static std::expected<io_buffer, std::error_code> allocate(std::size_t size);

    /// The caller's memory, kept alive by the caller's own owner. `keepalive` is
    /// held for as long as the operation exists -- pass the shared_ptr that already
    /// owns the storage (e.g. shared_from_this()).
    [[nodiscard]] static io_buffer borrow(std::span<std::byte> bytes, std::shared_ptr<void> keepalive);

    /// The caller's memory with NOTHING keeping it alive. Zero-copy and the one
    /// sharp edge in this component: the storage must outlive the operation, which
    /// is strictly longer than the await when the chain can be cancelled.
    [[nodiscard]] static io_buffer borrow_unsafe(std::span<std::byte> bytes);

    [[nodiscard]] std::span<std::byte> bytes() const noexcept;
    [[nodiscard]] std::size_t          size()  const noexcept;
};
```

Three constructors because the consumer has three situations:

- `allocate` — the default, and what `cpwf`'s `FileBody::buf_` becomes.
- `borrow(bytes, keepalive)` — the write path. Upload bytes live in a buffer owned by
  something that is already a `shared_ptr`; handing that pointer over makes the
  zero-copy write cancellation-safe with no reasoning required of the caller.
- `borrow_unsafe` — for a caller who can prove the lifetime (a static buffer, a buffer
  owned above a `sync_wait`). Named to be hard to reach for, and the documentation
  says exactly which guarantee is being taken over.

### Service and handle

```cpp
namespace alt
{
    enum class io_backend_kind : std::uint8_t { automatic, thread_pool, io_uring, iocp, manual };

    struct file_io_options
    {
        // Selects a built-in backend. `manual` is not selectable here -- a
        // manual_io_backend is passed by reference to the create() overload.
        io_backend_kind backend      = io_backend_kind::automatic;
        std::size_t     max_in_flight = 64;   // the backpressure limit
        std::size_t     queue_depth   = 0;    // ring size; 0 = derive from max_in_flight
    };

    class file_io
    {
    public:
        /// `workers` runs the thread-pool arm; `completion` is where every operation's
        /// awaiting coroutine is resumed. Both must outlive this object.
        [[nodiscard]] static std::expected<file_io, std::error_code>
            create(thread_pool& workers, scheduler_ref completion, file_io_options options = {});

        ~file_io();   // blocks until every in-flight operation has really completed

        file_io(const file_io&) = delete;   // and the other three
        ...

        /// Which backend the probe actually settled on. Never a mystery.
        [[nodiscard]] io_backend_kind backend() const noexcept;

        [[nodiscard]] task<std::expected<async_file, std::error_code>>
            open(const std::filesystem::path& path, open_flags flags, file_mode mode = {});

        [[nodiscard]] task<std::expected<file_status,  std::error_code>> stat(const std::filesystem::path&);
        [[nodiscard]] task<std::expected<void,         std::error_code>> rename(const std::filesystem::path& from,
                                                                               const std::filesystem::path& to);
        [[nodiscard]] task<std::expected<void,         std::error_code>> unlink(const std::filesystem::path&);
        [[nodiscard]] task<std::expected<std::vector<directory_entry>, std::error_code>>
            list_directory(const std::filesystem::path&);

        /// The escape hatch: runs `f` on a worker thread and resumes the caller on the
        /// completion scheduler with whatever it returned. For platform calls the
        /// portable set does not cover -- `cpwf`'s confined_open(), which is five
        /// syscalls that must run as a unit, is the motivating case.
        ///
        /// `f` is MOVED INTO the operation block, never left in the coroutine frame.
        /// See below for why that is not a detail.
        template<std::invocable F>
          requires std::is_nothrow_move_constructible_v<F>
        [[nodiscard]] task<std::expected<std::invoke_result_t<F>, std::error_code>> run(F f);
    };

    class async_file
    {
    public:
        /// Adopts an already-open native handle, taking ownership. This is how a
        /// consumer that opened a file its own way -- `cpwf`'s confined_open() -- gets
        /// it into the async world.
        [[nodiscard]] static async_file adopt(file_io&, native_handle);

        ~async_file();   // closes, if still open
        async_file(async_file&&) noexcept;   // movable, not copyable

        [[nodiscard]] task<std::expected<std::size_t, std::error_code>>
            read_at(io_buffer buffer, std::uint64_t offset, std::size_t size);
        [[nodiscard]] task<std::expected<std::size_t, std::error_code>>
            write_at(io_buffer buffer, std::uint64_t offset, std::size_t size);

        [[nodiscard]] task<std::expected<void,        std::error_code>> sync(sync_mode = sync_mode::full);
        [[nodiscard]] task<std::expected<file_status, std::error_code>> stat();
        [[nodiscard]] task<std::expected<void,        std::error_code>> close();

        [[nodiscard]] native_handle handle() const noexcept;

    private:
        /// Co-owned with every operation block in flight on this file, which is what
        /// keeps the descriptor from being closed -- or recycled -- under a backend
        /// that is still using it.
        std::shared_ptr<detail::io_handle> m_file;
        std::shared_ptr<detail::io_shared> m_owner;
    };
}
```

Notes on the surface:

- **`create` returns `std::expected`**, matching `thread_pool::create` for the same
  reason: starting a ring or a completion port can fail, and a constructor cannot say
  so without throwing. `file_io` is likewise immovable, so references held by
  suspended frames stay valid.
- **The thread pool is the caller's.** `file_io` borrows a `thread_pool&` rather than
  owning threads. This is what the issue asks for ("built on `alt::thread_pool`"), it
  keeps the component out of the thread-lifetime business, and it sidesteps the
  self-join hazard `cpwf`'s `StoreWorker` exists to work around — the pool's lifetime
  is the caller's problem, as it already is everywhere else in alt.
- **Every backend has a thread-pool arm.** io_uring and IOCP sit *in front of* the
  pool, not instead of it: `open`, `stat`, `rename`, `unlink`, `list_directory` and
  `run()` go to the pool on every platform, because Windows offers no async form of
  them and because `run()` has no meaning on a submission queue. So the fallback path
  is not a special mode — it is always present and always exercised.
- **`~file_io` blocks** until the backend is done with every operation — see
  "Shutdown" below, which is more delicate than one line can carry.
- **`sync_mode::data` degrades to a full flush on Windows**, which has no
  `fdatasync` equivalent. Documented on the enumerator, not discovered.
- **`file_status` carries a portable `identity`** (`st_dev`/`st_ino` on POSIX, volume
  serial plus file index on Windows) alongside `size`, `mtime_ns` and a `kind` enum.
  `cpwf`'s ETag is built from inode, size and mtime, so the identity has to survive
  the abstraction.
- **`directory_entry::kind` may be `unknown`.** `d_type` is `DT_UNKNOWN` on several
  filesystems; the caller stats when it needs to know. Documented rather than papered
  over with a hidden stat per entry.
- **`open_flags` uses `alt::flags<>`** from `<alt/flags.hpp>`, which is the library's
  existing bitmask type.

### Backpressure, and where the permit is released

`max_in_flight` permits in an `alt::async_semaphore` living in `io_shared`. An
operation acquires one before submitting and releases it when its block leaves
`waiting`.

**The permit is released on the home thread, inside the posted completion, and before
`resume_or_abandon`.** This is forced rather than chosen: `async_semaphore::release()`
resumes a parked waiter *inline on the releasing thread*, so releasing from the
backend's callback would resume the next operation's coroutine on an I/O thread and
have it submit from there. The posted completion therefore does exactly three things,
in this order:

1. read what it needs out of the block under the mutex;
2. release the slot — which may inline-resume the next waiter, which submits *from this
   same home thread*, which is the point;
3. `chain.resume_or_abandon(handle)`, last, because either branch may free the frame.

**The invariant this buys: every submission happens on a thread the completion
scheduler runs on, never on an I/O thread.** That answers the top anti-goal for the
submission path as well as the completion path, and it makes the backend's reentrancy
requirements trivial — it is never reentered from its own completion thread.

The consequence, stated rather than discovered: **slot handoff is serialized through
the home scheduler.** A loop that stops pumping stops issuing new I/O. For a
backpressure limit that is the right failure mode — the limit exists to couple
submission to the consumer's ability to keep up — and it is a coupling the consumer
already has for everything else it does.

Two further things fall out of reusing a built primitive. The waiting-for-a-slot phase
is cancellable by alt's ordinary machinery, with no kernel involvement and therefore no
buffer-lifetime question at all — the interesting cancellation window is only the
submitted phase. And a consumer that over-subscribes gets a suspended coroutine rather
than a growing queue, which is what "backpressure rather than unbounded queuing" has
to mean for a coroutine API.

### Shutdown

The first draft said `~file_io` "blocks until every in-flight operation has really
completed", which is ambiguous in a way that matters: every completion path ends in a
*post to the home scheduler*, and for the motivating consumer that scheduler is the
loop thread — which is the thread that will destroy `file_io`. Waiting for those posts
to *run* would deadlock on the spot, with nothing pumping the loop.

So, precisely:

**`~file_io` waits for the backend, not for the resumptions.** It stops admitting
submissions, then blocks until no operation is outstanding *at the backend*. When it
returns, no backend callback will fire again and the kernel holds none of this
object's memory. Resumptions already posted to the home scheduler may still be queued,
and that is fine: a block holds a `shared_ptr` to `io_shared`, so a resumption that
runs after `~file_io` releases its permit against state that is still alive. This is
why `io_shared` is refcounted and `file_io` is only a handle to it.

**Two preconditions, both with precedent in this library:**

- **`file_io` must outlive every operation started on it.** An operation still parked
  on a slot at destruction is a violation, not an error to report — the same contract
  `scheduler_ref` and `thread_pool` already state ("the pool must outlive every
  suspension that targets it"). This is why no "never ran" enumerator appears in the
  surface: there is no legitimate way to observe one.
- **`~file_io` must not run on a thread of the pool its backend runs on** (the borrowed `thread_pool` for the built-in overload, or whichever pool an injected backend was built with, if any). It would be
  waiting for jobs queued behind itself. This is exactly the hazard `cpwf` hand-rolled
  `StoreWorker` to escape — the last reference to the store can be held by a finishing
  job, so the store can die on the worker thread — and borrowing the pool rather than
  owning it means alt can state the precondition instead of solving it.

**No two-phase shutdown API.** `stop_accepting()`/`drain()` as separate public calls
were considered and rejected: `thread_pool` already settles this question the other
way, putting "stop admitting work before the pool is destroyed" on the caller, and a
second shutdown protocol in the same library with different ergonomics is worse than
one documented obligation.

### The escape hatch's lifetime, which is the same problem again

`run(F f)` as first sketched contradicted this design's own driving constraint. If
`run` is a coroutine, then `f` and its captures live in that coroutine's frame — and
cancellation destroys the frame while a worker is still running `f`. For the motivating
case, `cpwf`'s `confined_open()` with its captured path string, that is a
use-after-free on the lambda's captures by precisely the route the component exists to
close.

So `f` is **moved into the operation block**, type-erased behind a small virtual so the
block stays one type rather than a template family. The rest follows:

- **Cancelled before `f` ran:** `f` is never invoked, and is destroyed *on the worker*,
  taking its captures with it. This is not merely safe, it is a behaviour the consumer
  depends on: `CommitOnWorker`'s job drops its `OwnerFiles::Writer` this way, and with
  it the temporary file — *"the Writer is dropped with this job"*. Generalizing the
  pattern has to generalize that too.
- **Cancelled while `f` runs:** `f` finishes. Its result is destroyed with the block.
  Cancellation detaches; it does not abort.
- **`f` throws:** the exception is captured in the block as an `exception_ptr` and
  rethrown from the `co_await` if the chain is still there, swallowed with the block if
  it is not. This is `task_completion_source`'s existing convention — exceptions
  rethrow from the await, `error_code`s travel in the `expected` — and not a new one.
- **It consumes an in-flight slot** like every other operation, so the escape hatch
  cannot be used to route around the bound.
- **It always runs on the pool arm**, on every platform, because a submission queue has
  nowhere to run arbitrary code.
- The `expected`'s error channel carries submission failure only. A `void`-returning
  `F` yields `std::expected<void, std::error_code>`.

### Thread affinity

Stated per-member in the Doxygen, as every other alt primitive does. In summary:

| Subject | Contract |
|---------|----------|
| `file_io` operations | May be initiated from any thread; internally synchronized. In practice the slot invariant above means they are initiated on a home thread. |
| `~file_io` | Not from a worker of the pool the backend runs on (the borrowed `thread_pool` for the built-in overload). See "Shutdown". |
| Two `read_at`/`write_at` on one `async_file` | Supported concurrently. They are positional and share no file offset, which is why the operation set has no seek and no current-position read. |
| `close()` racing an outstanding operation on the same file | A precondition violation. The refcounted `io_handle` keeps the descriptor valid regardless, so this is a logic error rather than a memory error. |
| Two operations sharing one `io_buffer` region | A precondition violation; `io_buffer` does no interlocking. |
| `async_file` itself | Not synchronized: it may move between threads but is not itself thread-safe, matching `cpwf`'s `Writer` ("One thread at a time: a Writer may move between threads, but is not itself synchronised"). |

### Closing a file

`~async_file` cannot `co_await`, so it drops its `io_handle` reference and closes
inline **only if it was the last reference**. The cases:

- **No operations outstanding** (the common case): the descriptor is closed on the
  destroying thread. `::close`/`CloseHandle` on a regular file flushes nothing and is
  cheap; it can block on a network filesystem, which is why awaitable `close()` exists
  for callers who care. The honest statement is that the destructor is a convenience
  and `co_await file.close()` is the hot-path answer.
- **Operations outstanding** (a cancelled request, the case that matters): the
  destructor closes *nothing*. The last block to finish drops the last reference, so
  the close happens on a worker — off the loop thread, which is where the consumer
  wanted it anyway.

`close()` marks the handle closed-by-request and closes once the outstanding
operations finish; operations started on it afterwards fail with
`std::errc::bad_file_descriptor` rather than being submitted against a descriptor
number that may have been reused.

### io_uring without liburing

**Recommendation: raw syscalls, no new dependency.** `io_uring_setup`/`io_uring_enter`
through `::syscall`, rings `mmap`ed directly, built only when `<linux/io_uring.h>` is
present at configure time (`check_include_file`), with the runtime probe falling back
to the thread-pool arm when the kernel or a seccomp policy says no.

The reasoning is the no-dependency anti-goal: alt ships a `.deb` and a package config
carrying only `find_dependency(Threads)`, and `liburing` would change both, for every
consumer, to save a few hundred lines in one translation unit. The project is already
comfortable at this level — `cpwf`'s `confined_open` calls `::syscall(SYS_openat2,
...)` by hand for the same reason.

This is a call worth overriding if the extra dependency is cheaper than the risk: the
ring setup involves `mmap` layout and acquire/release ordering on the SQ and CQ head
and tail, and it is the one part of this design where getting it subtly wrong produces
a rare corruption rather than a test failure. An `ALT_USE_LIBURING` option is *not*
proposed, because two code paths through the submission layer doubles the part that is
hardest to test.

### Platform files and CMake

`src/*.cpp` is globbed, so platform sources cannot be excluded by CMake; they guard
themselves with `#ifdef`, which is the pattern `<alt/memory.hpp>` already uses.
Layout:

```
include/alt/file_io.hpp        file_io, file_io_options, io_backend_kind
include/alt/async_file.hpp     async_file, sync_mode, open_flags alias
include/alt/io_buffer.hpp      io_buffer
include/alt/io_backend.hpp     io_backend, io_request, io_completion, io_token,
                               native_handle, manual_io_backend
include/alt/detail/file_types.hpp     file_kind, file_identity, file_status,
                                      directory_entry, open_flag, file_mode,
                                      io_open_args, io_rename_args
include/alt/detail/io_operation.hpp   the control block and its awaiter
include/alt/detail/io_handle.hpp      the refcounted descriptor
include/alt/detail/io_shared.hpp      slots, backend pointer, completion scheduler
src/file_io.cpp                probe, dispatch, slot accounting, drain-on-destroy
src/io_buffer.cpp
src/io_backend_thread_pool.cpp always compiled
src/io_backend_uring.cpp       #if defined(__linux__) && ALT_HAVE_IO_URING
src/io_backend_iocp.cpp        #ifdef _WIN32
```

`include/alt/detail/` is right for `io_operation.hpp` by the convention's own test: no
caller would include it without `async_file.hpp`.

**`detail/file_types.hpp` is a correction to this map, made during implementation, and
the reason is layering rather than visibility.** The first draft put `file_status`,
`directory_entry`, `open_flag` and `file_mode` in `async_file.hpp`. They cannot live
there: the thread-pool backend implements `stat_path` and `list_directory`, so it needs
those types, and it sits *below* the file layer — it cannot include `async_file.hpp`
without inverting the dependency. Leaving them in `async_file.hpp` would have forced
every later phase's backend to be edited by the task that defines the types, which the
plan explicitly forbids.

They are nonetheless **public API in `namespace alt`**, not `alt::detail`. That is
exactly what the convention anticipates: the subdirectory describes includability, not
visibility, and no caller would include `file_types.hpp` without also including
`async_file.hpp` or `file_io.hpp`. `native_handle` moves to `io_backend.hpp` for the
same reason — `io_request` names it.

## What is left to the builder

Decisions a sensible builder should make without coming back:

- Exact spelling of `open_flags` enumerators and the POSIX/Win32 mapping table.
- Whether `list_directory` batches internally (`getdents64` loop vs. `readdir`) — only
  its one-shot `std::vector` result is specified. If a directory large enough to
  matter shows up, the follow-up is a generator version once roadmap phase 6 lands, not
  a cursor bolted onto v1.
- The `error_code` category for backend-specific failures: a new `alt` category versus
  `std::system_category()`/`std::generic_category()`. Prefer the standard categories
  wherever errno or `GetLastError` maps cleanly, which is nearly everywhere.
- Internal ring sizing when `queue_depth` is 0.
- Whether `manual_io_backend` lives in `io_backend.hpp` or its own header.

## Testing

This component's tests carry more weight than its code, for the reason the constraints
section gives: its worst failure mode is invisible to both sanitizers.

**A `manual_io_backend`** is the centerpiece, in the spirit of `manual_scheduler`: the
test decides when each submitted operation completes, so the cancel-versus-complete
race is a sequence the test writes rather than a timing it hopes for. Every state of
the three-phase protocol gets a test that pins the ordering exactly:

| Ordering | What must be true |
|----------|-------------------|
| complete, then cancel | the body ran, and a stop requested after the drain finds no stop callback at all, because `await_resume` deregisters it before the chain continues past the await; the handler's `state != phase::waiting` guard (which also covers `detached`) is the second line behind that |
| cancel, then complete | the body never ran; the chain was abandoned; the completion finds `detached` and touches no frame |
| cancel while the completion is mid-post | the resumption runs once, and takes the abandon branch |
| cancel while parked on a slot | the operation never reaches the backend at all |
| completion fires inline inside `submit` | the resumption is still posted, not run on the submitting thread |
| `file_io` destroyed with operations in flight | the destructor returns once the backend is quiet, with resumptions possibly still queued |
| a resumption runs after `~file_io` returned | the permit release touches live `io_shared`; nothing reads freed memory |
| cancel, then the backend completes, then `async_file` is gone | the descriptor was still open for the backend's read, and is closed by the block that dropped the last reference |
| two operations, one slot, one released | the second submits on a **home** thread — asserted by thread id, not inferred |
| `run()` cancelled before `f` ran | `f` was never invoked, and its captures were destroyed on the worker |
| `run()` cancelled while `f` ran | `f` completed and its result was discarded with the block (its exception needs no test: the `exception_ptr` member dies with the invocation, so there is no path on which it survives) |

**The buffer-survives-teardown test is the one that justifies the design.** A read into
an `io_buffer::allocate` buffer, cancelled while in flight, with the backend writing
into that buffer *after* the chain is gone, and the buffer's contents asserted
afterwards. Per `feedback_coverage_is_assertions_not_arrangements` and
`feedback_measure_before_claiming`: the variant with the block's `shared_ptr` keepalive
removed must be built and measured, and must actually fail, before the test is claimed
to guard anything.

**The second-callback shape is mandatory, not optional.** CLAUDE.md records that
libstdc++'s `request_stop` skips relocking the callback list after the last callback,
which is what kept a use-after-free in `detail::stop_forwarder` invisible on Linux
including under ASan — and that
`CancellationRefTest.ATeardownWithASecondCallbackOnTheChainsTokenIsSafe` is built with
a second registration on the same token on purpose, which is what made it visible
again (0 passes in 40 `debug` runs with the keepalive removed). Our awaiter registers
a `std::stop_callback` on the chain's token, so the file-I/O teardown tests must be
built the same way or they will pass on Linux and hang on MSVC.

**A liveness sentinel in the operation block**, not just a sanitizer run, because a
use-after-free reached through a mutex lock is invisible to ASan *and* TSan: ASan does
not intercept glibc's `pthread_mutex_lock`, libstdc++'s `std::mutex` destructor is
trivial so no `pthread_mutex_destroy` is intercepted, and TSan drops its
synchronization variables on free (`2026-11-15-waiter-liveness-guard.md`, Review Focus
2). This block's teardown races are all behind its mutex, so they sit squarely in that
hole.

Following the `waiter_list` precedent exactly, and recorded here so nobody re-derives
it: **the sentinel member exists in every configuration** — making it conditional on
`NDEBUG` would change the class layout between translation units compiled with
different flags, an ODR violation in a library that ships headers and a `.deb` — while
**only the checks are `assert`-guarded**, and are therefore absent in Release. That
leaves a use-after-free reachable only in an optimized build as invisible as it is
today, which is a stated residual gap, not an oversight. The sentinel is stored before
the block's teardown, not after, so a concurrent touch is what gets caught.

**The fd-recycling test needs a real recycled descriptor to be worth anything.** A
closed fd that is merely closed gives `EBADF`, which is a benign failure; the dangerous
case is the number being reassigned to another file, where the read *succeeds* against
the wrong data. The test must close and then deliberately reopen to capture the same
descriptor number before asserting the operation did not read through it. A test that
only checks for `EBADF` would pass against the broken design.

Beyond that: a real-filesystem fixture in a temp directory for each operation,
including the error paths (ENOENT, EACCES, EISDIR, a rename across devices, a short
read at EOF); death tests for the stated preconditions using the existing
`death_test_support.hpp`; a test that `backend()` reports `thread_pool` when the probe
is forced to fail; and, on Linux with a new enough kernel, every operation test run
against both the uring and thread-pool backends from the same test body.

New files `tests/test_file_io.cpp`, `tests/test_async_file.cpp`,
`tests/test_io_buffer.cpp`, `tests/test_io_backend.cpp`, registered in the `alt_tests`
sources list.

## Staging

Three phases, each with its own design spec, plan and pull request, per the roadmap's
ordering principle. **Phase 7a alone unblocks `cpwf`**, which is the point of putting
it first: it replaces `StoreWorker` and `CommitOnWorker` outright and makes the
`pread` serve path and the per-request `open`/`fstat` asynchronous on every platform.

| Phase | Contents |
|-------|----------|
| **7a** | `io_buffer`, `io_backend`, `manual_io_backend`, the operation block and awaiter, `file_io`, `async_file`, the thread-pool backend, the escape hatch, and the whole test suite above. |
| **7b** | The io_uring backend behind the same interface: ring setup, runtime probe, per-operation fallback to the pool arm. No public API change. |
| **7c** | The IOCP backend for read and write; everything else already runs on the pool arm. `automatic` selects it on Windows, and adopted files are routed to the pool arm (issue #53). One public API addition: `async_file::backend()`, which reports the backend serving a file; otherwise no public API change. |

7b and 7c change no public surface beyond what the table states, which is the test of
whether 7a's interface was drawn in the right place. If either needs to widen it, that is
a design error in 7a worth a revision rather than a leak. 7c's one addition,
`async_file::backend()`, is deliberate rather than such a leak: once an IOCP service
routes adopted files to the pool arm (issue #53), one service no longer has one backend,
and a caller can ask which serves a given file.

## Housekeeping

- `docs/file_io.md`, `docs/async_file.md`, `docs/io_buffer.md`,
  `docs/io_backend.md` — one per component, as every other component has.
- `2026-09-10-coroutines-roadmap.md` gains phase 7 and loses nothing: this is the
  first phase in it that is not a coroutine primitive, so the roadmap's framing needs
  a line acknowledging the scope expansion the issue calls out.
- `README.md`'s component list, and its note that alt needs nothing but a compiler and
  threads, which stays true under the no-dependency recommendation above.
- **Version bumps: open, and the author's to settle** (see the note at the end of this
  document). 7a is a new component and so clearly a minor, 1.14.0 → 1.15.0. What 7b and
  7c get is the real question: they add no public API, which argues patch, but each
  adds compiled code and may grow internal block types that `src/*.cpp` embed into the
  shipped `libalt`. The waiter-liveness-guard plan was written for a patch on exactly
  that reasoning and executed as a **minor** once `sizeof(detail::waiter_list)` was
  seen to grow, so "no public API change" is not sufficient grounds for a patch in this
  library.

## Record: what a builder review changed, and what is still open

A builder read the first draft of this document cold and reported where it would have
had to guess. Seven of its findings are resolved above; they are listed here so the
resolutions can be checked rather than taken on trust, since several are corrections
rather than clarifications.

| Finding | Resolution | Section |
|---------|-----------|---------|
| `~file_io` waiting on posts to the home scheduler deadlocks when destroyed on the loop thread | Waits for backend quiescence only; `io_shared` refcounted so late resumptions stay safe; two preconditions, no new API | Shutdown |
| The descriptor was not co-owned, so a cancelled frame could close — or let the OS recycle — an fd a worker was still reading through | Refcounted `io_handle` co-owned by every block; closed by whoever drops the last reference | Operation state; Closing a file |
| `run()`'s callable lived in the frame cancellation destroys — the same use-after-free the design exists to prevent | `f` moved into the block, type-erased; drop-on-worker made an explicit guarantee the consumer already relies on | The escape hatch's lifetime |
| `io_backend` was named in the file layout and never specified, though 7b/7c are forbidden to widen it | Specified in full: callback on an I/O thread, may fire inline, exactly once, token-based cancel | The `io_backend` contract |
| `manual_io_backend` was unreachable — an enum value cannot conjure an instance — leaving the centerpiece test suite unbuildable | `create` overload taking `io_backend&` | The `io_backend` contract |
| Releasing the backpressure permit from a backend callback would resume the next operation on an I/O thread, straight into the top anti-goal | Permit released on the home thread, before `resume_or_abandon`; submission-on-a-home-thread is now a stated invariant with a test | Backpressure |
| No thread-affinity contract, where every other alt primitive states one per member | Stated as a table | Thread affinity |
| Sentinel shipped or `assert`-only? | `assert`-only, member always present, per the `waiter_list` precedent | Testing |

**Still open, and not mine to decide: the version bumps for 7b and 7c.** CLAUDE.md
requires the question be asked rather than assumed, and the precedent above shows the
obvious answer was wrong once already.

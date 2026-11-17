# Death tests for task_group's and task_completion_source's preconditions

Short design. Issue #29. 2026-11-17.

## Intent

Five documented preconditions assert in a debug build and no test observes any of
them. Both test files say so in their own comments rather than leaving it silent.
This pins the debug-build guard, so an assert that is later weakened, renamed or
deleted fails a test instead of passing quietly.

The asserts, re-verified against the code on 2026-11-17:

| Assert | Message matched |
|--------|-----------------|
| `src/task_group.cpp:12` destructor, live tasks | `destroying an alt::task_group with live tasks` |
| `src/task_group.cpp:17` caller's `start_options::token` | `alt::task_group supplies its own stop token` |
| `src/task_group.cpp:18` caller's `start_options::on_completion` | `alt::task_group owns start_options::on_completion` |
| `include/alt/task_completion_source.hpp:293` set twice | `setting an alt::task_completion_source twice` |
| `include/alt/task_completion_source.hpp:272` await twice | `awaiting an alt::task_completion_source twice` |

## Why now

It used to need scaffolding that did not exist. `tests/death_test_support.hpp` now
carries the whole of it: `asserts_are_compiled_in`, `thread_sanitizer_is_active` and
`scoped_death_test_style`. Including that header is the setup.

## Goals

- One death test per assert, five in total, each matching its own assert's message.
- The suite's house pattern, from `tests/test_waiter_list.cpp:683-720`: the NDEBUG and
  ThreadSanitizer skip guards, a scoped `"threadsafe"` style, a plain
  `TEST(<Component>DeathTest, ...)`.
- The two "known gap" comment blocks in the test files are replaced by notes saying
  what the new tests pin and what they still do not reach.
- All eight CI gates green, `Coverage` included.

## Non-goals

- **Release-build behavior.** These asserts compile away under `NDEBUG`, so the
  release no-op of a second set is not reachable from any Debug preset. The tests skip
  under `NDEBUG` and say which guard is gone.
- **Library changes.** No assert text changes, no new member, no new sentinel. The
  tests observe what is there.
- **Concurrent shapes.** Two threads setting at once, and a spawn racing the
  destructor, are not deterministically reachable from a test. They stay uncovered and
  are stated as uncovered, not implied by the tests that land.

## Anti-goals

What would make this a failure even though the tests pass:

- **A test that dies for the wrong reason.** `_GLIBCXX_ASSERTIONS` is on by default
  here and aborts inside libstdc++ with its own message, which
  `tests/test_waiter_list.cpp:705-712` records hitting. Every matcher is this
  project's own assert text, never a generic `Assertion`.
- **A suppression comment carrying a number nobody measured.** The existing
  `NOLINTNEXTLINE(readability-function-cognitive-complexity)` comments carry measured
  figures from pinned `clang-tidy-19`. The new ones are measured, not copied.
- **Rewriting `tests/.clang-tidy`.** Append only.

## Constraints

- `clang-tidy-19` and `clang++-19` pinned, per `CLAUDE.md`'s clang-tidy section; the
  whole-project gate aborts at the first finding, so each changed file is also checked
  directly with `-isystem` for the GoogleTest headers.
- No new test files, so no `tests/CMakeLists.txt` change: both files are already in
  the `alt_tests` sources list.
- The suite's death tests take no fixture, so these get no
  `reset_coroutine_test_state()`. The two that build coroutine frames must therefore
  not read `live_frames` or `destruction_order`.
- `tests/death_test_support.hpp` is included, not re-derived. Skip messages stay local
  to each file and name that file's own assert, as the header's documentation requires.

## Approaches

Only the await-twice assert had a real choice of reach.

- **A. Call `source.operator co_await()` twice directly.** No coroutine. Pins the
  assert exactly, but reads as a test of a mechanism rather than of a mistake.
- **B. Start two `waiting_root` coroutines on one source.** *Chosen.* The first parks,
  the second's `co_await` aborts. This is the mistake a caller actually makes, and the
  helper already exists in the file.

The remaining four have one honest reach each: for the two `spawn` asserts and
set-twice, the direct call *is* the caller's mistake; the destructor's needs a parked
task either way.

Separately, and recorded as my call rather than a decision asked for: the tests go in
the two existing test files, beside the gap comments they retire, rather than in a new
`test_*_death.cpp`.

## The tests

`TEST(TaskGroupDeathTest, ...)` in `tests/test_task_group.cpp`:

1. **Destroying a group with a live task aborts.** `parked_root` on an `async_event`
   never set. **The declaration order inside the statement is load-bearing**: the
   event is declared *before* the group, so the group is destroyed first, while the
   task is still parked. Declared after, the event is destroyed first, releases the
   waiter, the count reaches zero and the destructor passes — a test that would fail
   loudly, but for a reason that reads as the assert being wrong.
2. **Spawning with a caller's stop token aborts.** A token from a live
   `std::stop_source`, which is what makes `stop_possible()` true.
3. **Spawning with a caller's completion hook aborts.** An `on_completion.invoke` of
   the caller's own.

`TEST(TaskCompletionSourceDeathTest, ...)` in `tests/test_task_completion_source.cpp`:

4. **Setting a source twice aborts.** `set_value` on an already-set source. No
   coroutine.
5. **Awaiting a source twice aborts.** Approach B. The parked first waiter's record
   must outlive the handle that cancels it, so the records are declared before the
   handle.

## Left to the builder

- The measured figure in each `NOLINTNEXTLINE` suppression, and whether every one of
  the five needs the suppression at all.
- The exact wording of each file's two skip messages.
- Whether `start_options::on_completion`'s `invoke` signature makes a file-local
  function or a captureless lambda the cleaner hook in test 3.

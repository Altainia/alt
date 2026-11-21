find_package(GTest QUIET)
find_package(Threads REQUIRED)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.16.0
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# When building with MSan, GoogleTest must also be instrumented so that
# reads inside uninstrumented gtest internals are not flagged as false positives.
if(ALT_SANITIZE STREQUAL "memory")
    foreach(_gtest_target IN ITEMS gtest gtest_main gmock gmock_main)
        if(TARGET ${_gtest_target})
            target_compile_options(${_gtest_target} PRIVATE
                -fsanitize=memory -fno-omit-frame-pointer -fsanitize-memory-track-origins
            )
            target_link_options(${_gtest_target} PRIVATE -fsanitize=memory)
        endif()
    endforeach()
endif()

add_executable(alt_tests
    test_algorithm.cpp
    test_version.cpp
    test_flags.cpp
    test_functional.cpp
    test_type_traits.cpp
    test_utility.cpp
    test_scope.cpp
    test_memory.cpp
    test_memory_header_hygiene.cpp
    test_memory_header_no_glibc.cpp
    test_clearing_string.cpp
    test_clearing_string_instrumented.cpp
    test_transcode.cpp
    test_projection.cpp
    test_concepts.cpp
    test_hash.cpp
    test_digest.cpp
    test_sha1.cpp
    test_sha2.cpp
    test_base64.cpp
    test_awaitable.cpp
    test_coroutine.cpp
    test_scheduler.cpp
    test_work_queue.cpp
    test_waiter_list.cpp
    test_async_wait.cpp
    test_async_event.cpp
    test_combinator_state.cpp
    test_combinator_election.cpp
    test_completion_counter.cpp
    test_async_latch.cpp
    test_async_semaphore.cpp
    test_async_mutex.cpp
    test_task_completion_source.cpp
    test_task_group.cpp
    test_run_loop.cpp
    test_thread_pool.cpp
    test_io_buffer.cpp
    test_io_backend.cpp
    test_io_handle.cpp
    test_io_operation.cpp
    test_async_file.cpp
    test_file_io.cpp
    test_cancellation.cpp
    test_cancellation_ref.cpp
    test_task.cpp
    test_sync_wait.cpp
    test_cancellable_task.cpp
    test_scheduler_integration.cpp
    test_combinator_results.cpp
    test_when_all.cpp
    test_when_any_result.cpp
    test_when_any.cpp
)

target_link_libraries(alt_tests
    PRIVATE
        alt::alt
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(alt_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

alt_add_warnings(alt_tests)

# MSVC's optimizer reports C4702 "unreachable code" after inlining, and test_scope.cpp
# is built to provoke exactly that: it instantiates the scope guards and
# unique_resource with types whose copy constructor always throws, to exercise
# P0052r10's exception-safety paths, so the member initializers after those copies
# really are unreachable in those instantiations. That is a property of the test types,
# not of scope.hpp, so the suppression is scoped to this one file and only appears
# from /O2 upward, where the backend analysis runs.
if(MSVC)
    set_source_files_properties(test_scope.cpp PROPERTIES COMPILE_OPTIONS /wd4702)
endif()

# MSVC's AddressSanitizer runtime is a DLL beside cl.exe, which is on PATH only inside
# a Developer Command Prompt. Copied next to the test executable instead, because
# gtest_discover_tests runs that executable to list the tests before any ctest
# environment applies, and a missing runtime fails discovery with 0xc0000135. Newer
# toolsets use one runtime for every configuration and older ones a _dbg_ variant for
# Debug, so whichever ones the toolset ships are copied.
if(MSVC AND ALT_SANITIZE STREQUAL "address")
    get_filename_component(_alt_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _alt_asan_runtimes "${_alt_msvc_bin}/clang_rt.asan*dynamic-x86_64.dll")
    if(NOT _alt_asan_runtimes)
        message(FATAL_ERROR "ALT_SANITIZE=address: no AddressSanitizer runtime DLL beside ${CMAKE_CXX_COMPILER}")
    endif()
    add_custom_command(TARGET alt_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_alt_asan_runtimes} $<TARGET_FILE_DIR:alt_tests>
        VERBATIM
    )
endif()

# GCC at -O2/-O3 reports -Wmaybe-uninitialized from inside libstdc++'s <stop_token>
# for a cancellable_task that co_awaits when_all. The read it describes cannot happen:
# the optional<stop_callback> in wait_awaiter::await_suspend is emplaced into a fresh,
# empty optional. It reproduces against main's headers with a four-line cancellable_task
# + when_all translation unit and no cancellation_ref.hpp, while main's own much larger
# test_when_all.cpp compiles clean at -O2 against the same headers (both measured during
# PR #46), so it tracks the translation unit's inlining context rather than the shape. Scoped to this one file and
# to GNU so every other translation unit keeps the warning.
#
# This suppression is load-bearing in CI, not inert -- which is what issue #44 asked, and
# it is now measured rather than inferred:
#
#   - CI's ubuntu-latest is Ubuntu 26.04 with GNU 15.2.0 (read off the configure step of
#     the "Build & Test (optimized)" job), the same version that fires it locally.
#   - Deleting these four lines and building `cmake --preset release-tests
#     -DALT_WARNINGS_AS_ERRORS=ON` -- the optimized job's own invocation, -O3 -DNDEBUG --
#     fails with "error: ... _M_ptr may be used uninitialized [-Werror=maybe-uninitialized]"
#     at /usr/include/c++/15/stop_token:449. With the lines present the same build is clean.
#   - GCC 16 (trunk, 16.0.1 20260322) still fires it, at stop_token:448, so this is not
#     fixed upstream and the suppression cannot be narrowed to GCC 15. A search of GCC
#     Bugzilla found no report for this false positive; none has been filed from here.
#
# Only the optimized job is affected: every other GNU job in CI (debug, asan, tsan,
# coverage) is -O0, where the warning does not fire.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(test_cancellation_ref.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()
alt_add_sanitizers(alt_tests)
alt_add_coverage(alt_tests)
alt_add_coverage_target(${CMAKE_BINARY_DIR})

if(ALT_CLANG_TIDY)
    set_target_properties(alt_tests PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_PATH}")
endif()

include(GoogleTest)

# The backstop is ctest's 1500s default without this. Several concurrency tests
# legitimately wait on a count or a latch rather than polling with a timeout, so a
# regression in the property they pin makes them hang instead of fail. A modest
# timeout turns a 25-minute silent stall into a prompt, named failure.
#
# Headroom is deliberate and large. The slowest test in the suite is
# sha2.hashes_the_one_million_character_vector at 0.79s under ThreadSanitizer, the
# slowest preset; the slowest concurrency test is at or under 0.5s there, and the
# longest deliberate sleep in the suite is 50ms. 60s is roughly 75x the slowest
# legitimate test, so it cannot fire on a slow machine, and it is short enough to
# report a hang while the build is still being watched.
gtest_discover_tests(alt_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60
)

# Compile checks for alt::task's [[nodiscard]] await_resume (issue #61). A diagnostic is
# not observable from a running test, so these build a probe and judge the build: the
# discarding variant must fail and the cast-to-void control must succeed, and the control
# is what lets the first mean "the discard warned" rather than "the probe is broken".
# Only that one diagnostic is made an error, so an unrelated warning cannot decide either
# result. EXCLUDE_FROM_ALL keeps them out of the normal build, and out of the clang-tidy
# gate, which builds without running ctest.
if(MSVC)
    set(_alt_discard_as_error /we4834)
else()
    set(_alt_discard_as_error -Werror=unused-result)
endif()
foreach(_alt_variant IN ITEMS discard control)
    add_library(task_await_discard_${_alt_variant} OBJECT EXCLUDE_FROM_ALL compile_checks/task_await_discard.cpp)
    target_link_libraries(task_await_discard_${_alt_variant} PRIVATE alt)
    target_compile_options(task_await_discard_${_alt_variant} PRIVATE ${_alt_discard_as_error})
    add_test(NAME task_await_discard_${_alt_variant}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target task_await_discard_${_alt_variant} --config $<CONFIG>
    )
    # Two builds of one tree at once would contend for the generator's own lock.
    set_tests_properties(task_await_discard_${_alt_variant} PROPERTIES RESOURCE_LOCK alt_build_tree TIMEOUT 300)
endforeach()
target_compile_definitions(task_await_discard_discard PRIVATE ALT_PROBE_DISCARD)
set_tests_properties(task_await_discard_discard PROPERTIES WILL_FAIL TRUE)

# A CMake-level test rather than a GoogleTest case, because what it exercises is not
# reachable from C++: find_package's version comparison, and the explanatory note that
# the root CMakeLists.txt appends to the generated AltConfigVersion.cmake. It installs
# this build to a scratch prefix under the build tree and configures the tiny consumer
# project in package/consumer against it once per case.
#
# It exists because two defects in that note shipped past careful hand-checking and
# were caught only by building a real consumer: a remedy reading "install <version> or
# newer", which SameMinorVersion makes false since it rejects a later minor as firmly
# as an earlier one, and a guard that fired the note on a versionless
# find_package(Alt), announcing a refusal that was not happening. Both are cases in
# the driver, and each was re-checked by mutation: dropping either guard, or weakening
# the remedy, fails this test by name.
#
# TIMEOUT is its own rather than the 60s the GoogleTest cases carry, because the cost
# here is an install plus six nested CMake configures rather than a unit test's. That
# cost turned out to be small: 1.0s for the whole test on the development machine,
# where one fresh consumer configure measures 0.19s. The timeout is set far above that
# rather than near it, because a cold CI runner performing full compiler-ABI detection
# per case is a different number and this measurement says nothing about it.
add_test(NAME package_version_gate
    COMMAND ${CMAKE_COMMAND}
        -DALT_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DALT_BUILD_CONFIG=$<CONFIG>
        -DCONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package/consumer
        -DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/package-version-gate
        -DALT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/package/run_version_gate_test.cmake
)
set_tests_properties(package_version_gate PROPERTIES TIMEOUT 300)

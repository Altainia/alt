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
    test_clearing_string.cpp
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

alt_add_warnings(alt_tests)
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
        -DCONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package/consumer
        -DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/package-version-gate
        -DALT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/package/run_version_gate_test.cmake
)
set_tests_properties(package_version_gate PROPERTIES TIMEOUT 300)

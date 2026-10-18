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
    test_run_loop.cpp
    test_thread_pool.cpp
    test_cancellation.cpp
    test_task.cpp
    test_sync_wait.cpp
    test_cancellable_task.cpp
    test_scheduler_integration.cpp
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
# slowest preset; every concurrency test is at or under 0.08s there, and the
# longest deliberate sleep in the suite is 50ms. 60s is roughly 75x the slowest
# legitimate test, so it cannot fire on a slow machine, and it is short enough to
# report a hang while the build is still being watched.
gtest_discover_tests(alt_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60
)

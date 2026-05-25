find_package(GTest QUIET)

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
)

target_link_libraries(alt_tests
    PRIVATE
        alt::alt
        GTest::gtest_main
)

alt_add_warnings(alt_tests)
alt_add_sanitizers(alt_tests)
alt_add_coverage(alt_tests)
alt_add_coverage_target(${CMAKE_BINARY_DIR})

if(ALT_CLANG_TIDY)
    set_target_properties(alt_tests PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_PATH}")
endif()

include(GoogleTest)
gtest_discover_tests(alt_tests DISCOVERY_MODE PRE_TEST)

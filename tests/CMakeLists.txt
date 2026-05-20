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

add_executable(alt_tests
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

include(GoogleTest)
gtest_discover_tests(alt_tests)

cmake_minimum_required(VERSION 3.25)
project(Alt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/compiler_checks.cmake)
include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/coverage.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ALT_CLANG_TIDY "Run clang-tidy on all sources during build" OFF)
option(ALT_CPPCHECK "Add a 'cppcheck' custom build target" OFF)

if(ALT_CLANG_TIDY)
    find_program(CLANG_TIDY_PATH clang-tidy REQUIRED)
endif()

file(GLOB_RECURSE ALT_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(alt ${ALT_SOURCES})
add_library(alt::alt ALIAS alt)

target_include_directories(alt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

alt_add_warnings(alt)
alt_add_sanitizers(alt)
alt_add_coverage(alt)

if(ALT_CLANG_TIDY)
    set_target_properties(alt PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_PATH}")
endif()

if(ALT_CPPCHECK)
    find_program(CPPCHECK_PATH cppcheck REQUIRED)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_PATH}
            --enable=all
            --std=c++23
            --error-exitcode=1
            --suppress=missingIncludeSystem
            --suppress=unmatchedSuppression
            --suppress=unusedFunction
            -I ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/include/alt
            ${CMAKE_SOURCE_DIR}/src
        COMMENT "Running cppcheck static analysis"
        VERBATIM
    )
endif()

install(TARGETS alt
    EXPORT AltTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/alt
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT AltTargets
    FILE AltTargets.cmake
    NAMESPACE alt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Alt
)

configure_package_config_file(
    cmake/AltConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AltConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Alt
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AltConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AltConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AltConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Alt
)

option(ALT_BUILD_TESTS "Build the test suite" ON)
if(ALT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

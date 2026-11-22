cmake_minimum_required(VERSION 3.25)
project(AltPackageConsumer LANGUAGES CXX)

# The gate under test is find_package's version comparison and the explanatory note
# appended to AltConfigVersion.cmake, so this project builds nothing and links
# nothing: configuring it either succeeds or fails, and its output is the evidence.
#
# PATHS with NO_DEFAULT_PATH is deliberate. Without it the search continues into the
# system prefixes after this one refuses, so any other Alt installed on the machine
# can satisfy the request and turn an expected refusal into an acceptance -- the
# development machine this was written on has an old Alt in /usr/local. Pointing at
# the scratch prefix makes the test about THIS build's version file. Passing the
# prefix root rather than its lib/cmake/Alt keeps it correct where CMAKE_INSTALL_LIBDIR
# is lib64, find_package appending the standard suffixes itself.
if("${ALT_WANT}" STREQUAL "")
    find_package(Alt REQUIRED PATHS "${ALT_PREFIX}" NO_DEFAULT_PATH)
    message(STATUS "consumer: accepted Alt ${Alt_VERSION} with no version requested")
elseif(ALT_QUIET)
    find_package(Alt ${ALT_WANT} QUIET PATHS "${ALT_PREFIX}" NO_DEFAULT_PATH)
    message(STATUS "consumer: quiet request, Alt_FOUND=${Alt_FOUND}")
else()
    find_package(Alt ${ALT_WANT} REQUIRED PATHS "${ALT_PREFIX}" NO_DEFAULT_PATH)
    message(STATUS "consumer: accepted Alt ${Alt_VERSION}")
endif()

# Configuring is not enough to catch a broken package config. An installed
# AltTargets.cmake may name an imported target the config never defined (on Linux,
# $<LINK_ONLY:PkgConfig::LIBURING>), and CMake resolves that only at generate time,
# when something links alt::alt. ALT_LINK adds that something, and the driver builds it.
if(ALT_LINK)
    add_executable(consumer main.cpp)
    target_link_libraries(consumer PRIVATE alt::alt)
    # A static alt built with a sanitizer or coverage keeps those runtime references
    # PRIVATE, so the consumer must carry the same link flags to resolve them.
    if(ALT_CONSUMER_LINK_FLAGS)
        target_link_options(consumer PRIVATE ${ALT_CONSUMER_LINK_FLAGS})
    endif()
endif()

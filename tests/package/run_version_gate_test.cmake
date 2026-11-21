# Driver for the package_version_gate test. Registered from tests/CMakeLists.txt,
# which explains why this is a CMake-level test rather than a GoogleTest case.
#
# It installs the build under test to a scratch prefix and configures the consumer
# project in tests/package/consumer against it once per case, asserting both the
# verdict (did find_package accept?) and the diagnostic (did the appended note say
# the right thing, or correctly stay silent?).

cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS ALT_BUILD_DIR CONSUMER_DIR SCRATCH ALT_VERSION)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "package_version_gate: ${_required} was not passed to this script")
    endif()
endforeach()

set(prefix "${SCRATCH}/prefix")
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${prefix}")

# A multi-config generator (Visual Studio) has no single build type to install, and
# cmake --install falls back to Release there, which ctest -C Debug never built.
# ALT_BUILD_CONFIG is the configuration ctest is testing; under a single-config
# generator it is the build type, and empty when that is unset.
set(_config_args "")
if(ALT_BUILD_CONFIG)
    set(_config_args --config "${ALT_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${ALT_BUILD_DIR}" --prefix "${prefix}" ${_config_args}
    RESULT_VARIABLE _install_rc
    OUTPUT_VARIABLE _install_out
    ERROR_VARIABLE _install_out
)
if(NOT _install_rc EQUAL 0)
    message(FATAL_ERROR
        "package_version_gate: installing the build under test to the scratch prefix "
        "failed (${_install_rc}). The rest of this test cannot run.\n${_install_out}")
endif()

set(failures "")

macro(record_failure _case _text)
    list(APPEND failures "[${_case}] ${_text}")
endmacro()

# Configures the consumer for one case. Sets CASE_RC and CASE_OUT in the caller.
function(run_case _want _quiet)
    string(MAKE_C_IDENTIFIER "want_${_want}_quiet_${_quiet}" _slug)
    set(_bin "${SCRATCH}/${_slug}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_DIR}" -B "${_bin}"
                "-DALT_WANT=${_want}" "-DALT_QUIET=${_quiet}" "-DALT_PREFIX=${prefix}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _so
        ERROR_VARIABLE _se
    )
    set(CASE_RC "${_rc}" PARENT_SCOPE)
    set(CASE_OUT "${_so}${_se}" PARENT_SCOPE)
endfunction()

# The note's own first line. Its presence proves this build's version file was the
# one consulted, which is what makes a refusal attributable rather than ambient.
set(note_marker "alt ${ALT_VERSION} is installed here")

string(REPLACE "." ";" _parts "${ALT_VERSION}")
list(GET _parts 0 major)
list(GET _parts 1 minor)
list(GET _parts 2 patch)
math(EXPR next_patch "${patch} + 1")
math(EXPR next_minor "${minor} + 1")
math(EXPR prev_minor "${minor} - 1")

# --- Case 1: the installed version exactly. Accepted, and the note stays silent. ---
run_case("${ALT_VERSION}" OFF)
if(NOT CASE_RC EQUAL 0)
    record_failure("exact ${ALT_VERSION}" "refused its own version:\n${CASE_OUT}")
elseif(NOT CASE_OUT MATCHES "accepted Alt ${ALT_VERSION}")
    record_failure("exact ${ALT_VERSION}" "configured but did not report acceptance:\n${CASE_OUT}")
endif()
if(CASE_OUT MATCHES "${note_marker}")
    record_failure("exact ${ALT_VERSION}" "the note fired on an ACCEPTED request:\n${CASE_OUT}")
endif()

# --- Cases 2-4: refusals. Each must name the series that WOULD satisfy it. ---
# A remedy phrased as "install ${ALT_VERSION} or newer" is false under
# SameMinorVersion, which rejects a later minor as firmly as an earlier one, so each
# case asserts its own requested series appears rather than the installed one.
set(refusals "${major}.${minor}.${next_patch}" "${major}.${next_minor}.0")
if(minor GREATER 0)
    list(APPEND refusals "${major}.${prev_minor}.0")
else()
    message(STATUS "package_version_gate: minor is 0, so the lower-minor case is skipped")
endif()

foreach(want IN LISTS refusals)
    string(REPLACE "." ";" _w "${want}")
    list(GET _w 0 w_major)
    list(GET _w 1 w_minor)

    run_case("${want}" OFF)
    if(CASE_RC EQUAL 0)
        record_failure("refuse ${want}"
            "was ACCEPTED against installed ${ALT_VERSION}. If another Alt satisfied it, the "
            "consumer's NO_DEFAULT_PATH is not isolating the scratch prefix:\n${CASE_OUT}")
    endif()
    if(NOT CASE_OUT MATCHES "${note_marker}")
        record_failure("refuse ${want}"
            "was refused with no explanation: the appended note did not fire:\n${CASE_OUT}")
    endif()
    if(NOT CASE_OUT MATCHES "same minor release \\(${major}\\.${minor}\\.x\\)")
        record_failure("refuse ${want}" "the note did not name the accepted series ${major}.${minor}.x:\n${CASE_OUT}")
    endif()
    if(NOT CASE_OUT MATCHES "Install alt ${w_major}\\.${w_minor}\\.x")
        record_failure("refuse ${want}"
            "the remedy did not name the REQUESTED series ${w_major}.${w_minor}.x, which is the "
            "only series that can satisfy this request:\n${CASE_OUT}")
    endif()
endforeach()

# --- Case 5: a QUIET consumer. Refused, and silently. ---
run_case("${major}.${next_minor}.0" ON)
if(NOT CASE_RC EQUAL 0)
    record_failure("quiet ${major}.${next_minor}.0"
        "a QUIET non-REQUIRED request failed the configure:\n${CASE_OUT}")
elseif(NOT CASE_OUT MATCHES "Alt_FOUND=0")
    record_failure("quiet ${major}.${next_minor}.0" "the package was found despite an incompatible request:\n${CASE_OUT}")
endif()
if(CASE_OUT MATCHES "${note_marker}")
    record_failure("quiet ${major}.${next_minor}.0" "the note printed to a QUIET consumer:\n${CASE_OUT}")
endif()

# --- Case 6: no version requested. Accepted, and silently. ---
# This one is why the note's guard exists: the generated comparison leaves
# PACKAGE_VERSION_COMPATIBLE FALSE here, and CMake accepts the package anyway, so a
# note keyed on that flag alone announces a refusal that is not happening.
run_case("" OFF)
if(NOT CASE_RC EQUAL 0)
    record_failure("no version" "a versionless find_package(Alt) was refused:\n${CASE_OUT}")
elseif(NOT CASE_OUT MATCHES "with no version requested")
    record_failure("no version" "configured but did not report acceptance:\n${CASE_OUT}")
endif()
if(CASE_OUT MATCHES "${note_marker}")
    record_failure("no version" "the note fired although no version was requested:\n${CASE_OUT}")
endif()

# --- Cases 7-8: linking alt::alt, and the same with the Linux dependency block stripped. ---
# Configuring a consumer cannot catch a package config that omits the imported target
# the export references (on Linux, $<LINK_ONLY:PkgConfig::LIBURING>): CMake resolves it
# only at generate time, when something links alt::alt. These cases build such a consumer.
#
# Both run on Linux (CMAKE_HOST_SYSTEM_NAME: this is a -P script, where CMAKE_SYSTEM_NAME
# is empty and a test on it would skip the cases everywhere), keyed on the platform and not on anything read from the install:
# a gate derived from the installed export would skip itself in exactly the case it
# exists to catch, an export that stopped naming the dependency. On Linux the export
# must name it, and its absence is a failure. Elsewhere there is no Linux block to
# strip and no liburing, so the cases are skipped.
file(GLOB_RECURSE _targets_file "${prefix}/*/AltTargets.cmake")
file(GLOB_RECURSE _config_file "${prefix}/*/AltConfig.cmake")
list(GET _targets_file 0 _targets_file)
list(GET _config_file 0 _config_file)
file(READ "${_targets_file}" _targets)

# Configures and builds the linking consumer against one prefix. Sets CASE_RC and
# CASE_OUT in the caller.
function(run_link_case _slug _prefix)
    set(_bin "${SCRATCH}/${_slug}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_DIR}" -B "${_bin}"
                "-DALT_WANT=" "-DALT_QUIET=OFF" "-DALT_PREFIX=${_prefix}" -DALT_LINK=ON
                "-DALT_CONSUMER_LINK_FLAGS=${ALT_CONSUMER_LINK_FLAGS}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _so
        ERROR_VARIABLE _se
    )
    if(_rc EQUAL 0)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${_bin}" ${_config_args}
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _bo
            ERROR_VARIABLE _be
        )
        string(APPEND _so "${_bo}${_be}")
    endif()
    set(CASE_RC "${_rc}" PARENT_SCOPE)
    set(CASE_OUT "${_so}${_se}" PARENT_SCOPE)
endfunction()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    if(NOT _targets MATCHES "PkgConfig::LIBURING")
        record_failure("link"
            "the installed AltTargets.cmake does not name PkgConfig::LIBURING, so the link cases below prove nothing about the dependency")
    endif()
    run_link_case(link_installed "${prefix}")
    if(NOT CASE_RC EQUAL 0)
        record_failure("link" "a consumer linking alt::alt did not configure and build:\n${CASE_OUT}")
    endif()

    # Negative control: the installed config with its Linux block removed must FAIL to
    # generate. If it passed, case 7 would not be able to detect the defect it exists for.
    set(stripped "${SCRATCH}/stripped")
    file(COPY "${prefix}/" DESTINATION "${stripped}")
    file(GLOB_RECURSE _stripped_config "${stripped}/*/AltConfig.cmake")
    list(GET _stripped_config 0 _stripped_config)
    file(READ "${_stripped_config}" _config)
    string(REGEX REPLACE "if\\(CMAKE_SYSTEM_NAME STREQUAL \"Linux\"\\).*\nendif\\(\\)\n" "" _config_stripped "${_config}")
    if(_config_stripped STREQUAL _config)
        record_failure("link without block"
            "could not strip the Linux block from the installed AltConfig.cmake; the control did not run")
    else()
        file(WRITE "${_stripped_config}" "${_config_stripped}")
        run_link_case(link_stripped "${stripped}")
        if(CASE_RC EQUAL 0)
            record_failure("link without block"
                "a consumer linked although the Linux dependency block was stripped, so the link case cannot detect a missing target")
        elseif(NOT CASE_OUT MATCHES "LIBURING")
            record_failure("link without block"
                "the control failed, but not over LIBURING, so it proved nothing about the stripped block:\n${CASE_OUT}")
        endif()
    endif()
else()
    message(STATUS "package_version_gate: this is not Linux, so the link cases are skipped")
endif()

if(failures)
    list(LENGTH failures _n)
    list(JOIN failures "\n\n" _report)
    message(FATAL_ERROR "package_version_gate: ${_n} assertion(s) failed.\n\n${_report}")
endif()

message(STATUS "package_version_gate: every case passed against installed ${ALT_VERSION}")

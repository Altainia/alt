# Fails unless a discovered test carries ENVIRONMENT=ALT_TEST_IO_BACKEND=thread_pool.
#
# Run as: cmake -DCTEST_COMMAND=<ctest> -DBUILD_DIR=<dir> -DTEST_NAME=<name> -P <this file>
#
# Nothing in the suite can see the property from inside: with it dropped, the discovered
# tests silently take `automatic`, which is io_uring on Linux, and every one of them still
# passes. So the property is read back from ctest's own description of the test.
foreach(_required IN ITEMS CTEST_COMMAND BUILD_DIR TEST_NAME)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} must be given")
    endif()
endforeach()

execute_process(
    COMMAND "${CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --show-only=json-v1 -R "^${TEST_NAME}$"
    OUTPUT_VARIABLE _json
    RESULT_VARIABLE _status
)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "ctest --show-only failed with ${_status}")
endif()

string(JSON _count LENGTH "${_json}" tests)
if(NOT _count EQUAL 1)
    message(FATAL_ERROR "expected exactly one discovered test named ${TEST_NAME}, found ${_count}; "
                        "if it was renamed, update tests/CMakeLists.txt")
endif()

string(JSON _property_count LENGTH "${_json}" tests 0 properties)
# Guarded ahead of the math: `foreach(_index RANGE -1)` does not error, it iterates 0 and
# then -1, and the FATAL_ERROR then comes from the string(JSON ... GET) below as an
# out-of-range message about index 0. Never a false pass, but a confusing failure instead of
# the informative one this script exists to give. (Measured; an earlier note here had the
# mechanism wrong and blamed foreach itself.)
if(_property_count EQUAL 0)
    message(FATAL_ERROR "${TEST_NAME} carries no test properties at all, so it has no "
                        "ENVIRONMENT and runs on whatever `automatic` gives")
endif()
math(EXPR _last "${_property_count} - 1")
foreach(_index RANGE ${_last})
    string(JSON _property_name GET "${_json}" tests 0 properties ${_index} name)
    if(_property_name STREQUAL "ENVIRONMENT")
        string(JSON _values GET "${_json}" tests 0 properties ${_index} value)
        # Matched WITH the quotes string(JSON ... GET ... value) hands back around each array
        # element. Unanchored, the bare substring would also accept ALT_TEST_IO_BACKEND=thread_pool_x;
        # no such value exists, but a guard whose whole purpose is not to pass for the wrong
        # reason should not be able to.
        if(_values MATCHES "\"ALT_TEST_IO_BACKEND=thread_pool\"")
            return()
        endif()
        message(FATAL_ERROR "${TEST_NAME} has ENVIRONMENT ${_values}, not ALT_TEST_IO_BACKEND=thread_pool")
    endif()
endforeach()
message(FATAL_ERROR "${TEST_NAME} has no ENVIRONMENT property, so it runs on whatever `automatic` gives")

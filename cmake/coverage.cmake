option(ALT_COVERAGE "Enable gcov code coverage instrumentation" OFF)

function(alt_add_coverage target)
    if(NOT ALT_COVERAGE)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "Code coverage (ALT_COVERAGE=ON) requires GCC/gcov. "
            "Current compiler: ${CMAKE_CXX_COMPILER_ID}. "
            "Use the debug-coverage preset or set CMAKE_CXX_COMPILER=g++."
        )
    endif()

    target_compile_options(${target} PRIVATE --coverage -fprofile-arcs -ftest-coverage)
    target_link_options(${target} PRIVATE --coverage)
endfunction()

function(alt_add_coverage_target binary_dir)
    if(NOT ALT_COVERAGE)
        return()
    endif()

    find_program(LCOV_PATH lcov REQUIRED)
    find_program(GENHTML_PATH genhtml REQUIRED)

    add_custom_target(coverage
        COMMENT "Generating coverage report in ${binary_dir}/coverage/"
        COMMAND ${LCOV_PATH} --zerocounters --directory ${binary_dir}
        COMMAND ctest --test-dir ${binary_dir} --output-on-failure
        COMMAND ${LCOV_PATH}
            --capture
            --directory ${binary_dir}
            --output-file ${binary_dir}/coverage.info
            --ignore-errors inconsistent,inconsistent,mismatch
        COMMAND ${LCOV_PATH}
            --remove ${binary_dir}/coverage.info
            "/usr/*"
            "*/googletest/*"
            "*/gtest/*"
            --output-file ${binary_dir}/coverage.filtered.info
            --ignore-errors inconsistent,inconsistent,unused
        COMMAND ${GENHTML_PATH}
            --output-directory ${binary_dir}/coverage
            ${binary_dir}/coverage.filtered.info
        VERBATIM
    )
endfunction()

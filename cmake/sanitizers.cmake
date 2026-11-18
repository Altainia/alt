set(ALT_SANITIZE "none" CACHE STRING "Sanitizer to enable: none, address, thread, memory")
set_property(CACHE ALT_SANITIZE PROPERTY STRINGS none address thread memory)

# MSVC supports AddressSanitizer alone: it has no UndefinedBehaviorSanitizer,
# ThreadSanitizer or MemorySanitizer. Its AddressSanitizer is set up here, once, for
# the whole build rather than per target as on GCC and Clang, for two reasons.
#
# MSVC's standard library annotates its containers for AddressSanitizer and records
# whether a translation unit was instrumented, so the linker refuses (LNK2038,
# "mismatch detected for 'annotate_vector'") to combine an instrumented test with an
# uninstrumented GoogleTest. Every target has to agree, GoogleTest's included.
#
# /RTC1, which CMake puts in the Debug flags, is incompatible with /fsanitize=address
# and has to leave the build-wide flags for the same reason.
if(MSVC AND NOT ALT_SANITIZE STREQUAL "none")
    if(NOT ALT_SANITIZE STREQUAL "address")
        message(FATAL_ERROR
            "ALT_SANITIZE=${ALT_SANITIZE} is not available with MSVC, which supports "
            "AddressSanitizer only (ALT_SANITIZE=address). Use GCC or Clang for "
            "ThreadSanitizer and MemorySanitizer."
        )
    endif()

    message(STATUS "alt: MSVC AddressSanitizer enabled; MSVC has no UndefinedBehaviorSanitizer to pair with it")
    foreach(_flags_var IN ITEMS CMAKE_CXX_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${_flags_var} "${${_flags_var}}")
    endforeach()
    add_compile_options(/fsanitize=address)
    # Incremental linking is incompatible with the ASan metadata; without this the
    # linker drops it itself and says so (LNK4300) on every link.
    add_link_options(/INCREMENTAL:NO)
endif()

function(alt_add_sanitizers target)
    if(ALT_SANITIZE STREQUAL "none" OR MSVC)
        # MSVC's AddressSanitizer is applied build-wide above.
        return()
    elseif(ALT_SANITIZE STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    elseif(ALT_SANITIZE STREQUAL "thread")
        # ThreadSanitizer is mutually exclusive with AddressSanitizer, and finds a
        # different class of defect: data races rather than invalid accesses. The
        # coroutine components are the only part of this library with concurrency,
        # so this is the build that covers them.
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
        )
    elseif(ALT_SANITIZE STREQUAL "memory")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR
                "MemorySanitizer (ALT_SANITIZE=memory) requires Clang. "
                "Current compiler: ${CMAKE_CXX_COMPILER_ID}. "
                "Use the debug-msan preset or set CMAKE_CXX_COMPILER=clang++."
            )
        endif()
        target_compile_options(${target} PRIVATE
            -fsanitize=memory
            -fno-omit-frame-pointer
            -fsanitize-memory-track-origins
        )
        target_link_options(${target} PRIVATE
            -fsanitize=memory
        )
    else()
        message(FATAL_ERROR
            "Unknown ALT_SANITIZE value '${ALT_SANITIZE}'. Valid values: none, address, thread, memory."
        )
    endif()
endfunction()

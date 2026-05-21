set(ALT_SANITIZE "none" CACHE STRING "Sanitizer to enable: none, address, memory")
set_property(CACHE ALT_SANITIZE PROPERTY STRINGS none address memory)

function(alt_add_sanitizers target)
    if(ALT_SANITIZE STREQUAL "none")
        return()
    elseif(ALT_SANITIZE STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
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
            "Unknown ALT_SANITIZE value '${ALT_SANITIZE}'. Valid values: none, address, memory."
        )
    endif()
endfunction()

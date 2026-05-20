option(ALT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(alt_add_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wnull-dereference
        -Woverloaded-virtual
        $<$<BOOL:${ALT_WARNINGS_AS_ERRORS}>:-Werror>
    )
endfunction()

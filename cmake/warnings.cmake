option(ALT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(alt_add_warnings target)
    if(MSVC)
        # /W4 is MSVC's counterpart to -Wall -Wextra, and it already includes the
        # implicit narrowing and signedness conversions (C4244, C4245, C4267, C4389)
        # that -Wconversion and -Wsign-conversion catch on GCC and Clang.
        #
        # /permissive- makes MSVC reject the non-standard constructs it otherwise
        # accepts, so code that compiles here also compiles on the other two. CMake
        # does not add it for /std:c++latest on its own.
        #
        # /Zc:__cplusplus makes __cplusplus report the real language level instead of
        # 199711L, and /Zc:preprocessor selects the standard-conforming preprocessor.
        # /utf-8 fixes the source and execution character sets, so the transcode
        # tests' non-ASCII literals do not depend on the machine's code page.
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            $<$<BOOL:${ALT_WARNINGS_AS_ERRORS}>:/WX>
        )
        return()
    endif()

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

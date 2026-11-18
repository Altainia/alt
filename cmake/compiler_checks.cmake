if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
        message(FATAL_ERROR "alt requires GCC 13 or later (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.0")
        message(FATAL_ERROR "alt requires Clang 19 or later (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # The oldest version the suite has been run against, not a guess at the oldest that
    # might work: 19.50 locally, 19.51 on CI's windows-latest. Visual Studio 2022's
    # 19.4x line is untested; see issue #43.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.50")
        message(FATAL_ERROR "alt requires MSVC 19.50 (Visual Studio 2026) or later (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
else()
    message(FATAL_ERROR "alt only supports GCC, Clang and MSVC (found ${CMAKE_CXX_COMPILER_ID})")
endif()

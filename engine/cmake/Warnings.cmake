# Keep diagnostics consistent for the library, tools and both test runners.
option(LLAVON_IME_WARNINGS_AS_ERRORS "Treat engine, tool and test warnings as errors" OFF)

function(llavon_ime_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # One list for GCC (Linux) and Clang (macOS) so both platforms check the
        # same things and a warning seen on one is not a surprise on the other.
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -Wformat=2 -Wundef -Wcast-align -Wswitch-enum
            # GCC's -Wextra enables this; Clang's does not.
            -Wimplicit-fallthrough
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor;-Woverloaded-virtual>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes;-Wstrict-prototypes>
        )
    endif()
    if(LLAVON_IME_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

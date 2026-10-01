# Keep diagnostics consistent for the library, tools and both test runners.
option(LLAVON_IME_WARNINGS_AS_ERRORS "Treat engine, tool and test warnings as errors" OFF)

function(llavon_ime_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            -Weverything
            # The project requires C++23, not source compatibility with old C++.
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++98-compat;-Wno-c++98-compat-pedantic;-Wno-pre-c++14-compat;-Wno-pre-c++17-compat;-Wno-pre-c++20-compat-pedantic>
            # C ABI tests use modern C declarations, not C90.
            $<$<COMPILE_LANGUAGE:C>:-Wno-declaration-after-statement>
            # Normal ABI padding is intentional; packed objects would be unsafe.
            -Wno-padded
            # Function-local tables and test registration have static lifetimes.
            -Wno-global-constructors -Wno-exit-time-destructors
            # Exhaustive enum switches deliberately omit a default. Keep
            # -Wswitch-enum enabled so newly added enum values are diagnosed.
            -Wno-switch-default
        )
        if(APPLE AND NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            # Apple's driver injects /usr/local/include even for native builds.
            target_compile_options(${target} PRIVATE -Wno-poison-system-directories)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -Wformat=2 -Wundef -Wcast-align -Wswitch-enum
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor;-Woverloaded-virtual>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes;-Wstrict-prototypes>
        )
    endif()
    if(LLAVON_IME_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

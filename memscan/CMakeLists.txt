cmake_minimum_required(VERSION 3.24)

project(llavon_ime_memscan VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "llavon-ime-memscan is Linux only (process_vm_readv)")
endif()

option(LLAVON_IME_MEMSCAN_BUILD_TESTS "Build the memscan self tests" ON)
# Same switch as the engine and the fcitx5 addon; off by default so the scan
# loop stays clean.
option(LLAVON_IME_DEBUG "Compile in debug logging (LLAVON_DEBUG_LOG)" OFF)

add_library(memscan_core STATIC
    src/anchor.cpp
    src/scan.cpp
)
target_include_directories(memscan_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(memscan_core PUBLIC cxx_std_23)
set_target_properties(memscan_core PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_options(memscan_core PRIVATE -Wall -Wextra -Wpedantic)
if(LLAVON_IME_DEBUG)
    target_compile_definitions(memscan_core PRIVATE LLAVON_IME_DEBUG=1)
endif()

add_executable(llavon-ime-memscan src/main.cpp)
find_package(nlohmann_json CONFIG REQUIRED)
target_compile_features(llavon-ime-memscan PRIVATE cxx_std_23)
set_target_properties(llavon-ime-memscan PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_options(llavon-ime-memscan PRIVATE -Wall -Wextra -Wpedantic)
if(LLAVON_IME_DEBUG)
    target_compile_definitions(llavon-ime-memscan PRIVATE LLAVON_IME_DEBUG=1)
endif()
target_link_libraries(llavon-ime-memscan PRIVATE memscan_core nlohmann_json::nlohmann_json)

install(TARGETS llavon-ime-memscan RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/llavon-ime")

if(LLAVON_IME_MEMSCAN_BUILD_TESTS)
    enable_testing()
    add_executable(memscan_tests tests/memscan_tests.cpp)
    target_compile_features(memscan_tests PRIVATE cxx_std_23)
    set_target_properties(memscan_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    target_compile_options(memscan_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(memscan_tests PRIVATE memscan_core)
    add_test(NAME memscan_tests COMMAND memscan_tests)
endif()

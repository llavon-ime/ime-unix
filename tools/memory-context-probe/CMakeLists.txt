cmake_minimum_required(VERSION 3.20)
project(context_memory_scan LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This experiment requires Linux process_vm_readv")
endif()

set(SCANNER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/src/memory/memory_scan.cpp")
add_executable(context_memory_scan ${SCANNER_SOURCE} probe.cpp)
target_include_directories(context_memory_scan PRIVATE ../../engine/src/memory)
target_compile_features(context_memory_scan PRIVATE cxx_std_23)
set_target_properties(context_memory_scan PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(context_memory_scan PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(memory_residue_probe ${SCANNER_SOURCE} memory_residue_probe.cpp)
target_include_directories(memory_residue_probe PRIVATE ../../engine/src/memory)
target_compile_features(memory_residue_probe PRIVATE cxx_std_23)
set_target_properties(memory_residue_probe PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(memory_residue_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

enable_testing()
add_test(NAME context_memory_scan COMMAND context_memory_scan --self-test)
set_tests_properties(context_memory_scan PROPERTIES TIMEOUT 30)

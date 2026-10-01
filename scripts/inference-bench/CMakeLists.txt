cmake_minimum_required(VERSION 3.24)
project(llavon_inference_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(../../engine/cmake/Warnings.cmake)
add_subdirectory(../../ime-core ime-core EXCLUDE_FROM_ALL)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(inference_bench bench.cpp)
target_link_libraries(inference_bench PRIVATE ime-core::ime-core nlohmann_json::nlohmann_json Threads::Threads)
# Treat the pinned dependency's public headers like the other external headers.
target_include_directories(inference_bench SYSTEM PRIVATE ../../ime-core/include)
llavon_ime_enable_warnings(inference_bench)

if(APPLE)
    add_executable(burst_bench burst.cpp)
    target_link_libraries(burst_bench PRIVATE ime-core::ime-core nlohmann_json::nlohmann_json Threads::Threads)
    target_include_directories(burst_bench SYSTEM PRIVATE ../../ime-core/include)
    llavon_ime_enable_warnings(burst_bench)
endif()

option(INFERENCE_BENCH_BUILD_RAWKEY "Build real-service raw-key latency probe" OFF)
if(INFERENCE_BENCH_BUILD_RAWKEY)
    set(INFERENCE_BENCH_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine"
        CACHE PATH "Engine source snapshot for reproducible raw-key experiments")
    set(LLAVON_IME_ENGINE_BUILD_TESTS OFF CACHE BOOL "Engine tests" FORCE)
    set(LLAVON_IME_TABLE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ime-core/table"
        CACHE PATH "Canonical reading tables")
    add_subdirectory("${INFERENCE_BENCH_ENGINE_SOURCE_DIR}" engine EXCLUDE_FROM_ALL)
    add_executable(rawkey_bench rawkey.cpp
        "${INFERENCE_BENCH_ENGINE_SOURCE_DIR}/tests/fake_host.cpp"
        "${INFERENCE_BENCH_ENGINE_SOURCE_DIR}/tests/rawkey/raw_key_harness.cpp")
    target_include_directories(rawkey_bench PRIVATE "${INFERENCE_BENCH_ENGINE_SOURCE_DIR}/tests")
    target_compile_definitions(rawkey_bench PRIVATE
        LLAVON_IME_TEST_TABLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../ime-core/table/bopomofo_char.json")
    target_link_libraries(rawkey_bench PRIVATE llavon_ime_engine)
    llavon_ime_enable_warnings(rawkey_bench)
endif()

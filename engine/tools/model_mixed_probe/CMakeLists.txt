cmake_minimum_required(VERSION 3.20)
project(model_mixed_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LLAVON_IME_ENGINE_BUILD_TESTS OFF CACHE BOOL "Build engine tests" FORCE)
set(LLAVON_IME_TABLE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../ime-core/table"
    CACHE PATH "Canonical reading tables")
add_subdirectory(../.. engine)

add_executable(model_mixed_probe
    probe.cpp
    ../../tests/fake_host.cpp
    ../../tests/rawkey/raw_key_harness.cpp)
target_link_libraries(model_mixed_probe PRIVATE llavon_ime_engine)
target_include_directories(model_mixed_probe PRIVATE ../../tests)
target_compile_definitions(model_mixed_probe PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
llavon_ime_enable_warnings(model_mixed_probe)

add_executable(typing_usability_probe
    ../../tests/rawkey/typing_usability_probe.cpp
    ../../tests/fake_host.cpp
    ../../tests/rawkey/raw_key_harness.cpp)
target_link_libraries(typing_usability_probe PRIVATE llavon_ime_engine)
target_include_directories(typing_usability_probe PRIVATE ../../tests)
target_compile_definitions(typing_usability_probe PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
llavon_ime_enable_warnings(typing_usability_probe)

add_executable(service_capability_probe service_capability_probe.cpp)
target_link_libraries(service_capability_probe PRIVATE llavon_ime_engine)
target_compile_definitions(service_capability_probe PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
llavon_ime_enable_warnings(service_capability_probe)

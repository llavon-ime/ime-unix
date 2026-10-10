cmake_minimum_required(VERSION 3.24)

project(ime_unix_service VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)
include("${CMAKE_CURRENT_SOURCE_DIR}/../engine/cmake/Warnings.cmake")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../common/protocol" "${CMAKE_CURRENT_BINARY_DIR}/protocol")

if(WIN32)
    message(FATAL_ERROR "ime-unix-service requires a Unix platform")
endif()

option(IME_UNIX_SERVICE_BUILD_TESTS "Build standalone service protocol/session tests" OFF)
option(LLAVON_IME_DEBUG "Compile in debug logging (predict/timing messages)" OFF)

set(IME_UNIX_SERVICE_TARGET llavon-ime-unix-service)

set(IME_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ime-core")
if(NOT EXISTS "${IME_CORE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ime-core submodule is missing; run git submodule update --init --recursive")
endif()

add_subdirectory("${IME_CORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ime-core" EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
    set(LLAVON_IME_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(LLAVON_IME_SQLITE_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR "FindSQLite3 did not provide an imported target")
endif()
find_package(utf8cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_executable(${IME_UNIX_SERVICE_TARGET}
    src/main.cpp
    src/platform/server_strategy.cpp
    src/platform/unix_socket_server.cpp
    src/engine/core_runtime.cpp
    src/engine/session_engine.cpp
    src/engine/stderr_logger.cpp
    src/session/session_manager.cpp
    src/training/commit_store.cpp
)
target_link_libraries(${IME_UNIX_SERVICE_TARGET} PRIVATE llavon_ime_protocol ime-core::ime-core Threads::Threads ${LLAVON_IME_SQLITE_TARGET}
    utf8cpp::utf8cpp unofficial-sodium::sodium)

target_compile_features(${IME_UNIX_SERVICE_TARGET} PRIVATE cxx_std_23)
set_target_properties(${IME_UNIX_SERVICE_TARGET} PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(${IME_UNIX_SERVICE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

llavon_ime_enable_warnings(${IME_UNIX_SERVICE_TARGET})
if(LLAVON_IME_DEBUG)
    target_compile_definitions(${IME_UNIX_SERVICE_TARGET} PRIVATE LLAVON_IME_DEBUG=1)
endif()

install(TARGETS ${IME_UNIX_SERVICE_TARGET} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
find_package(Git REQUIRED)
set(LLAVON_LORA_SUBMODULE "${CMAKE_CURRENT_SOURCE_DIR}/../lora-trainer")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/.." ls-files --stage -- lora-trainer
    OUTPUT_VARIABLE LLAVON_LORA_GITLINK OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LLAVON_LORA_INDEX_RESULT)
if(NOT LLAVON_LORA_INDEX_RESULT EQUAL 0 OR
   NOT LLAVON_LORA_GITLINK MATCHES "^160000 ([0-9a-f]+) 0[	]lora-trainer$")
    message(FATAL_ERROR "lora-trainer must be a pinned Git submodule")
endif()
set(LLAVON_LORA_COMMIT "${CMAKE_MATCH_1}")
string(LENGTH "${LLAVON_LORA_COMMIT}" LLAVON_LORA_COMMIT_LENGTH)
if(NOT LLAVON_LORA_COMMIT_LENGTH EQUAL 40)
    message(FATAL_ERROR "invalid lora-trainer submodule commit")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LLAVON_LORA_SUBMODULE}" rev-parse HEAD
    OUTPUT_VARIABLE LLAVON_LORA_CHECKOUT OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE LLAVON_LORA_CHECKOUT_RESULT)
if(NOT LLAVON_LORA_CHECKOUT_RESULT EQUAL 0 OR NOT LLAVON_LORA_CHECKOUT STREQUAL LLAVON_LORA_COMMIT)
    message(FATAL_ERROR "lora-trainer checkout does not match the pinned submodule commit; initialize submodules")
endif()
set(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH
    "${CMAKE_INSTALL_FULL_LIBDIR}/llavon-ime/tools/lora/llavon-lora"
    CACHE FILEPATH "Installed LoRA trainer executable matching the pinned submodule commit")

add_executable(llavon-ime-lora
    src/training/lora_cli.cpp
    src/training/libtorch_cache.cpp
    src/training/numeric_dataset.cpp
    src/training/commit_store.cpp
)
target_compile_features(llavon-ime-lora PRIVATE cxx_std_23)
set_target_properties(llavon-ime-lora PROPERTIES CXX_EXTENSIONS OFF)
llavon_ime_enable_warnings(llavon-ime-lora)
target_compile_definitions(llavon-ime-lora PRIVATE
    LLAVON_IME_LORA_PINNED_COMMIT="${LLAVON_LORA_COMMIT}"
    LLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LLAVON_IME_INSTALLED_LORA_TRAINER_PATH}")
target_include_directories(llavon-ime-lora PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(llavon-ime-lora PRIVATE llavon_ime_protocol ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
install(TARGETS llavon-ime-lora RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
add_subdirectory(src/training/native)
install(DIRECTORY "${IME_CORE_SOURCE_DIR}/table/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service")
install(FILES "${IME_CORE_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service"
    RENAME ime-core-LICENSE.txt)

if(IME_UNIX_SERVICE_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ime_trainer_release_discovery_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/trainer_release_discovery_tests.py"
        "$<TARGET_FILE:llavon-ime-lora>" "${LLAVON_LORA_COMMIT}")
    set_tests_properties(ime_trainer_release_discovery_tests PROPERTIES TIMEOUT 40)
    add_test(NAME ime_service_lifecycle_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/service_lifecycle_tests.py"
        "$<TARGET_FILE:${IME_UNIX_SERVICE_TARGET}>" "${IME_CORE_SOURCE_DIR}/table")
    set_tests_properties(ime_service_lifecycle_tests PROPERTIES TIMEOUT 40)
    add_executable(ime_worker_pool_tests tests/worker_pool_tests.cpp)
    target_include_directories(ime_worker_pool_tests PRIVATE src)
    target_compile_features(ime_worker_pool_tests PRIVATE cxx_std_23)
    target_link_libraries(ime_worker_pool_tests PRIVATE Threads::Threads)
    llavon_ime_enable_warnings(ime_worker_pool_tests)
    add_test(NAME ime_worker_pool_tests COMMAND ime_worker_pool_tests)
    set_tests_properties(ime_worker_pool_tests PROPERTIES TIMEOUT 10)
    add_executable(ime_protocol_compatibility_tests tests/protocol_compatibility_tests.cpp)
    target_link_libraries(ime_protocol_compatibility_tests PRIVATE llavon_ime_protocol)
    target_compile_features(ime_protocol_compatibility_tests PRIVATE cxx_std_23)
    target_include_directories(ime_protocol_compatibility_tests PRIVATE src ../engine/src)
    llavon_ime_enable_warnings(ime_protocol_compatibility_tests)
    add_test(NAME ime_protocol_compatibility_tests COMMAND ime_protocol_compatibility_tests)
    add_executable(ime_unix_service_tests
        tests/protocol_tests.cpp
        src/engine/core_runtime.cpp
        src/engine/session_engine.cpp
        src/engine/stderr_logger.cpp
        src/session/session_manager.cpp
        src/training/commit_store.cpp
        src/training/numeric_dataset.cpp
        src/training/lora_history_lca.cpp
    )
    target_compile_features(ime_unix_service_tests PRIVATE cxx_std_23)
    llavon_ime_enable_warnings(ime_unix_service_tests)
    target_include_directories(ime_unix_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(ime_unix_service_tests PRIVATE
        IME_UNIX_SERVICE_TEST_TABLE_DIR="${IME_CORE_SOURCE_DIR}/table")
    target_link_libraries(ime_unix_service_tests PRIVATE llavon_ime_protocol ime-core::ime-core Threads::Threads ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
    add_test(NAME ime_unix_service_tests COMMAND ime_unix_service_tests)
endif()

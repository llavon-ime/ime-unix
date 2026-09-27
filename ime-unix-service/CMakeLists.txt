cmake_minimum_required(VERSION 3.24)

project(ime_unix_service VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

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
    src/pipe/protocol.cpp
    src/engine/core_runtime.cpp
    src/engine/session_engine.cpp
    src/engine/stderr_logger.cpp
    src/session/session_manager.cpp
    src/training/commit_store.cpp
)
target_link_libraries(${IME_UNIX_SERVICE_TARGET} PRIVATE ime-core::ime-core Threads::Threads ${LLAVON_IME_SQLITE_TARGET}
    utf8cpp::utf8cpp unofficial-sodium::sodium)

target_compile_features(${IME_UNIX_SERVICE_TARGET} PRIVATE cxx_std_23)
set_target_properties(${IME_UNIX_SERVICE_TARGET} PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(${IME_UNIX_SERVICE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_options(${IME_UNIX_SERVICE_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
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
    src/pipe/protocol.cpp
)
target_compile_features(llavon-ime-lora PRIVATE cxx_std_23)
target_compile_definitions(llavon-ime-lora PRIVATE
    LLAVON_IME_LORA_PINNED_COMMIT="${LLAVON_LORA_COMMIT}"
    LLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LLAVON_IME_INSTALLED_LORA_TRAINER_PATH}")
target_include_directories(llavon-ime-lora PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(llavon-ime-lora PRIVATE ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
install(TARGETS llavon-ime-lora RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
add_executable(llavon-ime-lora-gui src/training/lora_gui.cpp src/training/commit_store.cpp src/pipe/protocol.cpp)
target_compile_features(llavon-ime-lora-gui PRIVATE cxx_std_23)
target_compile_definitions(llavon-ime-lora-gui PRIVATE
    LLAVON_IME_LORA_PINNED_COMMIT="${LLAVON_LORA_COMMIT}"
    LLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LLAVON_IME_INSTALLED_LORA_TRAINER_PATH}"
    LLAVON_IME_GUI_INSTALLED_TABLES_DIR="${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables")
target_link_libraries(llavon-ime-lora-gui PRIVATE ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
target_include_directories(llavon-ime-lora-gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
install(TARGETS llavon-ime-lora-gui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${IME_CORE_SOURCE_DIR}/table/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service")
install(FILES "${IME_CORE_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service"
    RENAME ime-core-LICENSE.txt)

if(IME_UNIX_SERVICE_BUILD_TESTS)
    enable_testing()
    add_executable(ime_unix_service_tests
        tests/protocol_tests.cpp
        src/pipe/protocol.cpp
        src/engine/core_runtime.cpp
        src/engine/session_engine.cpp
        src/engine/stderr_logger.cpp
        src/session/session_manager.cpp
        src/training/commit_store.cpp
        src/training/numeric_dataset.cpp
    )
    target_compile_features(ime_unix_service_tests PRIVATE cxx_std_23)
    target_include_directories(ime_unix_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(ime_unix_service_tests PRIVATE
        IME_UNIX_SERVICE_TEST_TABLE_DIR="${IME_CORE_SOURCE_DIR}/table")
    target_link_libraries(ime_unix_service_tests PRIVATE ime-core::ime-core Threads::Threads ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
    add_test(NAME ime_unix_service_tests COMMAND ime_unix_service_tests)
endif()

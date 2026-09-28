# Host-agnostic llavon-ime engine library.
#
# This directory can be embedded from a host project (the fcitx5 addon today,
# the macOS frontend later) with add_subdirectory. Variables normally provided
# by the enclosing project when embedded:
#   LLAVON_IME_UNIX_SERVICE_NAME          (defaults to llavon-ime-unix-service)
#   LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH
#   LLAVON_IME_INSTALLED_MODEL_PATH
#   LLAVON_IME_TABLE_SOURCE_DIR
#   LLAVON_IME_DEBUG
#
# It can also be configured on its own (for the macOS frontend):
#   cmake -S engine -B build/engine -DCMAKE_TOOLCHAIN_FILE=vcpkg/.../vcpkg.cmake

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(llavon-ime-engine VERSION 0.2.1 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(LLAVON_IME_DEBUG "Compile in debug logging (LLAVON_DEBUG_LOG)" OFF)
    set(LLAVON_IME_INSTALLED_MODEL_PATH "" CACHE FILEPATH "Installed default GGUF model path")
    set(LLAVON_IME_TABLE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../ime-core/table"
        CACHE PATH
        "Source directory for the canonical ime-core tables")
    if(LLAVON_IME_ENGINE_BUILD_TESTS)
        enable_testing()
    endif()
endif()

include(GNUInstallDirs)

option(LLAVON_IME_ENGINE_BUILD_TESTS "Build llavon-ime engine tests" ON)
option(LLAVON_IME_REQUIRE_ATSPI "Fail configuration when the AT-SPI backend is unavailable" OFF)

set(LLAVON_IME_UNIX_SERVICE_NAME "llavon-ime-unix-service" CACHE STRING "Installed Unix service executable name")
if(NOT DEFINED LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH)
    set(LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH "${CMAKE_INSTALL_FULL_BINDIR}/${LLAVON_IME_UNIX_SERVICE_NAME}")
endif()
if(NOT DEFINED LLAVON_IME_INSTALLED_MEMSCAN_PATH)
    set(LLAVON_IME_INSTALLED_MEMSCAN_PATH
        "${CMAKE_INSTALL_FULL_LIBEXECDIR}/llavon-ime/llavon-ime-memscan"
        CACHE FILEPATH "Installed memory probe helper used by the context fallback")
endif()

add_library(llavon_ime_engine STATIC src/llavon_ime_engine.cpp)
set_target_properties(llavon_ime_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(llavon_ime_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(llavon_ime_engine PRIVATE
    LLAVON_IME_UNIX_SERVICE_NAME="${LLAVON_IME_UNIX_SERVICE_NAME}"
    LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH="${LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH}"
    LLAVON_IME_INSTALLED_MODEL_PATH="${LLAVON_IME_INSTALLED_MODEL_PATH}"
    LLAVON_IME_INSTALLED_MEMSCAN_PATH="${LLAVON_IME_INSTALLED_MEMSCAN_PATH}"
)
target_compile_options(llavon_ime_engine PRIVATE -Wall -Wextra -Wpedantic)
if(LLAVON_IME_DEBUG)
    target_compile_definitions(llavon_ime_engine PRIVATE LLAVON_IME_DEBUG=1)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
    # Compile-time headers for the AT-SPI backend, glib/gio for the worker
    # loop and bus activation, and a hard link only for the standalone probe.
    pkg_check_modules(ATSPI QUIET atspi-2)
    pkg_check_modules(ATSPI_LINK QUIET IMPORTED_TARGET atspi-2)
    pkg_check_modules(ATSPI_GLIB QUIET IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
endif()
if(LLAVON_IME_REQUIRE_ATSPI AND NOT (ATSPI_FOUND AND ATSPI_GLIB_FOUND))
    message(FATAL_ERROR
        "LLAVON_IME_REQUIRE_ATSPI is ON, but atspi-2 and glib/gobject/gio development files were not found")
endif()

target_sources(llavon_ime_engine PRIVATE src/config/config.cpp)
target_sources(llavon_ime_engine PRIVATE src/config/config_schema.cpp)
target_sources(llavon_ime_engine PRIVATE
    c_api/llavon_ime.cpp
    src/host/engine.cpp
    src/host/render_state.cpp
    src/text/utf.cpp
    src/bopomofo/keymap.cpp
    src/bopomofo/syllable.cpp
    src/bopomofo/table_engine.cpp
    src/buffer/composition_buffer.cpp
    src/engine/fallback_engine.cpp
    src/input/ascii_tokenizer.cpp
    src/input/keypad.cpp
    src/input/input_processor.cpp
    src/input/mixed_input_decoder.cpp
    src/input/punctuation.cpp
    src/protocol/protocol.cpp
    src/symbol/symbol_menu.cpp
    src/phrase_override/phrase_override_store.cpp
    src/context/accessibility_context.cpp
    src/context/memory_context.cpp
)
if(NOT WIN32)
    target_sources(llavon_ime_engine PRIVATE
        src/engine/service_transport.cpp
        src/ipc/unix_socket.cpp
    )
endif()
target_link_libraries(llavon_ime_engine PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(ATSPI_FOUND AND ATSPI_GLIB_FOUND AND NOT WIN32 AND NOT APPLE)
    target_sources(llavon_ime_engine PRIVATE src/atspi/atspi_context_provider.cpp)
    target_compile_definitions(llavon_ime_engine PRIVATE LLAVON_IME_HAVE_ATSPI=1)
    target_include_directories(llavon_ime_engine PRIVATE ${ATSPI_INCLUDE_DIRS})
    # libatspi is loaded at runtime with dlopen so the addon still loads on
    # systems without at-spi2-core; only glib/gio are hard dependencies.
    target_link_libraries(llavon_ime_engine PRIVATE PkgConfig::ATSPI_GLIB ${CMAKE_DL_LIBS})
endif()

# Standalone probe that reports the text before the caret of the currently
# focused editable widget through the same AT-SPI provider the engine uses. It
# validates on a real desktop that the a11y bus provides reliable, low-latency
# context for IME prediction.
if(ATSPI_FOUND AND ATSPI_LINK_FOUND AND NOT WIN32 AND NOT APPLE)
    add_executable(atspi_probe tools/atspi_probe.cpp)
    target_link_libraries(atspi_probe PRIVATE PkgConfig::ATSPI_LINK llavon_ime_engine)
    target_compile_options(atspi_probe PRIVATE -Wall -Wextra)
    install(TARGETS atspi_probe RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
elseif(PkgConfig_FOUND AND NOT WIN32)
    message(STATUS "atspi-2 not found; skipping atspi_probe tool")
endif()

if(LLAVON_IME_ENGINE_BUILD_TESTS)
    # The C ABI is exercised by a C translation unit, so hosts that embed the
    # engine with tests enabled need the C language ready even if they only
    # declare CXX.
    enable_language(C)
    add_subdirectory(tests)
endif()

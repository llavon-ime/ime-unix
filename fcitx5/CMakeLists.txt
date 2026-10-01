cmake_minimum_required(VERSION 3.20)
project(llavon-ime VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

option(LLAVON_IME_BUILD_TESTS "Build fcitx5 tests" ON)
option(LLAVON_IME_DEBUG "Compile in debug logging (LLAVON_DEBUG_LOG)" OFF)
set(LLAVON_IME_UNIX_SERVICE_NAME "llavon-ime-unix-service" CACHE STRING "Installed Unix service executable name")
set(LLAVON_IME_DISPLAY_VERSION "${PROJECT_VERSION}" CACHE STRING "Version displayed in the fcitx5 configuration UI")
set(LLAVON_IME_FCITX_PLUGIN_DIR "plugin" CACHE STRING "macOS fcitx5-macos plugin metadata install directory")
set(LLAVON_IME_INSTALLED_MODEL_PATH "" CACHE FILEPATH "Installed default GGUF model path")
set(LLAVON_IME_TABLE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../ime-core/table"
    CACHE PATH
    "Source directory for the canonical ime-core tables"
)

if(NOT EXISTS "${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
    message(FATAL_ERROR "ime-core tables are missing; initialize submodules recursively")
endif()

set(LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH "${CMAKE_INSTALL_FULL_BINDIR}/${LLAVON_IME_UNIX_SERVICE_NAME}")
set(LLAVON_IME_INSTALLED_TABLE_PATH "${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables/bopomofo_char.json")

if(LLAVON_IME_BUILD_TESTS)
    enable_testing()
endif()
set(LLAVON_IME_ENGINE_BUILD_TESTS "${LLAVON_IME_BUILD_TESTS}")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" "${CMAKE_BINARY_DIR}/engine")

function(ime_fcitx5_install_destination output directory)
    if(IS_ABSOLUTE "${directory}")
        file(RELATIVE_PATH relative_directory "${CMAKE_INSTALL_PREFIX}" "${directory}")
        if(NOT relative_directory MATCHES "^\\.\\.")
            set(${output} "${relative_directory}" PARENT_SCOPE)
            return()
        endif()
    endif()

    set(${output} "${directory}" PARENT_SCOPE)
endfunction()

install(FILES "${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
install(DIRECTORY "${LLAVON_IME_TABLE_SOURCE_DIR}/tokens"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables"
        FILES_MATCHING PATTERN "*.json")

find_package(Fcitx5 QUIET)
if(Fcitx5_FOUND AND NOT WIN32)
    find_package(Fcitx5Utils QUIET CONFIG)
    if(APPLE)
        if(NOT DEFINED FCITX_INSTALL_ADDONDIR)
            set(FCITX_INSTALL_ADDONDIR "${CMAKE_INSTALL_FULL_LIBDIR}/fcitx5")
        endif()
        if(NOT DEFINED FCITX_INSTALL_PKGDATADIR)
            set(FCITX_INSTALL_PKGDATADIR "${CMAKE_INSTALL_FULL_DATADIR}/fcitx5")
        endif()
        ime_fcitx5_install_destination(LLAVON_IME_FCITX_ADDONDIR "${FCITX_INSTALL_ADDONDIR}")
        ime_fcitx5_install_destination(LLAVON_IME_FCITX_PKGDATADIR "${FCITX_INSTALL_PKGDATADIR}")
    else()
        # Match upstream Fcitx5 addons such as fcitx5-chewing: let
        # GNUInstallDirs select lib, lib64, or a multiarch libdir.
        set(LLAVON_IME_FCITX_ADDONDIR "${CMAKE_INSTALL_LIBDIR}/fcitx5")
        set(LLAVON_IME_FCITX_PKGDATADIR "${CMAKE_INSTALL_DATADIR}/fcitx5")
    endif()

    add_library(llavon-ime-addon MODULE fcitx5/ime_engine.cpp)
    target_include_directories(llavon-ime-addon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(llavon-ime-addon PRIVATE Fcitx5::Core llavon_ime_engine)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Fcitx5ModuleDBus REQUIRED)
        target_link_libraries(llavon-ime-addon PRIVATE Fcitx5::Module::DBus)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
        target_link_libraries(llavon-ime-addon PRIVATE PkgConfig::XCB)
    endif()
    target_compile_definitions(llavon-ime-addon PRIVATE
        LLAVON_IME_SOURCE_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json"
        LLAVON_IME_INSTALLED_TABLE_PATH="${LLAVON_IME_INSTALLED_TABLE_PATH}"
        LLAVON_IME_DISPLAY_VERSION="${LLAVON_IME_DISPLAY_VERSION}"
        LLAVON_IME_INSTALLED_LORA_GUI_PATH="${CMAKE_INSTALL_FULL_BINDIR}/llavon-ime-lora-gui"
        LLAVON_IME_INSTALLED_SETTINGS_PATH="${CMAKE_INSTALL_FULL_BINDIR}/llavon-ime-settings"
        LLAVON_IME_INSTALLED_PHRASES_PATH="${CMAKE_INSTALL_FULL_BINDIR}/llavon-ime-phrases"
    )
    if(LLAVON_IME_DEBUG)
        target_compile_definitions(llavon-ime-addon PRIVATE LLAVON_IME_DEBUG=1)
    endif()
    if(APPLE)
        target_compile_definitions(llavon-ime-addon PRIVATE LLAVON_IME_NATIVE_SURROUNDING=1)
    endif()
    llavon_ime_enable_warnings(llavon-ime-addon)
    set_target_properties(llavon-ime-addon PROPERTIES PREFIX "")
    configure_file(fcitx5/addon.conf.in "${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf" @ONLY)
    configure_file(fcitx5/inputmethod.conf.in "${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf" @ONLY)
    install(TARGETS llavon-ime-addon LIBRARY DESTINATION "${LLAVON_IME_FCITX_ADDONDIR}")
    install(FILES "${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf"
            DESTINATION "${LLAVON_IME_FCITX_PKGDATADIR}/addon")
    install(FILES "${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf"
            DESTINATION "${LLAVON_IME_FCITX_PKGDATADIR}/inputmethod")
    if(APPLE)
        configure_file(fcitx5/plugin.json.in "${CMAKE_BINARY_DIR}/fcitx5/plugin/llavon-ime.json" @ONLY)
        install(FILES "${CMAKE_BINARY_DIR}/fcitx5/plugin/llavon-ime.json"
                DESTINATION "${LLAVON_IME_FCITX_PLUGIN_DIR}")
    endif()
elseif(Fcitx5_FOUND)
    message(STATUS "llavon-ime Fcitx5 frontend is only built on Linux/macOS")
else()
    message(STATUS "fcitx5 development files not found; skipping llavon-ime-addon")
endif()

if(LLAVON_IME_BUILD_TESTS)
    add_subdirectory(tests)
endif()

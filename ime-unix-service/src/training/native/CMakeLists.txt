cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../engine/cmake/Warnings.cmake")

# Also buildable on its own: UI iteration does not rebuild llama.cpp or Torch.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(llavon_lora_native VERSION 0.2.0 LANGUAGES CXX)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../common/protocol" "${CMAKE_CURRENT_BINARY_DIR}/protocol")
    include(GNUInstallDirs)
    option(IME_UNIX_SERVICE_BUILD_TESTS "Build native manager integration tests" ON)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(LLAVON_IME_SQLITE_TARGET SQLite3::SQLite3)
    else()
        set(LLAVON_IME_SQLITE_TARGET SQLite::SQLite3)
    endif()
    find_package(utf8cpp 4 CONFIG QUIET)
    if(NOT TARGET utf8cpp::utf8cpp)
        find_path(UTF8CPP_INCLUDE_DIR utf8/cpp20.h PATH_SUFFIXES utf8cpp REQUIRED)
        add_library(utf8cpp::utf8cpp INTERFACE IMPORTED)
        set_target_properties(utf8cpp::utf8cpp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${UTF8CPP_INCLUDE_DIR}")
    endif()
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(unofficial-sodium CONFIG QUIET)
    if(NOT TARGET unofficial-sodium::sodium)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
        add_library(unofficial-sodium::sodium ALIAS PkgConfig::SODIUM)
    endif()
    if(NOT LLAVON_LORA_COMMIT)
        find_package(Git REQUIRED)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
            ls-files --stage -- lora-trainer OUTPUT_VARIABLE gitlink OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        if(NOT gitlink MATCHES "^160000 ([0-9a-f]+) 0[\t]lora-trainer$")
            message(FATAL_ERROR "lora-trainer must be a pinned submodule")
        endif()
        set(LLAVON_LORA_COMMIT "${CMAKE_MATCH_1}")
    endif()
    string(LENGTH "${LLAVON_LORA_COMMIT}" commit_length)
    if(NOT commit_length EQUAL 40 OR NOT LLAVON_LORA_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "LLAVON_LORA_COMMIT must be a 40-digit pinned commit")
    endif()
    set(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/llavon-ime/tools/lora/llavon-lora")
    add_executable(llavon-ime-lora ../lora_cli.cpp ../libtorch_cache.cpp ../numeric_dataset.cpp ../commit_store.cpp)
    target_compile_features(llavon-ime-lora PRIVATE cxx_std_23)
    set_target_properties(llavon-ime-lora PROPERTIES CXX_EXTENSIONS OFF)
    llavon_ime_enable_warnings(llavon-ime-lora)
    target_compile_definitions(llavon-ime-lora PRIVATE
        LLAVON_IME_LORA_PINNED_COMMIT="${LLAVON_LORA_COMMIT}"
        LLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LLAVON_IME_INSTALLED_LORA_TRAINER_PATH}")
    target_include_directories(llavon-ime-lora PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_link_libraries(llavon-ime-lora PRIVATE llavon_ime_protocol ${LLAVON_IME_SQLITE_TARGET} utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
    install(TARGETS llavon-ime-lora RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../../ime-core/table/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
set(CMAKE_AUTOMOC ON)
set(training "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(service_src "${training}/..")

set(engine_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../../engine/src")
add_library(llavon_lora_widgets STATIC manager.hpp manager.cpp appearance.hpp appearance.cpp controls.hpp controls.cpp host_settings.hpp host_settings.cpp backend.cpp history_graph.hpp history_graph.cpp
    guide.hpp guide.cpp guide_illustration.hpp guide_illustration.cpp
    settings.hpp settings.cpp phrase_table.hpp phrase_table.cpp phrase_list.hpp phrase_list.cpp "${engine_src}/config/config.cpp" "${engine_src}/config/config_schema.cpp"
    "${engine_src}/phrase_override/phrase_override_store.cpp" "${engine_src}/text/utf.cpp")
target_compile_features(llavon_lora_widgets PUBLIC cxx_std_23)
qt_add_resources(llavon_lora_widgets native_appearance PREFIX "/native"
    FILES chevron-light.xpm chevron-dark.xpm up-light.xpm up-dark.xpm)
set_target_properties(llavon_lora_widgets PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(llavon_lora_widgets PUBLIC Qt6::Widgets Qt6::Network nlohmann_json::nlohmann_json PRIVATE utf8cpp::utf8cpp)
target_include_directories(llavon_lora_widgets PUBLIC "${engine_src}")
target_compile_definitions(llavon_lora_widgets PRIVATE
    LLAVON_SETTINGS_INSTALLED_TABLE="${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables/bopomofo_char.json"
    LLAVON_SETTINGS_SOURCE_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/../../../../ime-core/table/bopomofo_char.json")
if(NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_sources(llavon_lora_widgets PRIVATE linux_updates.hpp linux_updates.cpp)
    target_link_libraries(llavon_lora_widgets PRIVATE Qt6::DBus)
endif()
target_include_directories(llavon_lora_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(llavon-ime-lora-gui main.cpp)
set(app_icon "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png")
qt_add_resources(llavon-ime-lora-gui native_app_icon PREFIX "/native"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${app_icon}")
if(APPLE)
    find_program(SIPS_EXECUTABLE sips REQUIRED)
    find_program(ICONUTIL_EXECUTABLE iconutil REQUIRED)
    set(bundle_icon "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
    add_custom_command(OUTPUT "${bundle_icon}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${app_icon}" "-DOUTPUT=${bundle_icon}"
            "-DSIPS=${SIPS_EXECUTABLE}" "-DICONUTIL=${ICONUTIL_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/build-app-icon.cmake"
        DEPENDS "${app_icon}" "${CMAKE_CURRENT_SOURCE_DIR}/build-app-icon.cmake"
        COMMENT "Generating native app icon" VERBATIM)
    set_source_files_properties("${bundle_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(llavon-ime-lora-gui PRIVATE "${bundle_icon}")
    enable_language(OBJCXX)
    target_sources(llavon-ime-lora-gui PRIVATE activation_macos.mm host_macos.mm)
    set_target_properties(llavon-ime-lora-gui PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    target_link_libraries(llavon-ime-lora-gui PRIVATE "-framework AppKit")
endif()
target_link_libraries(llavon-ime-lora-gui PRIVATE llavon_lora_widgets Qt6::Network)
target_compile_definitions(llavon-ime-lora-gui PRIVATE
    LLAVON_BUILD_CLI="$<TARGET_FILE:llavon-ime-lora>"
    LLAVON_BUILD_BACKEND="$<TARGET_FILE:llavon-ime-lora-backend>")
add_library(llavon_job_process STATIC "${training}/job_process.cpp")
target_compile_features(llavon_job_process PUBLIC cxx_std_23)
set_target_properties(llavon_job_process PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(llavon_job_process PUBLIC unofficial-sodium::sodium)
llavon_ime_enable_warnings(llavon_job_process)
add_executable(llavon-ime-lora-backend "${training}/manager_backend.cpp" "${training}/commit_store.cpp"
    "${training}/lora_history_lca.cpp" "${training}/manager_database.cpp" "${training}/manager_model_policy.cpp")
target_compile_features(llavon-ime-lora-backend PRIVATE cxx_std_23)
set_target_properties(llavon-ime-lora-backend PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(llavon-ime-lora-backend PRIVATE LLAVON_NATIVE_GUI=1
    LLAVON_IME_LORA_PINNED_COMMIT="${LLAVON_LORA_COMMIT}"
    LLAVON_IME_INSTALLED_LORA_TRAINER_PATH="${LLAVON_IME_INSTALLED_LORA_TRAINER_PATH}"
    LLAVON_IME_GUI_INSTALLED_TABLES_DIR="${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables")
target_include_directories(llavon-ime-lora-backend PRIVATE "${service_src}")
target_link_libraries(llavon-ime-lora-backend PRIVATE llavon_job_process llavon_ime_protocol ${LLAVON_IME_SQLITE_TARGET}
    utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
add_dependencies(llavon-ime-lora-gui llavon-ime-lora-backend llavon-ime-lora)

function(lora_warnings target)
    llavon_ime_enable_warnings(${target})
endfunction()
foreach(target llavon_lora_widgets llavon-ime-lora-gui llavon-ime-lora-backend)
    lora_warnings(${target})
endforeach()

install(TARGETS llavon-ime-lora-backend RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS llavon-ime-settings llavon-ime-phrases DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(APPLE)
    # Keep the existing menu-launched bin name and add a Finder-launchable app.
    set_target_properties(llavon-ime-lora-gui PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.llavon-ime.lora-manager"
        MACOSX_BUNDLE_BUNDLE_NAME "拉風輸入法"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2.0")
    # Finder uses localized bundle names rather than the executable filename.
    foreach(language en zh-Hant)
        set(localized_name "${CMAKE_CURRENT_BINARY_DIR}/${language}.lproj/InfoPlist.strings")
        configure_file(InfoPlist.strings "${localized_name}" COPYONLY)
        set_source_files_properties("${localized_name}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${language}.lproj")
        target_sources(llavon-ime-lora-gui PRIVATE "${localized_name}")
    endforeach()
    install(TARGETS llavon-ime-lora-gui BUNDLE DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(CODE "
        set(launcher \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/llavon-ime-lora-gui\")
        file(REMOVE \"\${launcher}\")
    ")
    install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/llavon-ime-lora-gui" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    qt_generate_deploy_script(TARGET llavon-ime-lora-gui OUTPUT_SCRIPT deploy_script
        CONTENT "qt_deploy_runtime_dependencies(EXECUTABLE \"${CMAKE_INSTALL_BINDIR}/llavon-ime-lora-gui.app\")")
    install(SCRIPT "${deploy_script}")
    # macdeployqt rewrites third-party dylib install names. Re-sign the complete
    # bundle after those edits; release packaging replaces this ad-hoc identity.
    install(CODE "
        execute_process(COMMAND /usr/bin/codesign --force --deep --timestamp=none --sign -
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/llavon-ime-lora-gui.app\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
else()
    install(TARGETS llavon-ime-lora-gui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    configure_file(llavon-ime-lora.desktop.in llavon-ime-lora.desktop @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/llavon-ime-lora.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${app_icon}" DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps" RENAME llavon-ime.png)
endif()

if(IME_UNIX_SERVICE_BUILD_TESTS)
    enable_testing()
    add_executable(ime_job_process_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/job_process_tests.cpp")
    target_include_directories(ime_job_process_tests PRIVATE "${service_src}")
    target_link_libraries(ime_job_process_tests PRIVATE llavon_job_process)
    llavon_ime_enable_warnings(ime_job_process_tests)
    add_test(NAME ime_job_process_tests COMMAND ime_job_process_tests)
    set_tests_properties(ime_job_process_tests PROPERTIES TIMEOUT 15)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(llavon_linux_update_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/linux_update_tests.cpp")
        target_link_libraries(llavon_linux_update_tests PRIVATE llavon_lora_widgets Qt6::Test Qt6::DBus)
        lora_warnings(llavon_linux_update_tests)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_test(NAME llavon_linux_update_tests COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:llavon_linux_update_tests>)
        set_tests_properties(llavon_linux_update_tests PROPERTIES TIMEOUT 30)
    endif()
    add_executable(llavon_lora_native_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/lora_native_tests.cpp"
        "${training}/commit_store.cpp")
    target_include_directories(llavon_lora_native_tests PRIVATE "${service_src}")
    target_link_libraries(llavon_lora_native_tests PRIVATE llavon_ime_protocol llavon_lora_widgets Qt6::Test ${LLAVON_IME_SQLITE_TARGET}
        utf8cpp::utf8cpp nlohmann_json::nlohmann_json unofficial-sodium::sodium)
    if(TARGET llavon-ime-lora)
        set(test_cli "$<TARGET_FILE:llavon-ime-lora>")
    else()
        set(test_cli "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/macos/llavon-ime-lora" CACHE FILEPATH "Manager CLI used for native integration tests")
    endif()
    target_compile_definitions(llavon_lora_native_tests PRIVATE
        LLAVON_NATIVE_BACKEND="$<TARGET_FILE:llavon-ime-lora-backend>"
        LLAVON_TEST_GUI="$<TARGET_FILE:llavon-ime-lora-gui>"
        LLAVON_TEST_CLI="${test_cli}"
        LLAVON_TEST_TABLES="${CMAKE_CURRENT_SOURCE_DIR}/../../../../ime-core/table")
    lora_warnings(llavon_lora_native_tests)
    add_dependencies(llavon_lora_native_tests llavon-ime-lora-gui)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(LLAVON_DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_test(NAME llavon_lora_native_tests COMMAND "${LLAVON_DBUS_RUN_SESSION}"
            "--config-file=${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/session-bus.conf"
            -- "$<TARGET_FILE:llavon_lora_native_tests>")
    else()
        add_test(NAME llavon_lora_native_tests COMMAND llavon_lora_native_tests)
    endif()
    set_tests_properties(llavon_lora_native_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
endif()

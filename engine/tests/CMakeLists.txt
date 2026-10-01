# Host-free engine tests. These exercise the engine library directly and must
# not depend on fcitx5 or any other host toolkit.
if(NOT LLAVON_IME_TABLE_SOURCE_DIR)
    set(LLAVON_IME_TABLE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../ime-core/table")
endif()

set(LLAVON_IME_ENGINE_TEST_SOURCES
    fake_host.cpp
    test_main.cpp
    config_tests.cpp
    bopomofo_tests.cpp
    buffer_tests.cpp
    protocol_tests.cpp
    service_transport_tests.cpp
    keypad_tests.cpp
    input_state_tests.cpp
    input_key_tests.cpp
    punctuation_tests.cpp
    candidate_view_tests.cpp
    input_processor_tests.cpp
    prediction_state_tests.cpp
    symbol_menu_tests.cpp
    ascii_tokenizer_tests.cpp
    caret_prefix_sampler_tests.cpp
    sample_adoption_tests.cpp
    accessibility_context_tests.cpp
    memory_context_tests.cpp
    real_service_tests.cpp
    fallback_engine_tests.cpp
    mixed_input_decoder_tests.cpp
    phrase_override_store_tests.cpp
    host_engine_tests.cpp
    host_prediction_tests.cpp
    c_api_tests.c
)

enable_testing()

if(NOT WIN32)
    add_executable(llavon_ime_resource_tests resource_tests.cpp)
    target_link_libraries(llavon_ime_resource_tests PRIVATE llavon_ime_engine)
    llavon_ime_enable_warnings(llavon_ime_resource_tests)
    add_test(NAME llavon_ime_resource_tests COMMAND llavon_ime_resource_tests)
endif()

add_executable(mixed_input_probe ../tools/mixed_input_probe.cpp)
target_link_libraries(mixed_input_probe PRIVATE llavon_ime_engine)
target_compile_definitions(mixed_input_probe PRIVATE LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
llavon_ime_enable_warnings(mixed_input_probe)

add_executable(llavon_ime_tests ${LLAVON_IME_ENGINE_TEST_SOURCES})
llavon_ime_enable_warnings(llavon_ime_tests)
target_link_libraries(llavon_ime_tests PRIVATE llavon_ime_engine)
target_compile_definitions(llavon_ime_tests PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
add_test(NAME llavon_ime_tests COMMAND llavon_ime_tests)
set_tests_properties(llavon_ime_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# The standard raw-key suite: every frontend behaviour test lives here, drives
# keys through the harness and can start a real prediction service. It is
# host-free, so Linux and macOS run the same suites.
set(LLAVON_IME_RAWKEY_TEST_SOURCES
    fake_host.cpp
    rawkey/rawkey_test_main.cpp
    rawkey/raw_key_harness.cpp
    rawkey/basic_input_tests.cpp
    rawkey/candidate_navigation_tests.cpp
    rawkey/context_source_tests.cpp
    rawkey/native_context_tests.cpp
    rawkey/hsu_layout_tests.cpp
    rawkey/keyboard_layout_tests.cpp
    rawkey/smart_editing_tests.cpp
    rawkey/lifecycle_tests.cpp
    rawkey/memory_format_tests.cpp
    rawkey/phrase_override_tests.cpp
    rawkey/prediction_integration_tests.cpp
    rawkey/live_mixed_model_tests.cpp
    rawkey/punctuation_tests.cpp
    rawkey/shift_letter_tests.cpp
    rawkey/smart_candidate_tests.cpp
    rawkey/smart_chat_tests.cpp
    rawkey/smart_config_tests.cpp
    rawkey/smart_decoder_tests.cpp
    rawkey/smart_edge_tests.cpp
    rawkey/smart_hsu_tests.cpp
    rawkey/smart_long_tests.cpp
    rawkey/smart_mixed_tests.cpp
    rawkey/smart_regression_tests.cpp
    rawkey/smart_space_tests.cpp
    rawkey/smart_standard_tests.cpp
    rawkey/smart_usability_tests.cpp
    rawkey/mixed_benchmark_tests.cpp
    rawkey/structured_boundary_tests.cpp
    rawkey/smart_work_tests.cpp
    rawkey/symbol_menu_tests.cpp
    rawkey/training_commit_tests.cpp
)

add_executable(llavon_ime_rawkey_tests ${LLAVON_IME_RAWKEY_TEST_SOURCES})
llavon_ime_enable_warnings(llavon_ime_rawkey_tests)
target_link_libraries(llavon_ime_rawkey_tests PRIVATE llavon_ime_engine)
target_include_directories(llavon_ime_rawkey_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(llavon_ime_rawkey_tests PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
add_test(NAME llavon_ime_rawkey_tests COMMAND llavon_ime_rawkey_tests)
set_tests_properties(llavon_ime_rawkey_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../..")

#include <cstdlib>

int run_config_tests();
int run_bopomofo_tests();
int run_buffer_tests();
int run_protocol_tests();
int run_service_transport_tests();
int run_keypad_tests();
int run_input_state_tests();
int run_input_key_tests();
int run_punctuation_tests();
int run_candidate_view_tests();
int run_input_processor_tests();
int run_input_processor_process_tests();
int run_prediction_state_tests();
int run_symbol_menu_tests();
int run_ascii_tokenizer_tests();
int run_caret_prefix_sampler_tests();
int run_sample_adoption_tests();
int run_accessibility_context_tests();
int run_memory_context_tests();
int run_real_service_tests();
int run_fallback_engine_tests();
int run_mixed_input_decoder_tests();
int run_phrase_override_store_tests();
int run_host_engine_tests();
int run_host_prediction_tests();
extern "C" int run_c_api_tests();

int main() {
    const char* real_service_only = std::getenv("LLAVON_IME_REAL_SERVICE_ONLY");
    if (real_service_only != nullptr && real_service_only[0] != '\0') return run_real_service_tests();
    const char* transport_only = std::getenv("LLAVON_IME_TRANSPORT_ONLY");
    if (transport_only != nullptr && transport_only[0] != '\0') return run_service_transport_tests();

    if (run_config_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_bopomofo_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_buffer_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_protocol_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_service_transport_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_keypad_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_input_state_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_input_key_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_punctuation_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_candidate_view_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_input_processor_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_input_processor_process_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_prediction_state_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_symbol_menu_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_ascii_tokenizer_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_caret_prefix_sampler_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_sample_adoption_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_accessibility_context_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_memory_context_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_real_service_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_fallback_engine_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_mixed_input_decoder_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_phrase_override_store_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_host_engine_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_host_prediction_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_c_api_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

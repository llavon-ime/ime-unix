#include <cstdlib>
#include <cstdio>

#include "test_suites.h"

namespace {

int run_suite(const char* name, int (*run)()) {
    const int result = run();
    if (result != EXIT_SUCCESS) std::fprintf(stderr, "failed suite: %s\n", name);
    return result;
}

}  // namespace

int main() {
    const char* real_service_only = std::getenv("LLAVON_IME_REAL_SERVICE_ONLY");
    if (real_service_only != nullptr && real_service_only[0] != '\0') return run_real_service_tests();
    const char* transport_only = std::getenv("LLAVON_IME_TRANSPORT_ONLY");
    if (transport_only != nullptr && transport_only[0] != '\0') return run_service_transport_tests();

    if (run_suite("config", run_config_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("bopomofo", run_bopomofo_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("buffer", run_buffer_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("protocol", run_protocol_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("service-transport", run_service_transport_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("keypad", run_keypad_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("input-state", run_input_state_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("input-key", run_input_key_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("punctuation", run_punctuation_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("candidate-view", run_candidate_view_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("input-processor", run_input_processor_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("input-processor-process", run_input_processor_process_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("prediction-state", run_prediction_state_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("symbol-menu", run_symbol_menu_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("ascii-tokenizer", run_ascii_tokenizer_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("caret-prefix-sampler", run_caret_prefix_sampler_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("sample-adoption", run_sample_adoption_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("accessibility-context", run_accessibility_context_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("memory-context", run_memory_context_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("real-service", run_real_service_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("fallback-engine", run_fallback_engine_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("mixed-input-decoder", run_mixed_input_decoder_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("phrase-override-store", run_phrase_override_store_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("host-engine", run_host_engine_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("host-prediction", run_host_prediction_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_suite("c-api", run_c_api_tests) != EXIT_SUCCESS) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

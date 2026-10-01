#pragma once

#ifdef __cplusplus
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
extern "C" {
#endif
int run_c_api_tests(void);
#ifdef __cplusplus
}
#endif

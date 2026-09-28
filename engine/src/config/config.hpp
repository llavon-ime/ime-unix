#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace llavon::ime {

struct Config {
    std::string model_path;
    int context_length = 512;
    int thread_count = 1;
    int gpu_layers = 0;
    int idle_timeout_seconds = 1800;
    std::string keyboard_layout = "standard";
    std::string selection_keys = "1234567890";
    int selection_key_count = 10;
    int candidate_page_size = 10;
    std::string candidate_layout = "not_set";
    bool space_selects_candidate = true;
    std::string select_phrase = "before_cursor";
    bool move_cursor_after_selection = false;
    bool esc_clears_entire_buffer = false;
    // CapsLock on still inputs bopomofo (MS IME style).
    bool caps_lock_inputs_bopomofo = true;
    std::string shift_letter_keys = "directly_output_uppercase";
    // Smart Chinese-English: lowercase letters are held raw as a pending word
    // until a tone key or space decides whether they were 注音 or English.
    bool smart_english = false;
    // Last-resort context source: probe the focused application's memory for a
    // token the engine inserts at the caret. Needs an external helper with
    // CAP_SYS_PTRACE (or kernel.yama.ptrace_scope=0).
    bool memory_context = false;
};

Config default_config();
Config load_config();
nlohmann::json to_json(const Config& cfg);
Config config_from_json(const nlohmann::json& json);
std::filesystem::path config_path();
std::filesystem::path legacy_config_path();
std::filesystem::path phrase_overrides_path();
std::filesystem::path runtime_dir();
std::filesystem::path socket_path();
std::filesystem::path pid_path();

}  // namespace llavon::ime

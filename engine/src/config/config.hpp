#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace llavon::ime {

struct Config {
    std::string model_path;
    std::string inference_provider = "auto";
    int context_length = 512;
    int thread_count = 1;
    int gpu_layers = 0;
    int idle_timeout_seconds = 1800;
    std::string keyboard_layout = "standard";
    std::string selection_keys = "1234567890";
    int selection_key_count = 10;
    int candidate_page_size = 10;
    std::string candidate_layout = "not_set";
    // Tab in the candidate list: expand the whole page (default) or move to
    // the next candidate.
    std::string candidate_tab_action = "expand";
    bool space_selects_candidate = true;
    std::string select_phrase = "before_cursor";
    bool move_cursor_after_selection = false;
    bool esc_clears_entire_buffer = false;
    // macOS Zhuyin style: Escape commits the composition as shown. Takes
    // precedence over esc_clears_entire_buffer; an open list closes first.
    bool esc_commits_composition = false;
    // macOS Zhuyin style: a tone key typed right after a completed character
    // rewrites that character's tone instead of starting a new reading.
    bool tone_key_overwrites_previous = false;
    // macOS Zhuyin style: a completed reading without characters stays in the
    // composition (tone included) for editing instead of being discarded.
    bool keep_invalid_reading = false;
    // macOS Zhuyin style: Shift+BackSpace removes only the tone of the
    // character before the caret, turning it back into an editable reading.
    bool shift_backspace_removes_tone = false;
    // Which full-width marks the punctuation keys type: libchewing's table
    // ("chewing") or macOS Zhuyin's ("apple": \ is 、, ' and " alternate
    // between opening and closing quotes, _ is ＿ and ^ is ＾).
    std::string punctuation_style = "chewing";
    // CapsLock on still inputs bopomofo (MS IME style).
    bool caps_lock_inputs_bopomofo = true;
    std::string shift_letter_keys = "directly_output_uppercase";
    // Smart Chinese-English keeps reversible raw keys while the decoder ranks
    // English and 注音 interpretations for the current preview.
    bool smart_english = false;
    // Internal test/probe control, not a persisted user setting. Production
    // smart English automatically refines complete readings without waiting
    // for punctuation; old JSON/INI preview toggles are ignored by the schema.
    bool smart_model_preview = true;
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

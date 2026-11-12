#include "config/config_schema.hpp"
#include "util/parse_number.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

#include "config/config.hpp"

namespace llavon::ime {

namespace {

ConfigField boolean_field(std::string key, std::string ini_key, std::string label, std::string group,
                          bool Config::*member) {
    ConfigField field;
    field.key = std::move(key);
    field.ini_key = std::move(ini_key);
    field.label = std::move(label);
    field.group = std::move(group);
    field.kind = ConfigValueKind::Boolean;
    field.member = member;
    return field;
}

ConfigField integer_field(std::string key, std::string ini_key, std::string label, std::string group,
                          int Config::*member, int minimum, int maximum) {
    ConfigField field;
    field.key = std::move(key);
    field.ini_key = std::move(ini_key);
    field.label = std::move(label);
    field.group = std::move(group);
    field.kind = ConfigValueKind::Integer;
    field.minimum = minimum;
    field.maximum = maximum;
    field.member = member;
    return field;
}

ConfigField text_field(std::string key, std::string ini_key, std::string label, std::string group,
                       std::string Config::*member) {
    ConfigField field;
    field.key = std::move(key);
    field.ini_key = std::move(ini_key);
    field.label = std::move(label);
    field.group = std::move(group);
    field.kind = ConfigValueKind::Text;
    field.member = member;
    return field;
}

ConfigField choice_field(std::string key, std::string ini_key, std::string label, std::string group,
                         std::string Config::*member, std::vector<ConfigChoice> choices) {
    ConfigField field;
    field.key = std::move(key);
    field.ini_key = std::move(ini_key);
    field.label = std::move(label);
    field.group = std::move(group);
    field.kind = ConfigValueKind::Choice;
    field.choices = std::move(choices);
    field.member = member;
    return field;
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

}  // namespace

const std::vector<ConfigField>& config_fields() {
    static const std::vector<ConfigField> fields = {
        // Model and runtime tuning.
        text_field("model_path", "ModelPath", "模型路徑", "模型與執行", &Config::model_path),
        choice_field("inference_provider", "InferenceProvider", "推論後端", "模型與執行",
                     &Config::inference_provider,
                     {{"auto", "自動", {"自動（MLX 失敗時改用 llama.cpp）"}},
                      {"mlx", "MLX", {}},
                      {"llama_cpp", "llama.cpp", {"llama.cpp"}}}),
        integer_field("context_length", "ContextLength", "上下文長度", "模型與執行", &Config::context_length, 1,
                      1048576),
        integer_field("thread_count", "ThreadCount", "執行緒數", "模型與執行", &Config::thread_count, 1, 1024),
        integer_field("gpu_layers", "GpuLayers", "顯示卡分層數", "模型與執行", &Config::gpu_layers, 0, 1024),
        integer_field("idle_timeout_seconds", "IdleTimeoutSeconds", "閒置逾時秒數", "模型與執行",
                      &Config::idle_timeout_seconds, 0, 86400),
        // Keyboard and candidates.
        choice_field("keyboard_layout", "BopomofoKeyboardLayout", "注音鍵盤配置", "鍵盤與候選字",
                     &Config::keyboard_layout,
                      {{"standard", "標準", {}}, {"hsu", "許氏", {"Hsu", "許氏鍵盤"}},
                       {"ibm", "IBM", {"IBM鍵盤"}}, {"et", "倚天", {"ET", "ET41", "倚天41鍵"}},
                       {"ginyieh", "精業", {"GinYieh", "精業鍵盤"}},
                       {"et26", "倚天26鍵", {"ET26", "倚天26"}},
                       {"dachen_cp26", "大千26鍵", {"DachenCP26", "大千26", "dc26"}}}),
        choice_field("selection_keys", "SelectionKeys", "候選選字鍵", "鍵盤與候選字", &Config::selection_keys,
                     {{"1234567890", "數字鍵", {"123456789"}},
                      {"asdfghjkl", "本位列", {}},
                      {"asdfzxcvb", "左手鍵", {}}}),
        integer_field("selection_key_count", "SelectionKeysCount", "候選選字鍵數量", "鍵盤與候選字",
                      &Config::selection_key_count, 4, 10),
        integer_field("candidate_page_size", "CandidatePageSize", "候選頁大小", "鍵盤與候選字",
                      &Config::candidate_page_size, 1, 50),
        choice_field("candidate_layout", "CandidateLayout", "候選窗排列", "鍵盤與候選字",
                     &Config::candidate_layout,
                     {{"not_set", "系統預設", {}}, {"vertical", "垂直", {}}, {"horizontal", "水平", {}}}),
        choice_field("select_phrase", "SelectPhrase", "候選字查詢位置", "鍵盤與候選字", &Config::select_phrase,
                     {{"before_cursor", "游標前", {}}, {"after_cursor", "游標後", {}}}),
        choice_field("shift_letter_keys", "ShiftLetterKeys", "Shift 鍵輸入英文", "鍵盤與候選字",
                     &Config::shift_letter_keys,
                     {{"directly_output_uppercase", "直接輸出大寫", {}},
                      {"directly_put_to_buffer", "直接放入組字區",
                       {"put_lowercase_to_buffer", "小寫放入組字區"}}}),
        // Input behavior.
        boolean_field("space_selects_candidate", "ChooseCandidateUsingSpace", "空白鍵選取候選字", "輸入行為",
                      &Config::space_selects_candidate),
        choice_field("candidate_tab_action", "CandidateTabAction", "候選字窗 Tab 鍵", "輸入行為",
                     &Config::candidate_tab_action,
                     {{"expand", "展開候選頁", {}},
                      {"next_candidate", "切換下一個候選", {"next", "cycle"}}}),
        boolean_field("move_cursor_after_selection", "MoveCursorAfterSelection", "選字後移動游標", "輸入行為",
                      &Config::move_cursor_after_selection),
        boolean_field("caps_lock_inputs_bopomofo", "CapsLockInputsBopomofo", "大寫鎖定時仍輸入注音", "輸入行為",
                      &Config::caps_lock_inputs_bopomofo),
        boolean_field("esc_clears_entire_buffer", "EscKeyClearsEntireComposingBuffer", "Esc 鍵清除整個組字區",
                      "輸入行為", &Config::esc_clears_entire_buffer),
        boolean_field("esc_commits_composition", "EscKeyCommitsComposingBuffer", "Esc 鍵送出組字區", "輸入行為",
                      &Config::esc_commits_composition),
        boolean_field("tone_key_overwrites_previous", "ToneKeyOverwritesPrevious", "聲調鍵修改前一字聲調",
                      "輸入行為", &Config::tone_key_overwrites_previous),
        boolean_field("keep_invalid_reading", "KeepInvalidReading", "注音組不出字時保留注音", "輸入行為",
                      &Config::keep_invalid_reading),
        boolean_field("shift_backspace_removes_tone", "ShiftBackSpaceRemovesTone", "Shift+退格鍵移除前一字的聲調",
                      "輸入行為", &Config::shift_backspace_removes_tone),
        choice_field("punctuation_style", "PunctuationStyle", "標點符號樣式", "輸入行為",
                     &Config::punctuation_style,
                     {{"chewing", "新酷音", {}}, {"apple", "蘋果注音", {"macos"}}}),
        boolean_field("smart_english", "SmartEnglish", "智慧型中英文", "輸入行為", &Config::smart_english),
        // The memory probe helper is Linux-only (process_vm_readv), so the
        // option is only offered where it can actually work.
#if defined(__linux__)
        boolean_field("memory_context", "MemoryContext", "記憶體上下文取樣", "模型與執行",
                      &Config::memory_context),
#endif
    };
    return fields;
}

ConfigValue config_field_value(const Config& config, const ConfigField& field) {
    return std::visit([&](auto member) -> ConfigValue { return config.*member; }, field.member);
}

bool set_config_field_value(Config& config, const ConfigField& field, const ConfigValue& value) {
    switch (field.kind) {
        case ConfigValueKind::Boolean:
            if (const auto* member = std::get_if<bool Config::*>(&field.member); member && std::holds_alternative<bool>(value)) {
                config.*(*member) = std::get<bool>(value);
                return true;
            }
            return false;
        case ConfigValueKind::Integer:
            if (!std::holds_alternative<int>(value)) return false;
            if (std::get<int>(value) < field.minimum || std::get<int>(value) > field.maximum) return false;
            if (const auto* member = std::get_if<int Config::*>(&field.member)) {
                config.*(*member) = std::get<int>(value);
                return true;
            }
            return false;
        case ConfigValueKind::Text:
            if (const auto* member = std::get_if<std::string Config::*>(&field.member); member && std::holds_alternative<std::string>(value)) {
                config.*(*member) = std::get<std::string>(value);
                return true;
            }
            return false;
        case ConfigValueKind::Choice: {
            if (!std::holds_alternative<std::string>(value)) return false;
            const auto canonical = canonical_choice(field, std::get<std::string>(value));
            const auto* member = std::get_if<std::string Config::*>(&field.member);
            if (!canonical || member == nullptr) return false;
            config.*(*member) = *canonical;
            return true;
        }
    }
    return false;
}

ConfigValue default_config_field_value(const ConfigField& field) {
    return config_field_value(default_config(), field);
}

std::optional<std::string> canonical_choice(const ConfigField& field, std::string_view value) {
    for (const auto& choice : field.choices) {
        if (value == choice.value || value == choice.label) return choice.value;
        for (const auto& alias : choice.aliases) {
            if (value == alias) return choice.value;
        }
    }
    // English values are also accepted case-insensitively (files written by
    // older versions used "Horizontal", "Hsu" and friends).
    const auto lowered = lowercase(value);
    for (const auto& choice : field.choices) {
        if (lowered == lowercase(choice.value)) return choice.value;
        if (lowered == lowercase(choice.label)) return choice.value;
        for (const auto& alias : choice.aliases) {
            if (lowered == lowercase(alias)) return choice.value;
        }
    }
    return std::nullopt;
}

std::string choice_label(const ConfigField& field, std::string_view value) {
    for (const auto& choice : field.choices) {
        if (value == choice.value) return choice.label;
    }
    return std::string(value);
}

std::optional<ConfigValue> config_value_from_ini(const ConfigField& field, const std::string& value) {
    switch (field.kind) {
        case ConfigValueKind::Boolean:
            if (value == "True" || value == "true" || value == "1") return ConfigValue{true};
            if (value == "False" || value == "false" || value == "0") return ConfigValue{false};
            return std::nullopt;
        case ConfigValueKind::Integer: {
            const auto integer = parse_decimal<int>(value);
            if (!integer || *integer < field.minimum || *integer > field.maximum) return std::nullopt;
            return ConfigValue{*integer};
        }
        case ConfigValueKind::Text:
            return ConfigValue{value};
        case ConfigValueKind::Choice: {
            const auto canonical = canonical_choice(field, value);
            if (!canonical) return std::nullopt;
            return ConfigValue{*canonical};
        }
    }
    return std::nullopt;
}

namespace {

const ConfigField* find_config_field(std::string_view key) {
    const auto& fields = config_fields();
    const auto it = std::ranges::find(fields, key, &ConfigField::key);
    return it == fields.end() ? nullptr : &*it;
}

nlohmann::json config_value_json(const ConfigValue& value) {
    return std::visit([](const auto& item) { return nlohmann::json(item); }, value);
}

}  // namespace

const std::vector<ConfigPreset>& config_presets() {
    using namespace std::string_literals;
    // Values follow each input method's documented or shipped defaults:
    // Microsoft's support article for Microsoft Bopomofo, McBopomofo's
    // Preferences.swift, and macOS Zhuyin as documented by Apple, McBopomofo
    // (issue 753) and vChewing, plus behavior verified on a Mac.
    static const std::vector<ConfigPreset> presets = {
        {"llavon", "拉風預設", "空白鍵選字、每頁 10 個候選字、大寫鎖定時仍輸入注音。", {}},
        {"microsoft", "微軟注音", "接近 Windows 微軟注音：直式候選窗、選游標後的字、大寫鎖定時仍輸入注音。",
         {
             {"selection_key_count", 9},
             {"candidate_page_size", 9},
             {"candidate_layout", "vertical"s},
             {"space_selects_candidate", false},
             {"select_phrase", "after_cursor"s},
             {"esc_commits_composition", true},
             {"caps_lock_inputs_bopomofo", true},
             {"shift_letter_keys", "directly_output_uppercase"s},
         }},
        {"apple", "蘋果注音", "接近 macOS 內建注音：直式候選窗、聲調鍵修改前一字、保留組不出字的注音、Esc 送出、蘋果的標點符號。",
         {
             {"selection_key_count", 9},
             {"candidate_page_size", 9},
             {"candidate_layout", "vertical"s},
             {"space_selects_candidate", false},
             {"select_phrase", "before_cursor"s},
             {"esc_commits_composition", true},
             {"tone_key_overwrites_previous", true},
             {"keep_invalid_reading", true},
             {"shift_backspace_removes_tone", true},
             {"punctuation_style", "apple"s},
             {"caps_lock_inputs_bopomofo", true},
             {"shift_letter_keys", "directly_output_uppercase"s},
         }},
        {"mcbopomofo", "小麥注音", "接近小麥注音的預設：直式候選窗、空白鍵選字、大寫鎖定時輸入英文。",
         {
             {"selection_key_count", 9},
             {"candidate_page_size", 9},
             {"candidate_layout", "vertical"s},
             {"space_selects_candidate", true},
             {"select_phrase", "before_cursor"s},
             {"esc_clears_entire_buffer", false},
             {"caps_lock_inputs_bopomofo", false},
             {"shift_letter_keys", "directly_output_uppercase"s},
         }},
    };
    return presets;
}

const std::vector<std::string>& config_preset_field_keys() {
    static const std::vector<std::string> keys = {
        "selection_keys",
        "selection_key_count",
        "candidate_page_size",
        "candidate_layout",
        "select_phrase",
        "shift_letter_keys",
        "space_selects_candidate",
        "move_cursor_after_selection",
        "caps_lock_inputs_bopomofo",
        "esc_clears_entire_buffer",
        "esc_commits_composition",
        "tone_key_overwrites_previous",
        "keep_invalid_reading",
        "shift_backspace_removes_tone",
        "punctuation_style",
    };
    return keys;
}

std::optional<std::vector<std::pair<std::string, ConfigValue>>> config_preset_values(std::string_view key) {
    const auto& presets = config_presets();
    const auto preset = std::ranges::find(presets, key, &ConfigPreset::key);
    if (preset == presets.end()) return std::nullopt;

    Config config = default_config();
    for (const auto& [field_key, value] : preset->values) {
        const auto* field = find_config_field(field_key);
        if (field == nullptr || std::ranges::find(config_preset_field_keys(), field_key) ==
                                    config_preset_field_keys().end() ||
            !set_config_field_value(config, *field, value)) {
            return std::nullopt;
        }
    }

    std::vector<std::pair<std::string, ConfigValue>> values;
    for (const auto& field_key : config_preset_field_keys()) {
        const auto* field = find_config_field(field_key);
        if (field == nullptr) return std::nullopt;
        values.emplace_back(field_key, config_field_value(config, *field));
    }
    return values;
}

bool apply_config_preset(Config& config, std::string_view key) {
    const auto values = config_preset_values(key);
    if (!values) return false;
    Config updated = config;
    for (const auto& [field_key, value] : *values) {
        const auto* field = find_config_field(field_key);
        if (field == nullptr || !set_config_field_value(updated, *field, value)) return false;
    }
    config = std::move(updated);
    return true;
}

nlohmann::json config_schema_json() {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : config_fields()) {
        const auto default_value = default_config_field_value(field);
        nlohmann::json entry{
            {"key", field.key},
            {"ini_key", field.ini_key},
            {"label", field.label},
            {"group", field.group},
        };
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                entry["kind"] = "boolean";
                entry["default"] = std::get<bool>(default_value);
                break;
            case ConfigValueKind::Integer:
                entry["kind"] = "integer";
                entry["minimum"] = field.minimum;
                entry["maximum"] = field.maximum;
                entry["default"] = std::get<int>(default_value);
                break;
            case ConfigValueKind::Text:
                entry["kind"] = "text";
                entry["default"] = std::get<std::string>(default_value);
                break;
            case ConfigValueKind::Choice: {
                entry["kind"] = "choice";
                entry["default"] = std::get<std::string>(default_value);
                nlohmann::json choices = nlohmann::json::array();
                for (const auto& choice : field.choices) {
                    choices.push_back({{"value", choice.value}, {"label", choice.label}});
                }
                entry["choices"] = std::move(choices);
                break;
            }
        }
        fields.push_back(std::move(entry));
    }

    nlohmann::json presets = nlohmann::json::array();
    for (const auto& preset : config_presets()) {
        const auto values = config_preset_values(preset.key);
        if (!values) continue;
        nlohmann::json object = nlohmann::json::object();
        for (const auto& [key, value] : *values) object[key] = config_value_json(value);
        presets.push_back({{"key", preset.key},
                           {"label", preset.label},
                           {"description", preset.description},
                           {"values", std::move(object)}});
    }
    return nlohmann::json{{"fields", std::move(fields)}, {"presets", std::move(presets)}};
}

}  // namespace llavon::ime

#include "config/config_schema.hpp"

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
        integer_field("context_length", "ContextLength", "上下文長度", "模型與執行", &Config::context_length, 1,
                      1048576),
        integer_field("thread_count", "ThreadCount", "執行緒數", "模型與執行", &Config::thread_count, 1, 1024),
        integer_field("gpu_layers", "GpuLayers", "顯示卡分層數", "模型與執行", &Config::gpu_layers, 0, 1024),
        integer_field("idle_timeout_seconds", "IdleTimeoutSeconds", "閒置逾時秒數", "模型與執行",
                      &Config::idle_timeout_seconds, 0, 86400),
        // Keyboard and candidates.
        choice_field("keyboard_layout", "BopomofoKeyboardLayout", "注音鍵盤配置", "鍵盤與候選字",
                     &Config::keyboard_layout,
                     {{"standard", "標準", {}}, {"hsu", "許氏", {"Hsu", "許氏鍵盤"}}}),
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
        boolean_field("move_cursor_after_selection", "MoveCursorAfterSelection", "選字後移動游標", "輸入行為",
                      &Config::move_cursor_after_selection),
        boolean_field("caps_lock_inputs_bopomofo", "CapsLockInputsBopomofo", "大寫鎖定時仍輸入注音", "輸入行為",
                      &Config::caps_lock_inputs_bopomofo),
        boolean_field("esc_clears_entire_buffer", "EscKeyClearsEntireComposingBuffer", "Esc 鍵清除整個組字區",
                      "輸入行為", &Config::esc_clears_entire_buffer),
        boolean_field("smart_english", "SmartEnglish", "智慧型中英文", "輸入行為", &Config::smart_english),
        boolean_field("memory_context", "MemoryContext", "記憶體上下文取樣", "模型與執行",
                      &Config::memory_context),
    };
    return fields;
}

ConfigValue config_field_value(const Config& config, const ConfigField& field) {
    ConfigValue value;
    std::visit(
        [&](auto member) {
            using Member = decltype(member);
            if constexpr (std::is_same_v<Member, bool Config::*>) {
                value.boolean = config.*member;
            } else if constexpr (std::is_same_v<Member, int Config::*>) {
                value.integer = config.*member;
            } else {
                value.text = config.*member;
            }
        },
        field.member);
    return value;
}

bool set_config_field_value(Config& config, const ConfigField& field, const ConfigValue& value) {
    switch (field.kind) {
        case ConfigValueKind::Boolean:
            if (const auto* member = std::get_if<bool Config::*>(&field.member)) {
                config.*(*member) = value.boolean;
                return true;
            }
            return false;
        case ConfigValueKind::Integer:
            if (value.integer < field.minimum || value.integer > field.maximum) return false;
            if (const auto* member = std::get_if<int Config::*>(&field.member)) {
                config.*(*member) = value.integer;
                return true;
            }
            return false;
        case ConfigValueKind::Text:
            if (const auto* member = std::get_if<std::string Config::*>(&field.member)) {
                config.*(*member) = value.text;
                return true;
            }
            return false;
        case ConfigValueKind::Choice: {
            const auto canonical = canonical_choice(field, value.text);
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
            if (value == "True" || value == "true" || value == "1") return ConfigValue{.boolean = true};
            if (value == "False" || value == "false" || value == "0") return ConfigValue{.boolean = false};
            return std::nullopt;
        case ConfigValueKind::Integer: {
            try {
                size_t parsed = 0;
                const int integer = std::stoi(value, &parsed);
                if (parsed != value.size() || integer < field.minimum || integer > field.maximum) {
                    return std::nullopt;
                }
                return ConfigValue{.integer = integer};
            } catch (...) {
                return std::nullopt;
            }
        }
        case ConfigValueKind::Text:
            return ConfigValue{.text = value};
        case ConfigValueKind::Choice: {
            const auto canonical = canonical_choice(field, value);
            if (!canonical) return std::nullopt;
            return ConfigValue{.text = *canonical};
        }
    }
    return std::nullopt;
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
                entry["default"] = default_value.boolean;
                break;
            case ConfigValueKind::Integer:
                entry["kind"] = "integer";
                entry["minimum"] = field.minimum;
                entry["maximum"] = field.maximum;
                entry["default"] = default_value.integer;
                break;
            case ConfigValueKind::Text:
                entry["kind"] = "text";
                entry["default"] = default_value.text;
                break;
            case ConfigValueKind::Choice: {
                entry["kind"] = "choice";
                entry["default"] = default_value.text;
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
    return nlohmann::json{{"fields", std::move(fields)}};
}

}  // namespace llavon::ime

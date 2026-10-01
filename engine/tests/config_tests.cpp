#include "test_suites.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "config/config_schema.hpp"

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) saved_ = std::string(value);
    }

    ~ScopedEnv() {
        if (saved_) {
            setenv(name_.c_str(), saved_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

}  // namespace

// The schema is the single source of truth for every config option: each field
// must carry a unique key, a default, and survive the generic JSON codec and
// the INI parser without per-field code.
static bool test_config_schema() {
    using namespace llavon::ime;
    bool ok = true;
    const auto& fields = config_fields();
    ok = ok && fields.size() >= 17;

    std::vector<std::string> keys;
    std::vector<std::string> ini_keys;
    for (const auto& field : fields) {
        ok = ok && !field.key.empty() && !field.ini_key.empty() && !field.label.empty() && !field.group.empty();
        ok = ok && std::find(keys.begin(), keys.end(), field.key) == keys.end();
        ok = ok && std::find(ini_keys.begin(), ini_keys.end(), field.ini_key) == ini_keys.end();
        keys.push_back(field.key);
        ini_keys.push_back(field.ini_key);
        if (field.kind == ConfigValueKind::Choice) ok = ok && !field.choices.empty();
        if (field.kind == ConfigValueKind::Integer) ok = ok && field.minimum <= field.maximum;
    }

    // Every field round-trips through the generic JSON codec.
    for (const auto& field : fields) {
        Config config = default_config();
        const auto original = config_field_value(config, field);
        ConfigValue changed = original;
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                changed = !std::get<bool>(original);
                break;
            case ConfigValueKind::Integer:
                changed = std::get<int>(original) == field.maximum ? field.minimum : field.maximum;
                break;
            case ConfigValueKind::Text:
                changed = std::get<std::string>(original) + "-schema";
                break;
            case ConfigValueKind::Choice:
                for (const auto& choice : field.choices) {
                    if (choice.value != std::get<std::string>(original)) {
                        changed = choice.value;
                        break;
                    }
                }
                break;
        }
        ok = ok && set_config_field_value(config, field, changed);
        const auto decoded = config_from_json(to_json(config));
        const auto roundtrip = config_field_value(decoded, field);
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                ok = ok && roundtrip == changed;
                break;
            case ConfigValueKind::Integer:
                ok = ok && roundtrip == changed;
                break;
            case ConfigValueKind::Text:
            case ConfigValueKind::Choice:
                ok = ok && roundtrip == changed;
                break;
        }
    }

    // The exported schema describes every field for host UIs.
    const auto schema = config_schema_json();
    ok = ok && std::ranges::none_of(fields, [](const auto& field) {
        return field.key == "smart_model_preview" || field.ini_key == "SmartModelPreview";
    });
    const auto legacy = config_from_json({{"smart_english", true}, {"smart_model_preview", false}});
    ok = ok && legacy.smart_english && legacy.smart_model_preview;
    ok = ok && !to_json(legacy).contains("smart_model_preview");
    ok = ok && schema.contains("fields") && schema["fields"].size() == fields.size();
    for (size_t i = 0; i < fields.size() && i < schema["fields"].size(); ++i) {
        const auto& entry = schema["fields"].at(i);
        const auto& field = fields[i];
        ok = ok && entry["key"] == field.key && entry["ini_key"] == field.ini_key;
        ok = ok && entry["label"] == field.label && entry["group"] == field.group;
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                ok = ok && entry["kind"] == "boolean";
                break;
            case ConfigValueKind::Integer:
                ok = ok && entry["kind"] == "integer";
                ok = ok && entry["minimum"] == field.minimum && entry["maximum"] == field.maximum;
                break;
            case ConfigValueKind::Text:
                ok = ok && entry["kind"] == "text";
                break;
            case ConfigValueKind::Choice:
                ok = ok && entry["kind"] == "choice";
                ok = ok && entry["choices"].size() == field.choices.size();
                break;
        }
    }

    // INI spellings: booleans, integer bounds and choice labels/aliases.
    for (const auto& field : fields) {
        const auto value = default_config_field_value(field);
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                ok = ok && config_value_from_ini(field, "True").has_value();
                ok = ok && config_value_from_ini(field, "0").has_value();
                ok = ok && !config_value_from_ini(field, "yes").has_value();
                break;
            case ConfigValueKind::Integer:
                ok = ok && config_value_from_ini(field, std::to_string(std::get<int>(value))).has_value();
                ok = ok && !config_value_from_ini(field, std::to_string(field.maximum + 1)).has_value();
                for (const auto& spelling : {"+" + std::to_string(field.maximum),
                                             " \t+" + std::to_string(field.maximum)}) {
                    const auto parsed = config_value_from_ini(field, spelling);
                    ok = ok && parsed && std::get<int>(*parsed) == field.maximum;
                }
                ok = ok && !config_value_from_ini(field, "2tail").has_value();
                ok = ok && !config_value_from_ini(field, "999999999999999999999").has_value();
                {
                    auto config = default_config();
                    const auto before = to_json(config);
                    ok = ok && !set_config_field_value(config, field, ConfigValue{std::string("2")});
                    ok = ok && to_json(config) == before;
                }
                break;
            case ConfigValueKind::Text:
                ok = ok && config_value_from_ini(field, "text").has_value();
                break;
            case ConfigValueKind::Choice:
                for (const auto& choice : field.choices) {
                    const auto label = config_value_from_ini(field, choice.label);
                    const auto canonical = config_value_from_ini(field, choice.value);
                    ok = ok && label && std::get<std::string>(*label) == choice.value;
                    ok = ok && canonical && std::get<std::string>(*canonical) == choice.value;
                    for (const auto& alias : choice.aliases) {
                        const auto parsed = config_value_from_ini(field, alias);
                        ok = ok && parsed && std::get<std::string>(*parsed) == choice.value;
                    }
                }
                ok = ok && !config_value_from_ini(field, "diagonal").has_value();
                break;
        }
    }
    return ok;
}

int run_config_tests() {
    bool ok = test_config_schema();
    auto cfg = llavon::ime::default_config();
    ok = ok && cfg.context_length == 512;
    ok = ok && cfg.thread_count >= 1;
    ok = ok && cfg.gpu_layers == 999;
    ok = ok && cfg.idle_timeout_seconds == 1800;
    ok = ok && cfg.keyboard_layout == "standard";
    ok = ok && cfg.selection_keys == "1234567890";
    ok = ok && cfg.selection_key_count == 10;
    ok = ok && cfg.candidate_page_size == 10;
    ok = ok && cfg.candidate_layout == "not_set";
    ok = ok && cfg.space_selects_candidate;
    ok = ok && cfg.select_phrase == "before_cursor";
    ok = ok && !cfg.move_cursor_after_selection;
    ok = ok && !cfg.esc_clears_entire_buffer;
    ok = ok && cfg.caps_lock_inputs_bopomofo;
    ok = ok && cfg.shift_letter_keys == "directly_output_uppercase";

    auto json = llavon::ime::to_json(cfg);
    auto roundtrip = llavon::ime::config_from_json(json);
    ok = ok && roundtrip.context_length == cfg.context_length;
    ok = ok && roundtrip.idle_timeout_seconds == cfg.idle_timeout_seconds;
    ok = ok && roundtrip.selection_keys == cfg.selection_keys;
    ok = ok && roundtrip.select_phrase == cfg.select_phrase;
    ok = ok && roundtrip.caps_lock_inputs_bopomofo == cfg.caps_lock_inputs_bopomofo;
    ok = ok && roundtrip.shift_letter_keys == cfg.shift_letter_keys;
    ok = ok && roundtrip.keyboard_layout == cfg.keyboard_layout;
    ok = ok && llavon::ime::socket_path().filename() == "ime.sock";
    ok = ok && llavon::ime::pid_path().filename() == "service.pid";

    // JSON round-trip preserves the Hsu layout.
    cfg.keyboard_layout = "hsu";
    const auto hsu_json = llavon::ime::to_json(cfg);
    ok = ok && hsu_json.at("keyboard_layout").get<std::string>() == "hsu";
    const auto hsu_roundtrip = llavon::ime::config_from_json(hsu_json);
    ok = ok && hsu_roundtrip.keyboard_layout == "hsu";
    cfg.keyboard_layout = "standard";

    ScopedEnv xdg_config("XDG_CONFIG_HOME");
    ScopedEnv xdg_runtime("XDG_RUNTIME_DIR");
    ScopedEnv home("HOME");
    ScopedEnv config_override("LLAVON_IME_CONFIG_PATH");
    ScopedEnv phrase_overrides_override("LLAVON_IME_PHRASE_OVERRIDES_PATH");
    setenv("XDG_CONFIG_HOME", "", 1);
    setenv("XDG_RUNTIME_DIR", "", 1);
    setenv("HOME", "/tmp/ime-home", 1);
    unsetenv("LLAVON_IME_CONFIG_PATH");
    unsetenv("LLAVON_IME_PHRASE_OVERRIDES_PATH");
    ok = ok && llavon::ime::config_path() == "/tmp/ime-home/.config/fcitx5/conf/llavon-ime.conf";
    ok = ok && llavon::ime::legacy_config_path() == "/tmp/ime-home/.config/llavon-ime/config.json";
    ok = ok && llavon::ime::phrase_overrides_path() == "/tmp/ime-home/.config/llavon-ime/phrase_overrides.txt";

    // The pre-rename environment variable names keep working.
    {
        ScopedEnv legacy_config_override("IME_FCITX5_CONFIG_PATH");
        ScopedEnv legacy_phrase_overrides_override("IME_FCITX5_PHRASE_OVERRIDES_PATH");
        setenv("IME_FCITX5_CONFIG_PATH", "/tmp/ime-legacy/llavon-ime.conf", 1);
        setenv("IME_FCITX5_PHRASE_OVERRIDES_PATH", "/tmp/ime-legacy/phrase_overrides.txt", 1);
        ok = ok && llavon::ime::config_path() == "/tmp/ime-legacy/llavon-ime.conf";
        ok = ok && llavon::ime::phrase_overrides_path() == "/tmp/ime-legacy/phrase_overrides.txt";
        unsetenv("IME_FCITX5_CONFIG_PATH");
        unsetenv("IME_FCITX5_PHRASE_OVERRIDES_PATH");
    }
    ok = ok && llavon::ime::runtime_dir() == std::filesystem::temp_directory_path() / "llavon-ime";

    const auto config_root = std::filesystem::temp_directory_path() / "llavon-ime-config-test";
    std::filesystem::remove_all(config_root);
    setenv("XDG_CONFIG_HOME", config_root.c_str(), 1);
    std::filesystem::create_directories(llavon::ime::config_path().parent_path());
    {
        std::ofstream output(llavon::ime::config_path());
        output << "ModelPath=/tmp/model.gguf\n"
               << "ContextLength=1024\n"
               << "ThreadCount=2\n"
               << "GpuLayers=3\n"
               << "IdleTimeoutSeconds=4\n"
               << "BopomofoKeyboardLayout=standard\n"
               << "SelectionKeys=asdfghjkl\n"
               << "SelectionKeysCount=6\n"
               << "CandidatePageSize=5\n"
               << "CandidateLayout=Horizontal\n"
               << "ChooseCandidateUsingSpace=False\n"
               << "SelectPhrase=after_cursor\n"
               << "MoveCursorAfterSelection=True\n"
               << "EscKeyClearsEntireComposingBuffer=True\n"
               << "CapsLockInputsBopomofo=False\n"
               << "SmartModelPreview=False\n"
               << "ShiftLetterKeys=直接放入組字區\n";
    }
    const auto loaded = llavon::ime::load_config();
    ok = ok && loaded.smart_model_preview;
    ok = ok && loaded.model_path == "/tmp/model.gguf";
    ok = ok && loaded.context_length == 1024;
    ok = ok && loaded.thread_count == 2;
    ok = ok && loaded.gpu_layers == 3;
    ok = ok && loaded.idle_timeout_seconds == 4;
    ok = ok && loaded.keyboard_layout == "standard";
    ok = ok && loaded.selection_keys == "asdfghjkl";
    ok = ok && loaded.selection_key_count == 6;
    ok = ok && loaded.candidate_page_size == 5;
    ok = ok && loaded.candidate_layout == "horizontal";
    ok = ok && !loaded.space_selects_candidate;
    ok = ok && loaded.select_phrase == "after_cursor";
    ok = ok && loaded.move_cursor_after_selection;
    ok = ok && loaded.esc_clears_entire_buffer;
    ok = ok && !loaded.caps_lock_inputs_bopomofo;
    ok = ok && loaded.shift_letter_keys == "directly_put_to_buffer";

    {
        std::ofstream output(llavon::ime::config_path());
        output << "ModelPath=\"/Library/Application Support/llavon-ime/models/model.gguf\"\n";
    }
    const auto quoted_space_loaded = llavon::ime::load_config();
    ok = ok && quoted_space_loaded.model_path == "/Library/Application Support/llavon-ime/models/model.gguf";

    {
        std::ofstream output(llavon::ime::config_path());
        output << "ModelPath=/Library/Application\\ Support/llavon-ime/models/model.gguf\n";
    }
    const auto escaped_space_loaded = llavon::ime::load_config();
    ok = ok && escaped_space_loaded.model_path == "/Library/Application Support/llavon-ime/models/model.gguf";

    {
        std::ofstream output(llavon::ime::config_path());
        output << "BopomofoKeyboardLayout=標準\n"
               << "SelectionKeys=左手鍵\n"
               << "CandidateLayout=垂直\n"
               << "SelectPhrase=游標後\n"
               << "CapsLockInputsBopomofo=True\n";
    }
    const auto chinese_loaded = llavon::ime::load_config();
    ok = ok && chinese_loaded.keyboard_layout == "standard";
    ok = ok && chinese_loaded.selection_keys == "asdfzxcvb";
    ok = ok && chinese_loaded.candidate_layout == "vertical";
    ok = ok && chinese_loaded.select_phrase == "after_cursor";
    ok = ok && chinese_loaded.caps_lock_inputs_bopomofo;

    // Legacy ShiftLetterKeys value aliases normalize to the new name.
    {
        std::ofstream output(llavon::ime::config_path());
        output << "ShiftLetterKeys=小寫放入組字區\n";
    }
    const auto legacy_shift_keys_loaded = llavon::ime::load_config();
    ok = ok && legacy_shift_keys_loaded.shift_letter_keys == "directly_put_to_buffer";

    // Legacy nine-key digit configuration normalizes to Chewing's 1..9,0 order.
    {
        std::ofstream output(llavon::ime::config_path());
        output << "SelectionKeys=123456789\n";
    }
    const auto legacy_digit_keys_loaded = llavon::ime::load_config();
    ok = ok && legacy_digit_keys_loaded.selection_keys == "1234567890";

    // Hsu layout aliases normalize to "hsu"; invalid values fall back.
    for (const char* value : {"hsu", "Hsu", "許氏", "許氏鍵盤"}) {
        {
            std::ofstream output(llavon::ime::config_path());
            output << "BopomofoKeyboardLayout=" << value << "\n";
        }
        const auto hsu_loaded = llavon::ime::load_config();
        ok = ok && hsu_loaded.keyboard_layout == "hsu";
    }
    {
        std::ofstream output(llavon::ime::config_path());
        output << "BopomofoKeyboardLayout=diagonal\n";
    }
    const auto invalid_layout_loaded = llavon::ime::load_config();
    ok = ok && invalid_layout_loaded.keyboard_layout == llavon::ime::default_config().keyboard_layout;

    // An empty model path means "use the installed default", matching the
    // fcitx5 addon's INI rule, so settings UIs show the effective path.
    {
        const auto empty = llavon::ime::config_from_json(nlohmann::json::parse(R"({"model_path":""})"));
        ok = ok && empty.model_path == llavon::ime::default_config().model_path;

        const auto explicit_path =
            llavon::ime::config_from_json(nlohmann::json::parse(R"({"model_path":"/tmp/explicit.gguf"})"));
        ok = ok && explicit_path.model_path == "/tmp/explicit.gguf";
    }

    const auto invalid = llavon::ime::config_from_json(nlohmann::json::parse(
        R"({"model_path":"/tmp/valid.gguf","keyboard_layout":"diagonal","selection_keys":"bad","selection_key_count":99,"candidate_page_size":0,"candidate_layout":"diagonal","space_selects_candidate":"yes","select_phrase":"near_cursor","move_cursor_after_selection":true,"shift_letter_keys":"diagonal"})"));
    ok = ok && invalid.model_path == "/tmp/valid.gguf";
    ok = ok && invalid.keyboard_layout == llavon::ime::default_config().keyboard_layout;
    ok = ok && invalid.selection_keys == llavon::ime::default_config().selection_keys;
    ok = ok && invalid.selection_key_count == llavon::ime::default_config().selection_key_count;
    ok = ok && invalid.candidate_page_size == llavon::ime::default_config().candidate_page_size;
    ok = ok && invalid.candidate_layout == llavon::ime::default_config().candidate_layout;
    ok = ok && invalid.space_selects_candidate == llavon::ime::default_config().space_selects_candidate;
    ok = ok && invalid.select_phrase == llavon::ime::default_config().select_phrase;
    ok = ok && invalid.move_cursor_after_selection;
    ok = ok && invalid.caps_lock_inputs_bopomofo == llavon::ime::default_config().caps_lock_inputs_bopomofo;
    ok = ok && invalid.shift_letter_keys == llavon::ime::default_config().shift_letter_keys;

    std::filesystem::remove(llavon::ime::config_path());
    std::filesystem::create_directories(llavon::ime::legacy_config_path().parent_path());
    {
        std::ofstream output(llavon::ime::legacy_config_path());
        output << R"({"model_path":"/tmp/legacy.gguf","gpu_layers":7})";
    }
    const auto legacy_loaded = llavon::ime::load_config();
    ok = ok && legacy_loaded.model_path == "/tmp/legacy.gguf";
    ok = ok && legacy_loaded.gpu_layers == 7;

    bool malformed_uses_default = false;
    {
        std::ofstream output(llavon::ime::config_path());
        output << "not json";
    }
    try {
        const auto malformed = llavon::ime::load_config();
        malformed_uses_default = malformed.model_path == llavon::ime::default_config().model_path &&
                                 malformed.context_length == llavon::ime::default_config().context_length;
    } catch (...) {
        malformed_uses_default = false;
    }
    ok = ok && malformed_uses_default;
    std::filesystem::remove_all(config_root);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

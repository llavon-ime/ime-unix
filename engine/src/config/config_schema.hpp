#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace llavon::ime {

struct Config;

// Value of one config field, independent of the field's C++ type.
using ConfigValue = std::variant<bool, int, std::string>;

enum class ConfigValueKind { Boolean, Integer, Text, Choice };

// One selectable value of a Choice field: `value` is the canonical form shared
// by the engine and the JSON config, `label` is what the fcitx5 INI and the
// host UIs display, and `aliases` are older spellings accepted when reading
// existing files.
struct ConfigChoice {
    std::string value;
    std::string label;
    std::vector<std::string> aliases;
};

// One entry of the shared config schema. This list is the single place to add
// a configuration option: JSON/INI (de)serialization, the C ABI schema export,
// the fcitx5 addon config UI and the native macOS settings window all derive
// from it, so no per-platform code has to change.
struct ConfigField {
    std::string key;      // canonical JSON key, e.g. "candidate_page_size"
    std::string ini_key;  // fcitx5 INI key, e.g. "CandidatePageSize"
    std::string label;    // UI label
    std::string group;    // UI section title
    ConfigValueKind kind = ConfigValueKind::Text;
    int minimum = 0;  // Integer bounds
    int maximum = 0;
    std::vector<ConfigChoice> choices;
    std::variant<bool Config::*, int Config::*, std::string Config::*> member;
};

// The ordered schema; also the order host UIs present the fields in.
const std::vector<ConfigField>& config_fields();

ConfigValue config_field_value(const Config& config, const ConfigField& field);
bool set_config_field_value(Config& config, const ConfigField& field, const ConfigValue& value);
ConfigValue default_config_field_value(const ConfigField& field);

// Choice helpers. `canonical_choice` accepts the canonical value, the label or
// an alias; `choice_label` maps a canonical value back to its label.
std::optional<std::string> canonical_choice(const ConfigField& field, std::string_view value);
std::string choice_label(const ConfigField& field, std::string_view value);

// Parses the INI spelling of a value ("True"/"False"/"1"/"0" for booleans,
// decimal digits for integers, label or canonical value for choices).
std::optional<ConfigValue> config_value_from_ini(const ConfigField& field, const std::string& value);

// Schema for host UIs: key, label, group, kind, bounds, default and choices.
// Field order matches config_fields().
nlohmann::json config_schema_json();

}  // namespace llavon::ime

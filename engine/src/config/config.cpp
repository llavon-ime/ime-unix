#include "config/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "config/config_schema.hpp"
#include "util/env.hpp"

namespace llavon::ime {

namespace {

const char* non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string unescape_ini_value(std::string value) {
    const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
    std::string_view input(value);
    if (quoted) input = input.substr(1, input.size() - 2);

    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\' || i + 1 >= input.size()) {
            result.push_back(input[i]);
            continue;
        }

        const char escaped = input[++i];
        switch (escaped) {
            case '\\':
                result.push_back('\\');
                break;
            case '"':
                result.push_back('"');
                break;
            case ' ':
                result.push_back(' ');
                break;
            case 'n':
                if (quoted) {
                    result.push_back('\n');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'f':
                if (quoted) {
                    result.push_back('\f');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'r':
                if (quoted) {
                    result.push_back('\r');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 't':
                if (quoted) {
                    result.push_back('\t');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            case 'v':
                if (quoted) {
                    result.push_back('\v');
                } else {
                    result.push_back('\\');
                    result.push_back(escaped);
                }
                break;
            default:
                result.push_back('\\');
                result.push_back(escaped);
                break;
        }
    }
    return result;
}

std::map<std::string, std::string> read_simple_ini(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return {};

    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#' || stripped.front() == '[') continue;

        const auto separator = stripped.find('=');
        if (separator == std::string::npos) continue;

        auto key = trim(std::string_view(stripped).substr(0, separator));
        auto value = trim(std::string_view(stripped).substr(separator + 1));
        if (!key.empty()) fields[std::move(key)] = unescape_ini_value(std::move(value));
    }
    return fields;
}

std::string installed_default_model_path() {
#ifdef LLAVON_IME_INSTALLED_MODEL_PATH
    const std::filesystem::path path(LLAVON_IME_INSTALLED_MODEL_PATH);
    if (!path.empty() && std::filesystem::exists(path)) return path.string();
#endif
    return {};
}

Config with_default_model(Config cfg) {
    if (cfg.model_path.empty()) cfg.model_path = installed_default_model_path();
    return cfg;
}

Config config_from_fcitx_ini(const std::filesystem::path& path) {
    Config cfg = default_config();
    const auto fields = read_simple_ini(path);
    if (fields.empty()) return cfg;

    for (const auto& field : config_fields()) {
        const auto it = fields.find(field.ini_key);
        if (it == fields.end()) continue;
        if (const auto value = config_value_from_ini(field, it->second)) {
            (void)set_config_field_value(cfg, field, *value);
        }
    }
    return cfg;
}

Config load_legacy_json_config(const std::filesystem::path& path) {
    try {
        std::ifstream input(path);
        if (!input) return default_config();
        nlohmann::json json;
        input >> json;
        return config_from_json(json);
    } catch (...) {
        return default_config();
    }
}

#ifdef __APPLE__
std::filesystem::path macos_fcitx_config_path(const char* filename) {
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "fcitx5" / "conf" / filename;
    }
    return {};
}
#endif

std::filesystem::path fcitx_config_path_for(const char* filename) {
    if (const char* xdg = non_empty_env("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "fcitx5" / "conf" / filename;
    }
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / ".config" / "fcitx5" / "conf" / filename;
    }
    return std::filesystem::path(".") / "fcitx5" / "conf" / filename;
}

std::filesystem::path legacy_json_config_path_for(const char* directory) {
    if (const char* xdg = non_empty_env("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / directory / "config.json";
    }
    if (const char* home = non_empty_env("HOME")) {
        return std::filesystem::path(home) / ".config" / directory / "config.json";
    }
    return std::filesystem::path(".") / directory / "config.json";
}

void append_unique_path(std::vector<std::filesystem::path>& paths, std::filesystem::path path) {
    if (path.empty()) return;
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(std::move(path));
}

std::vector<std::filesystem::path> fcitx_config_paths() {
    std::vector<std::filesystem::path> paths;
    append_unique_path(paths, config_path());
#ifdef __APPLE__
    if (!env_with_legacy("LLAVON_IME_CONFIG_PATH", "IME_FCITX5_CONFIG_PATH")) {
        append_unique_path(paths, macos_fcitx_config_path("llavon-ime.conf"));
    }
#endif
    return paths;
}

}  // namespace

Config default_config() {
    Config cfg;
    cfg.model_path = installed_default_model_path();
    cfg.gpu_layers = 999;
    const auto threads = std::thread::hardware_concurrency();
    cfg.thread_count = threads == 0 ? 1 : static_cast<int>(threads);
    return cfg;
}

Config load_config() {
    std::error_code ec;
    for (const auto& path : fcitx_config_paths()) {
        if (std::filesystem::exists(path, ec) && !ec) return with_default_model(config_from_fcitx_ini(path));
        ec.clear();
    }

    if (const auto legacy_path = legacy_config_path(); std::filesystem::exists(legacy_path, ec) && !ec)
        return with_default_model(load_legacy_json_config(legacy_path));
    ec.clear();
    return default_config();
}

nlohmann::json to_json(const Config& cfg) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& field : config_fields()) {
        const auto value = config_field_value(cfg, field);
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                json[field.key] = std::get<bool>(value);
                break;
            case ConfigValueKind::Integer:
                json[field.key] = std::get<int>(value);
                break;
            case ConfigValueKind::Text:
            case ConfigValueKind::Choice:
                json[field.key] = std::get<std::string>(value);
                break;
        }
    }
    return json;
}

Config config_from_json(const nlohmann::json& json) {
    Config cfg = default_config();
    if (!json.is_object()) return cfg;

    for (const auto& field : config_fields()) {
        if (!json.contains(field.key)) continue;
        const auto& entry = json.at(field.key);
        std::optional<ConfigValue> value;
        switch (field.kind) {
            case ConfigValueKind::Boolean:
                if (entry.is_boolean()) value = ConfigValue{entry.get<bool>()};
                break;
            case ConfigValueKind::Integer:
                if (entry.is_number_integer()) {
                    const int integer = entry.get<int>();
                    if (integer >= field.minimum && integer <= field.maximum) {
                        value = ConfigValue{integer};
                    }
                }
                break;
            case ConfigValueKind::Text:
                if (entry.is_string()) value = ConfigValue{entry.get<std::string>()};
                break;
            case ConfigValueKind::Choice:
                if (entry.is_string()) {
                    if (const auto canonical = canonical_choice(field, entry.get<std::string>())) {
                        value = ConfigValue{*canonical};
                    }
                }
                break;
        }
        if (value) (void)set_config_field_value(cfg, field, *value);
    }
    // An empty model path means "use the installed default", the same rule the
    // fcitx5 addon applies to its INI and the legacy JSON config.
    return with_default_model(std::move(cfg));
}

std::filesystem::path config_path() {
    if (const char* override = env_with_legacy("LLAVON_IME_CONFIG_PATH", "IME_FCITX5_CONFIG_PATH")) {
        return override;
    }
    return fcitx_config_path_for("llavon-ime.conf");
}

std::filesystem::path legacy_config_path() {
    return legacy_json_config_path_for("llavon-ime");
}

std::filesystem::path phrase_overrides_path() {
    if (const char* override = env_with_legacy("LLAVON_IME_PHRASE_OVERRIDES_PATH",
                                                "IME_FCITX5_PHRASE_OVERRIDES_PATH")) {
        return override;
    }
    return legacy_config_path().parent_path() / "phrase_overrides.txt";
}

std::filesystem::path runtime_dir() {
    if (const char* xdg = non_empty_env("XDG_RUNTIME_DIR")) {
        return std::filesystem::path(xdg) / "llavon-ime";
    }
    return std::filesystem::temp_directory_path() / "llavon-ime";
}

std::filesystem::path socket_path() {
    return runtime_dir() / "ime.sock";
}

std::filesystem::path pid_path() {
    return runtime_dir() / "service.pid";
}

}  // namespace llavon::ime

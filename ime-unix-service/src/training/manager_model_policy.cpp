#include "manager_model_policy.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#ifdef __APPLE__
#include <notify.h>
#endif

namespace ime::unix_service::manager {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
[[maybe_unused]] std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

fs::path config_file() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "llavon-ime" / "config.json";
#else
    if (const char* value = std::getenv("LLAVON_IME_CONFIG_PATH"); value && *value) return value;
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "fcitx5" / "conf" / "llavon-ime.conf";
#endif
}

void write_model_path(const std::optional<std::string>& path) {
    const auto config = config_file();
    fs::create_directories(config.parent_path());
    const auto temporary = fs::path(config.string() + ".lora.partial");
#ifdef __APPLE__
    json values = json::object();
    if (fs::is_regular_file(config)) values = json::parse(std::ifstream(config));
    if (!values.is_object()) throw std::runtime_error("invalid input method settings");
    if (path) values["model_path"] = *path;
    else values.erase("model_path");
    { std::ofstream output(temporary, std::ios::trunc); output << values.dump(2) << '\n';
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#else
    std::vector<std::string> lines;
    std::ifstream input(config);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("ModelPath=")) lines.push_back(line);
    }
    { std::ofstream output(temporary, std::ios::trunc);
      for (const auto& value : lines) output << value << '\n';
      if (path) {
          std::string escaped;
          for (char ch : *path) {
              if (ch == '\\' || ch == '"') escaped += '\\';
              escaped += ch;
          }
          output << "ModelPath=\"" << escaped << "\"\n";
      }
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#endif
    if (::chmod(temporary.c_str(), 0600) != 0) throw std::runtime_error("cannot protect input method settings");
    fs::rename(temporary, config);
#ifdef __APPLE__
    (void)::notify_post("org.llavon-ime.lora.model-changed");
#else
    const auto child = ::fork();
    if (child == 0) { ::execlp("fcitx5-remote", "fcitx5-remote", "-r", static_cast<char*>(nullptr)); _exit(127); }
    if (child > 0) { int status; while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
#endif
}

bool has_pinned_trainer_stamp(const fs::path& executable) {
    try {
        return json::parse(std::ifstream(executable.parent_path() / "trainer-release.json")).at("commit")
               == LLAVON_IME_LORA_PINNED_COMMIT;
    } catch (...) { return false; }
}

bool has_trainer_libraries(const fs::path& directory, int depth = 0) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".so") || name.find(".so.") != std::string::npos || name.ends_with(".dylib")) return true;
        if (depth < 3 && entry.is_directory() && has_trainer_libraries(entry.path(), depth + 1)) return true;
    }
    return false;
}

bool trainer_usable(const fs::path& executable) {
    return fs::is_regular_file(executable) && has_pinned_trainer_stamp(executable) &&
           has_trainer_libraries(executable.parent_path());
}
}  // namespace

std::string configured_model_path() {
    try {
        std::ifstream input(config_file());
        if (!input) return {};
#ifdef __APPLE__
        return json::parse(input).value("model_path", std::string{});
#else
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with("ModelPath=")) continue;
            auto value = trim(line.substr(std::string("ModelPath=").size()));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            std::string unescaped;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '\\' && index + 1 < value.size()) ++index;
                unescaped += value[index];
            }
            return unescaped;
        }
        return {};
#endif
    } catch (...) { return {}; }
}

void use_model(const fs::path& path) { write_model_path(path.string()); }
void use_base_model() { write_model_path(std::nullopt); }

fs::path trainer_path(const fs::path& state) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH"); override && *override)
        return fs::absolute(override);
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (trainer_usable(managed)) return managed;
    if (trainer_usable(system)) return system;
    return fs::is_regular_file(managed) ? managed : system;
}

bool trainer_ready(const fs::path& executable, const fs::path& state) {
    if (!fs::is_regular_file(executable) || ::access(executable.c_str(), X_OK) != 0) return false;
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (executable != managed && executable != system) return true;
    return trainer_usable(executable);
}
}  // namespace ime::unix_service::manager

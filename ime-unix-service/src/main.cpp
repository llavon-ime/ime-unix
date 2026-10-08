#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "platform/server_strategy.hpp"
#include "pipe/protocol.hpp"
#include "platform/unix_socket_server.hpp"

namespace {

void print_usage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "  --model PATH\n"
              << "  --tables PATH\n"
              << "  --context-length N       (default 512)\n"
              << "  --threads N              (default 8)\n"
              << "  --gpu-layers auto|N      (default auto)\n"
              << "  --vulkan-pipeline-cache-dir PATH (default per-user cache; empty disables)\n"
              << "  --max-sessions N         (default 8)\n"
              << "  --max-idle-sessions N    (default 4)\n"
              << "  --max-concurrent-predictions N (default 2)\n"
              << "  --idle-timeout N         seconds (default 1800)\n"
              << "  --socket PATH             override runtime socket\n"
              << "  --pid PATH                override PID artifact\n";
}

const char* env_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::uint64_t positive_number(std::string_view value, const char* option) {
    if (value.empty() || value.front() == '-') throw std::runtime_error(std::string("invalid ") + option);
    std::size_t parsed = 0;
    unsigned long long number = 0;
    try {
        number = std::stoull(std::string(value), &parsed, 10);
    } catch (...) {
        throw std::runtime_error(std::string("invalid ") + option + ": " + std::string(value));
    }
    if (parsed != value.size() || number > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(std::string("invalid ") + option + ": " + std::string(value));
    }
    return number;
}

std::filesystem::path default_tables_dir(const char* executable) {
    if (const char* value = env_value("LLAVON_IME_TABLES_DIR")) return value;
    if (const char* value = env_value("LLAVON_IME_TABLE_DIR")) return value;
    if (const char* value = env_value("IME_FCITX5_TABLE_DIR")) return value;

    const auto has_required_tables = [](const std::filesystem::path& path) {
        return std::filesystem::is_regular_file(path / "bopomofo_char.json") &&
               std::filesystem::is_regular_file(path / "tokens" / "chars.json") &&
               std::filesystem::is_regular_file(path / "tokens" / "latin.json") &&
               std::filesystem::is_regular_file(path / "tokens" / "special_tokens.json") &&
               std::filesystem::is_regular_file(path / "tokens" / "bpmf.json");
    };

    const auto cwd = std::filesystem::current_path();
    for (const auto& candidate : {cwd / "ime-core" / "table",
                                  cwd.parent_path() / "ime-core" / "table", cwd / "tables"}) {
        if (has_required_tables(candidate)) return candidate;
    }

    std::error_code error;
    const auto executable_path = std::filesystem::absolute(executable, error);
    if (!error) {
        for (const auto& candidate : {executable_path.parent_path().parent_path() / "share" / "llavon-ime" / "tables",
                                      executable_path.parent_path().parent_path() / "tables"}) {
            if (has_required_tables(candidate)) return candidate;
        }
    }
    return cwd / "tables";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        ime::unix_service::UnixServerOptions options;
        options.runtime.tables_dir = default_tables_dir(argv[0]);

        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            auto require_value = [&](const char* option) -> std::string_view {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + option);
                return argv[++i];
            };
            if (argument == "--help") {
                print_usage(argv[0]);
                return 0;
            } else if (argument == "--model") {
                options.runtime.model_path = require_value("--model");
            } else if (argument == "--tables") {
                options.runtime.tables_dir = require_value("--tables");
            } else if (argument == "--vulkan-pipeline-cache-dir") {
                options.runtime.vulkan_pipeline_cache_dir = require_value("--vulkan-pipeline-cache-dir");
            } else if (argument == "--context-length") {
                options.runtime.context_length = static_cast<std::uint32_t>(positive_number(require_value("--context-length"), "--context-length"));
            } else if (argument == "--threads") {
                options.runtime.threads = static_cast<std::uint32_t>(positive_number(require_value("--threads"), "--threads"));
            } else if (argument == "--gpu-layers") {
                const auto value = require_value("--gpu-layers");
                if (value == "auto") {
                    options.runtime.gpu_layers = -2;
                } else {
                    options.runtime.gpu_layers = static_cast<int>(positive_number(value, "--gpu-layers"));
                }
            } else if (argument == "--max-sessions") {
                options.limits.max_sessions = positive_number(require_value("--max-sessions"), "--max-sessions");
            } else if (argument == "--max-idle-sessions") {
                options.limits.max_idle_sessions = positive_number(require_value("--max-idle-sessions"), "--max-idle-sessions");
            } else if (argument == "--max-concurrent-predictions") {
                options.limits.max_concurrent_predictions = positive_number(require_value("--max-concurrent-predictions"), "--max-concurrent-predictions");
            } else if (argument == "--idle-timeout") {
                options.limits.idle_timeout = std::chrono::seconds(positive_number(require_value("--idle-timeout"), "--idle-timeout"));
            } else if (argument == "--socket") {
                options.socket_path = std::filesystem::path(require_value("--socket"));
            } else if (argument == "--pid") {
                options.pid_path = std::filesystem::path(require_value("--pid"));
            } else {
                throw std::runtime_error("unknown option: " + std::string(argument));
            }
        }

        // Frontend-provided settings. The frontend keeps these in sync with its
        // settings file and the service inherits its environment, so a restart
        // applies the values the frontend has now instead of the ones it had
        // when it created its transport. They win over the command line.
        if (const char* value = env_value("LLAVON_IME_MODEL_PATH")) options.runtime.model_path = value;
        if (const char* value = std::getenv("LLAVON_IME_VULKAN_PIPELINE_CACHE_DIR"))
            options.runtime.vulkan_pipeline_cache_dir = value;
        if (const char* value = env_value("LLAVON_IME_CONTEXT_LENGTH"))
            options.runtime.context_length = static_cast<std::uint32_t>(positive_number(value, "--context-length"));
        if (const char* value = env_value("LLAVON_IME_THREADS"))
            options.runtime.threads = static_cast<std::uint32_t>(positive_number(value, "--threads"));
        if (const char* value = env_value("LLAVON_IME_GPU_LAYERS")) {
            const std::string_view layers = value;
            options.runtime.gpu_layers =
                layers == "auto" ? -2 : static_cast<int>(positive_number(layers, "--gpu-layers"));
        }
        if (const char* value = env_value("LLAVON_IME_IDLE_TIMEOUT"))
            options.limits.idle_timeout = std::chrono::seconds(positive_number(value, "--idle-timeout"));

        if (options.runtime.context_length == 0 ||
            options.runtime.context_length > ime::unix_service::protocol::kMaxContextCodeUnits) {
            throw std::runtime_error("--context-length is out of range");
        }
        if (options.runtime.threads == 0) throw std::runtime_error("--threads must be positive");
        std::clog << "[SRV] IME Unix Service starting\n";
        auto server = ime::unix_service::create_server_strategy(std::move(options));
        std::clog << "[SRV] platform server: " << server->name() << '\n';
        return server->run();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()) == "--help") return 0;
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}

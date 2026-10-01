#include "core_runtime.hpp"

#include "engine/stderr_logger.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace ime::unix_service {

CoreRuntime::CoreRuntime(RuntimeConfig config) : config_(std::move(config)) {}

void CoreRuntime::validate_configuration() const {
    if (!std::filesystem::is_regular_file(config_.model_path)) {
        throw std::runtime_error("model file not found: " + config_.model_path.string());
    }
    if (!std::filesystem::is_directory(config_.tables_dir)) {
        throw std::runtime_error("tables directory not found: " + config_.tables_dir.string());
    }

    constexpr std::array required_tables{
        "tokens/chars.json",
        "tokens/latin.json",
        "tokens/special_tokens.json",
        "tokens/bpmf.json",
        "bopomofo_char.json",
    };
    for (const auto* relative_path : required_tables) {
        const auto path = config_.tables_dir / relative_path;
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("required table file not found: " + path.string());
        }
    }
    if (config_.context_length == 0 || config_.threads == 0) {
        throw std::runtime_error("context length and threads must be positive");
    }
}

std::unique_ptr<llavon::ime::core::Session> CoreRuntime::create_session() {
    ensure_loaded();
    std::shared_ptr<llavon::ime::core::Core> core;
    {
        std::lock_guard lock(state_mutex_);
        core = core_;
    }
    if (!core) throw std::runtime_error("ime-core is unavailable");
    return core->create_session();
}

bool CoreRuntime::loaded() const noexcept {
    std::lock_guard lock(state_mutex_);
    return core_ != nullptr;
}

void CoreRuntime::ensure_loaded() {
    std::call_once(load_once_, [this]() {
        validate_configuration();
        auto logger = std::make_shared<StderrLogger>();
        auto core = std::make_shared<llavon::ime::core::Core>(llavon::ime::core::CoreConfig{
            .model_path = config_.model_path,
            .tables_dir = config_.tables_dir,
            .vulkan_pipeline_cache_dir = {},
            .context_length = config_.context_length,
            .threads = config_.threads,
            .gpu_layers = config_.gpu_layers,
            .inference_device = {},
            .logger = logger,
        });
        std::lock_guard lock(state_mutex_);
        core_ = std::move(core);
        logger_ = std::move(logger);
    });
}

}  // namespace ime::unix_service

#include "core_runtime.hpp"

#include "engine/stderr_logger.hpp"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace ime::unix_service {

std::filesystem::path detail::resolve_vulkan_pipeline_cache_dir(std::string_view cache_home, std::string_view home) {
    const std::filesystem::path cache_root(cache_home);
    const std::filesystem::path home_root(home);
#ifdef __APPLE__
    // Match the native macOS cache location even when an XDG environment is
    // inherited from a terminal. Vulkan remains optional on this platform.
    (void)cache_root;
    if (home_root.is_absolute()) return home_root / "Library/Caches/llavon-ime/vulkan";
#else
    // XDG directories must be absolute. Never create a cache relative to the
    // frontend's working directory or fall back to a shared temporary path.
    if (cache_root.is_absolute()) return cache_root / "llavon-ime/vulkan";
    if (home_root.is_absolute()) return home_root / ".cache/llavon-ime/vulkan";
#endif
    return {};
}

std::filesystem::path default_vulkan_pipeline_cache_dir() {
    const auto* cache_home = std::getenv("XDG_CACHE_HOME");
    const auto* home = std::getenv("HOME");
    return detail::resolve_vulkan_pipeline_cache_dir(cache_home ? cache_home : "", home ? home : "");
}

llavon::ime::core::CoreConfig RuntimeConfig::to_core_config(std::shared_ptr<llavon::ime::core::Logger> logger) const {
    return {
        .model_path = model_path,
        .tables_dir = tables_dir,
        .vulkan_pipeline_cache_dir = vulkan_pipeline_cache_dir,
        .context_length = context_length,
        .threads = threads,
        .gpu_layers = gpu_layers,
        .inference_device = {},
        .logger = std::move(logger),
    };
}

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
        auto core = std::make_shared<llavon::ime::core::Core>(config_.to_core_config(logger));
        std::lock_guard lock(state_mutex_);
        core_ = std::move(core);
        logger_ = std::move(logger);
    });
}

}  // namespace ime::unix_service

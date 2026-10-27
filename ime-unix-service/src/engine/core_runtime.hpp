#pragma once

#include <ime-core/core.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace ime::unix_service {

class StderrLogger;

namespace detail {

std::filesystem::path resolve_vulkan_pipeline_cache_dir(std::string_view cache_home, std::string_view home);

enum class RuntimeInferenceProvider : std::uint8_t {
    automatic,
    mlx,
    llama_cpp,
};

RuntimeInferenceProvider parse_inference_provider(std::string_view value);
llavon::ime::core::InferenceProvider resolve_inference_provider(RuntimeInferenceProvider provider) noexcept;
std::optional<llavon::ime::core::InferenceProvider> fallback_inference_provider(
    RuntimeInferenceProvider configured,
    llavon::ime::core::InferenceProvider failed) noexcept;

}  // namespace detail

std::filesystem::path default_vulkan_pipeline_cache_dir();

struct RuntimeConfig {
    std::filesystem::path model_path;
    std::filesystem::path tables_dir;
    std::filesystem::path vulkan_pipeline_cache_dir = default_vulkan_pipeline_cache_dir();
    detail::RuntimeInferenceProvider inference_provider = detail::RuntimeInferenceProvider::automatic;
    std::uint32_t context_length = 512;
    std::uint32_t threads = 8;
    int gpu_layers = -2;

    llavon::ime::core::CoreConfig to_core_config(std::shared_ptr<llavon::ime::core::Logger> logger = {}) const;
};

class CoreRuntime final {
public:
    explicit CoreRuntime(RuntimeConfig config);

    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;

    void validate_configuration() const;
    std::unique_ptr<llavon::ime::core::Session> create_session();
    bool loaded() const noexcept;

private:
    void ensure_loaded();

    RuntimeConfig config_;
    std::once_flag load_once_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<llavon::ime::core::Core> core_;
    std::shared_ptr<StderrLogger> logger_;
};

}  // namespace ime::unix_service

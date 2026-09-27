#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ime::unix_service {

// Version 1 of the provisional product presets, shared with the Windows
// manager so both frontends train with the same strengths. Keep the adapter
// structure fixed across strengths so a user can explicitly continue a
// compatible adapter.
enum class LoraTrainingStrength : std::int32_t {
    ultra_low = 0,
    low = 1,
    medium = 2,
    high = 3,
    advanced = 4,
};

struct LoraTrainingPreset {
    double learning_rate;
    int epochs;
};

constexpr LoraTrainingPreset lora_training_preset(LoraTrainingStrength strength) noexcept {
    switch (strength) {
        case LoraTrainingStrength::ultra_low: return {1e-8, 1};
        case LoraTrainingStrength::medium: return {3e-6, 2};
        case LoraTrainingStrength::high: return {1e-5, 5};
        case LoraTrainingStrength::low:
        case LoraTrainingStrength::advanced: return {1e-6, 1};
    }
    return {1e-6, 1};
}

// The CLI spelling stored in run requests; the presets never guess a strength
// from parameters, so the name is recorded explicitly.
constexpr std::string_view lora_strength_name(LoraTrainingStrength strength) noexcept {
    switch (strength) {
        case LoraTrainingStrength::ultra_low: return "ultra-low";
        case LoraTrainingStrength::low: return "low";
        case LoraTrainingStrength::medium: return "medium";
        case LoraTrainingStrength::high: return "high";
        case LoraTrainingStrength::advanced: return "advanced";
    }
    return "advanced";
}

constexpr std::string_view lora_strength_label(LoraTrainingStrength strength) noexcept {
    switch (strength) {
        case LoraTrainingStrength::ultra_low: return "極低";
        case LoraTrainingStrength::low: return "低";
        case LoraTrainingStrength::medium: return "中";
        case LoraTrainingStrength::high: return "高";
        case LoraTrainingStrength::advanced: return "進階";
    }
    return "進階";
}

constexpr std::optional<LoraTrainingStrength> lora_strength_from_name(std::string_view name) noexcept {
    if (name == "ultra-low") return LoraTrainingStrength::ultra_low;
    if (name == "low") return LoraTrainingStrength::low;
    if (name == "medium") return LoraTrainingStrength::medium;
    if (name == "high") return LoraTrainingStrength::high;
    if (name == "advanced") return LoraTrainingStrength::advanced;
    return std::nullopt;
}

}  // namespace ime::unix_service

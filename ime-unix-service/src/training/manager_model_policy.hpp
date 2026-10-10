#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ime::unix_service::manager {
std::string configured_model_path();
void use_model(const std::filesystem::path& path);
void use_base_model();
std::filesystem::path trainer_path(const std::filesystem::path& state);
bool trainer_ready(const std::filesystem::path& executable, const std::filesystem::path& state);
}  // namespace ime::unix_service::manager

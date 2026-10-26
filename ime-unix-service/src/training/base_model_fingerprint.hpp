#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ime::unix_service {

// Content identity shared with the Windows manager. A revision alone cannot
// distinguish a checkpoint whose remote files were replaced in place.
std::string compute_base_model_fingerprint(const std::filesystem::path& directory);
bool valid_base_model_fingerprint(std::string_view fingerprint) noexcept;

}  // namespace ime::unix_service

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ime::unix_service {

struct LoraHistoryParent {
    std::int64_t id;
    std::int64_t parent_id;
};

// The base model has ID 0. Returns nullopt for unknown IDs or invalid
// ancestry, mirroring the Windows manager's Tarjan shortcut.
std::optional<std::int64_t> tarjan_lca(
    std::span<const LoraHistoryParent> runs,
    std::int64_t first_id,
    std::int64_t second_id);

}  // namespace ime::unix_service

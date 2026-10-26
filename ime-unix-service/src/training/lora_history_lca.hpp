#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ime::unix_service {

struct LoraHistoryParent {
    std::int64_t id;
    std::int64_t parent_id;
};

// ID 0 is the virtual forest root; negative IDs identify distinct base model
// roots. Separate base trees intentionally have no common ancestor.
std::optional<std::int64_t> tarjan_lca(
    std::span<const LoraHistoryParent> runs,
    std::int64_t first_id,
    std::int64_t second_id);

}  // namespace ime::unix_service

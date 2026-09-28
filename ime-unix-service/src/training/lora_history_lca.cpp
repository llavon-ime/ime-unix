#include "lora_history_lca.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace ime::unix_service {

std::optional<std::int64_t> tarjan_lca(
    std::span<const LoraHistoryParent> runs,
    std::int64_t first_id,
    std::int64_t second_id) {
    std::vector<std::int64_t> ids{0};
    ids.reserve(runs.size() + 1);
    std::unordered_map<std::int64_t, std::size_t> index{{0, 0}};
    index.reserve(runs.size() + 1);
    for (const auto& run : runs) {
        if (run.id <= 0 || !index.emplace(run.id, ids.size()).second) return std::nullopt;
        ids.push_back(run.id);
    }
    const auto first = index.find(first_id);
    const auto second = index.find(second_id);
    if (first == index.end() || second == index.end()) return std::nullopt;

    std::vector<std::vector<std::size_t>> children(ids.size());
    for (std::size_t offset = 0; offset < runs.size(); ++offset) {
        const auto parent = index.find(runs[offset].parent_id);
        if (parent == index.end()) return std::nullopt;
        children[parent->second].push_back(offset + 1);
    }

    // Tarjan's offline LCA: after each child is visited, union its set with
    // the parent's set and label that set with the parent. When both query
    // nodes are black, the other node's set label is their LCA.
    std::vector<std::size_t> disjoint_parent(ids.size());
    std::iota(disjoint_parent.begin(), disjoint_parent.end(), 0);
    std::vector<std::size_t> set_size(ids.size(), 1);
    std::vector<std::size_t> ancestor(ids.size());
    std::vector<bool> black(ids.size(), false);
    std::size_t visited = 0;
    std::optional<std::size_t> answer;

    const auto find_set = [&](std::size_t node) {
        std::size_t root = node;
        while (disjoint_parent[root] != root) root = disjoint_parent[root];
        while (disjoint_parent[node] != node) {
            const auto next = disjoint_parent[node];
            disjoint_parent[node] = root;
            node = next;
        }
        return root;
    };
    const auto unite = [&](std::size_t left, std::size_t right) {
        left = find_set(left);
        right = find_set(right);
        if (left == right) return;
        if (set_size[left] < set_size[right]) std::swap(left, right);
        disjoint_parent[right] = left;
        set_size[left] += set_size[right];
    };
    struct Frame {
        std::size_t node;
        std::size_t next_child = 0;
    };
    std::vector<Frame> stack{{0}};
    ancestor[0] = 0;
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.next_child < children[frame.node].size()) {
            const auto child = children[frame.node][frame.next_child++];
            ancestor[find_set(child)] = child;
            stack.push_back({child});
            continue;
        }
        const auto node = frame.node;
        black[node] = true;
        ++visited;
        if (node == first->second && black[second->second]) {
            answer = ancestor[find_set(second->second)];
        } else if (node == second->second && black[first->second]) {
            answer = ancestor[find_set(first->second)];
        }
        stack.pop_back();
        if (!stack.empty()) {
            const auto parent = stack.back().node;
            unite(parent, node);
            ancestor[find_set(parent)] = parent;
        }
    }
    if (visited != ids.size() || !answer) return std::nullopt;
    return ids[*answer];
}

}  // namespace ime::unix_service

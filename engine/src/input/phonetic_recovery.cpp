#include "input/phonetic_recovery.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace llavon::ime {
namespace {

constexpr size_t kLayoutCount = 7;
constexpr size_t kSpellingsPerState = 3;
constexpr size_t kMaxSearchStates = 4096;
constexpr size_t kMaxCachedQueries = 128;
// Supplying an unseen phonetic key is weaker evidence than removing an
// observed extra key; an actual adjacent repeat supplies the strongest cue.
constexpr double kMissingKeyCost = 1.5;
constexpr double kExtraKeyCost = 0.85;
constexpr double kDuplicateKeyCost = 0.5;

struct TrieNode {
    std::vector<std::pair<char16_t, size_t>> children;
    std::vector<size_t> terminals;
};

struct ReadingEntry {
    std::u16string body;
    char32_t tone;
    std::u16string reading;
    std::shared_ptr<const std::vector<char32_t>> candidates;
};

struct ReadingIndex {
    std::vector<ReadingEntry> entries;
    std::vector<TrieNode> bodies{1};
    std::vector<TrieNode> complete{1};
};

size_t layout_index(BopomofoKeyboardLayout layout) {
    switch (layout) {
        case BopomofoKeyboardLayout::Standard: return 0;
        case BopomofoKeyboardLayout::Hsu: return 1;
        case BopomofoKeyboardLayout::Ibm: return 2;
        case BopomofoKeyboardLayout::Et: return 3;
        case BopomofoKeyboardLayout::GinYieh: return 4;
        case BopomofoKeyboardLayout::Et26: return 5;
        case BopomofoKeyboardLayout::DachenCp26: return 6;
    }
    return 0;
}

size_t child_node(const std::vector<TrieNode>& trie, size_t node, char16_t key) {
    const auto& children = trie[node].children;
    const auto found = std::ranges::find_if(children, [key](const auto& child) { return child.first == key; });
    return found == children.end() ? std::numeric_limits<size_t>::max() : found->second;
}

void insert(std::vector<TrieNode>& trie, std::u16string_view spelling, size_t entry) {
    size_t node = 0;
    for (const auto key : spelling) {
        size_t next = child_node(trie, node, key);
        if (next == std::numeric_limits<size_t>::max()) {
            next = trie.size();
            trie.emplace_back();
            trie[node].children.emplace_back(key, next);
        }
        node = next;
    }
    trie[node].terminals.push_back(entry);
}

struct Position { int x; int y; };

std::optional<Position> physical_position(char16_t key) {
    constexpr std::array<std::u16string_view, 4> rows{u"1234567890-=", u"qwertyuiop[]\\", u"asdfghjkl;'", u"zxcvbnm,./"};
    constexpr std::array<int, 4> offsets{0, 1, 2, 4};
    for (size_t row = 0; row < rows.size(); ++row) {
        const auto column = rows[row].find(key);
        if (column != std::u16string_view::npos) {
            return Position{static_cast<int>(column) * 4 + offsets[row], static_cast<int>(row) * 4};
        }
    }
    return std::nullopt;
}

bool neighboring_keys(char16_t raw, char16_t corrected) {
    const auto first = physical_position(raw);
    const auto second = physical_position(corrected);
    if (!first || !second) return false;
    const int dx = first->x - second->x;
    const int dy = first->y - second->y;
    return dx * dx + dy * dy <= 32;
}

// Prefer initial/medial/final order when several natural spellings describe
// the same state. Compact-key cycles remain part of the state graph.
std::vector<char16_t> body_alphabet(BopomofoKeyboardLayout layout) {
    std::vector<std::pair<int, char16_t>> sorted;
    for (char16_t key = u'!'; key <= u'~'; ++key) {
        if (key >= u'A' && key <= u'Z') continue;
        Syllable syllable;
        const auto step = apply_bopomofo_key(syllable, layout, key, false);
        if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension || syllable.empty()) continue;
        const int order = syllable.has_initial() ? 0 : syllable.has_medial() ? 1 : 2;
        sorted.emplace_back(order, key);
    }
    std::ranges::sort(sorted);
    std::vector<char16_t> result;
    for (const auto& [order, key] : sorted) {
        (void)order;
        result.push_back(key);
    }
    return result;
}

std::vector<char16_t> tone_alphabet(BopomofoKeyboardLayout layout) {
    std::vector<char16_t> result{u' '};
    for (char16_t key = u'!'; key <= u'~'; ++key) {
        if (is_bopomofo_tone_key(key, layout)) result.push_back(key);
    }
    return result;
}

}  // namespace

struct PhoneticRecovery::Impl {
    explicit Impl(LookupFn supplied_lookup) : lookup(std::move(supplied_lookup)) {}

    LookupFn lookup;
    std::mutex mutex;
    std::array<std::shared_ptr<const ReadingIndex>, kLayoutCount> indexes;
    std::unordered_map<std::u16string, std::shared_ptr<const std::vector<char32_t>>> readings;
    // Entries use offsets relative to their bounded raw spelling. A pending
    // prefix usually leaves all but the final eight-key windows unchanged.
    struct CachedQuery {
        std::shared_ptr<const std::vector<RecoveredSyllable>> value;
        std::list<std::u16string>::iterator position;
    };
    std::unordered_map<std::u16string, CachedQuery> queries;
    std::list<std::u16string> query_order;

    std::shared_ptr<const std::vector<RecoveredSyllable>> cached_query(const std::u16string& key) {
        std::lock_guard lock(mutex);
        const auto found = queries.find(key);
        if (found == queries.end()) return nullptr;
        // Appending one key reuses older windows at every preceding position.
        // Keep those hot windows rather than evicting them by insertion age.
        query_order.splice(query_order.end(), query_order, found->second.position);
        return found->second.value;
    }

    void remember_query(const std::u16string& key, const std::vector<RecoveredSyllable>& result) {
        auto value = std::make_shared<const std::vector<RecoveredSyllable>>(result);
        std::lock_guard lock(mutex);
        if (queries.contains(key)) return;
        query_order.push_back(key);
        queries.emplace(key, CachedQuery{std::move(value), std::prev(query_order.end())});
        if (query_order.size() > kMaxCachedQueries) {
            queries.erase(query_order.front());
            query_order.pop_front();
        }
    }

    std::shared_ptr<const std::vector<char32_t>> candidates(const std::u16string& reading) {
        const auto cached = readings.find(reading);
        if (cached != readings.end()) return cached->second;
        auto result = std::make_shared<const std::vector<char32_t>>(lookup ? lookup(reading) : std::vector<char32_t>{});
        readings.emplace(reading, result);
        return result;
    }

    std::shared_ptr<const ReadingIndex> index(BopomofoKeyboardLayout layout) {
        std::lock_guard lock(mutex);
        auto& cached = indexes[layout_index(layout)];
        if (cached) return cached;
        auto result = std::make_shared<ReadingIndex>();
        const auto alphabet = body_alphabet(layout);
        const auto tones = tone_alphabet(layout);
        struct State { Syllable syllable; std::u16string body; };
        struct BestState { size_t length; size_t count; };
        std::unordered_map<std::u16string, BestState> visited;
        visited.emplace(std::u16string{}, BestState{0, 1});
        std::deque<State> queue;
        queue.push_back({{}, {}});
        while (!queue.empty()) {
            auto state = std::move(queue.front());
            queue.pop_front();
            if (!state.body.empty()) {
                for (const auto tone : tones) {
                    auto completed = state.syllable;
                    if (apply_bopomofo_key(completed, layout, tone, false).status != BopomofoKeyStatus::Completed) continue;
                    const auto reading = completed.text();
                    auto characters = candidates(reading);
                    if (characters->empty()) continue;
                    const size_t entry = result->entries.size();
                    result->entries.push_back({state.body, tone, reading, std::move(characters)});
                    insert(result->bodies, state.body, entry);
                    auto spelling = state.body;
                    spelling.push_back(tone);
                    insert(result->complete, spelling, entry);
                }
            }
            if (state.body.size() == kMaxBodyKeys) continue;
            for (const auto key : alphabet) {
                auto next = state.syllable;
                const auto step = apply_bopomofo_key(next, layout, key, false);
                if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension || next.has_tone()) continue;
                auto body = state.body;
                body.push_back(key);
                const auto text = next.text();
                auto [found, added] = visited.try_emplace(text, BestState{body.size(), 0});
                if (!added && (body.size() > found->second.length || found->second.count >= kSpellingsPerState)) continue;
                ++found->second.count;
                queue.push_back({std::move(next), std::move(body)});
            }
        }
        cached = std::move(result);
        return cached;
    }
};

PhoneticRecovery::PhoneticRecovery(LookupFn lookup) : impl_(std::make_shared<Impl>(std::move(lookup))) {}

std::vector<RecoveredSyllable> PhoneticRecovery::recover_at(std::u16string_view raw, size_t begin,
                                                          BopomofoKeyboardLayout layout) const {
    if (begin >= raw.size() || !physical_position(raw[begin])) return {};
    const size_t original_begin = begin;
    raw = raw.substr(begin, kMaxRawSpan);
    begin = 0;
    std::u16string cache_key(1, static_cast<char16_t>(layout_index(layout)));
    cache_key.append(raw);
    if (const auto cached = impl_->cached_query(cache_key)) {
        auto result = *cached;
        for (auto& entry : result) {
            entry.begin += original_begin;
            entry.end += original_begin;
        }
        return result;
    }
    const auto index = impl_->index(layout);
    const size_t limit = raw.size();
    std::vector<RecoveredSyllable> result;
    const auto search = [&](const std::vector<TrieNode>& trie, bool missing_tone) {
        struct State { size_t node; size_t position; double cost; bool edited; };
        std::vector<State> pending{{0, begin, 0.0, false}};
        size_t searched = 0;
        while (!pending.empty() && searched < kMaxSearchStates) {
            const auto state = pending.back();
            pending.pop_back();
            ++searched;
            if (state.position > begin && (missing_tone || state.edited)) {
                for (const auto entry_index : trie[state.node].terminals) {
                    const auto& entry = index->entries[entry_index];
                    result.push_back({begin, state.position, entry.body, entry.tone, entry.reading,
                                      *entry.candidates, state.cost, missing_tone, state.edited});
                }
            }
            if (state.position > limit) continue;
            if (!state.edited) {
                // A missing physical key may be supplied without consuming
                // raw input. Space is a boundary, not an editable body key.
                for (const auto& [key, node] : trie[state.node].children) {
                    if (key != u' ') pending.push_back({node, state.position, kMissingKeyCost, true});
                }
            }
            if (state.position == limit) continue;
            const char16_t key = raw[state.position];
            const auto exact = child_node(trie, state.node, key);
            if (exact != std::numeric_limits<size_t>::max()) {
                pending.push_back({exact, state.position + 1, state.cost, state.edited});
            }
            if (state.edited || key == u' ' || !physical_position(key)) continue;
            // An extra physical key is skipped once; provenance still covers
            // it because the returned end offset advances past that key.
            if (!(missing_tone && is_bopomofo_tone_key(key, layout) &&
                  !trie[state.node].terminals.empty())) {
                // An explicit tone after a valid body is a tone choice, not
                // an extra key beside an omitted tone. Preserve that reason
                // and its one-edit cost for the complete-reading search.
                const bool duplicate = (state.position > begin && raw[state.position - 1] == key) ||
                    (state.position + 1 < limit && raw[state.position + 1] == key);
                pending.push_back({state.node, state.position + 1,
                                   duplicate ? kDuplicateKeyCost : kExtraKeyCost, true});
            }
            for (const auto& [corrected, node] : trie[state.node].children) {
                if (corrected != key && neighboring_keys(key, corrected)) {
                    pending.push_back({node, state.position + 1, 1.0, true});
                } else if (corrected != key && !missing_tone &&
                           is_bopomofo_tone_key(key, layout) && is_bopomofo_tone_key(corrected, layout)) {
                    // Choosing a wrong tone is a phonetic error even when the
                    // two physical keys are far apart. Only completed tone
                    // endpoints qualify: compact-layout tone letters can also
                    // be body consonants, which retain the nearby-key rule.
                    // Do not continue into children after this tone change.
                    for (const auto entry_index : trie[node].terminals) {
                        const auto& entry = index->entries[entry_index];
                        result.push_back({begin, state.position + 1, entry.body, entry.tone,
                                          entry.reading, *entry.candidates, 1.0, false, true, false});
                    }
                }
            }
            if (state.position + 1 < limit && key != raw[state.position + 1] && raw[state.position + 1] != u' ') {
                const auto first = child_node(trie, state.node, raw[state.position + 1]);
                if (first != std::numeric_limits<size_t>::max()) {
                    const auto second = child_node(trie, first, key);
                    if (second != std::numeric_limits<size_t>::max()) pending.push_back({second, state.position + 2, 0.75, true});
                }
            }
        }
    };
    search(index->bodies, true);
    search(index->complete, false);
    std::ranges::sort(result, [](const auto& first, const auto& second) {
        if (first.edit_cost != second.edit_cost) return first.edit_cost < second.edit_cost;
        if (first.end != second.end) return first.end > second.end;
        if (first.missing_tone != second.missing_tone) return first.missing_tone;
        return first.reading < second.reading;
    });
    std::vector<RecoveredSyllable> unique;
    unique.reserve(std::min(result.size(), kMaxSuggestionsPerPosition));
    for (auto& entry : result) {
        if (std::ranges::any_of(unique, [&](const auto& previous) {
                return previous.end == entry.end && previous.reading == entry.reading;
            })) continue;
        unique.push_back(std::move(entry));
        if (unique.size() == kMaxSuggestionsPerPosition) break;
    }
    impl_->remember_query(cache_key, unique);
    for (auto& entry : unique) {
        entry.begin += original_begin;
        entry.end += original_begin;
    }
    return unique;
}

std::vector<RecoveredSyllable> PhoneticRecovery::recover(std::u16string_view raw,
                                                       BopomofoKeyboardLayout layout) const {
    std::vector<RecoveredSyllable> result;
    for (size_t begin = 0; begin < raw.size(); ++begin) {
        auto next = recover_at(raw, begin, layout);
        result.insert(result.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
    }
    return result;
}

std::vector<RecoveredSyllable> PhoneticRecovery::recover_initial_at(std::u16string_view raw, size_t begin,
                                                                  BopomofoKeyboardLayout layout) const {
    if (begin >= raw.size() || !physical_position(raw[begin])) return {};
    Syllable initial;
    const auto step = apply_bopomofo_key(initial, layout, raw[begin], false);
    if (step.status != BopomofoKeyStatus::Composing || !initial.has_initial() ||
        initial.has_medial() || initial.has_final()) return {};
    const size_t original_begin = begin;
    std::u16string cache_key(1, static_cast<char16_t>(kLayoutCount + layout_index(layout)));
    cache_key.push_back(raw[begin]);
    if (const auto cached = impl_->cached_query(cache_key)) {
        auto result = *cached;
        for (auto& entry : result) {
            entry.begin += original_begin;
            entry.end += original_begin;
        }
        return result;
    }
    const auto index = impl_->index(layout);
    std::vector<RecoveredSyllable> result;
    for (const auto& entry : index->entries) {
        if (entry.body.size() < 2 || entry.body.front() != raw[begin] ||
            entry.reading.empty() || !is_bopomofo_initial(entry.reading.front())) continue;
        result.push_back({0, 1, entry.body, entry.tone, entry.reading,
                          *entry.candidates, 0.0, true, false, true});
    }
    std::ranges::sort(result, [](const auto& first, const auto& second) {
        if (first.reading != second.reading) return first.reading < second.reading;
        if (first.body_keys.size() != second.body_keys.size()) return first.body_keys.size() < second.body_keys.size();
        return first.body_keys < second.body_keys;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& first, const auto& second) {
                     return first.reading == second.reading;
                 }), result.end());
    if (result.size() > kMaxSuggestionsPerPosition) result.resize(kMaxSuggestionsPerPosition);
    impl_->remember_query(cache_key, result);
    for (auto& entry : result) {
        entry.begin += original_begin;
        entry.end += original_begin;
    }
    return result;
}

}  // namespace llavon::ime

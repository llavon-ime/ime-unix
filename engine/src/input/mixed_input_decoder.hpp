#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bopomofo/keymap.hpp"

namespace llavon::ime {

enum class MixedSegmentKind {
    Latin,
    Bopomofo,
    BopomofoIncomplete,
    // A bounded malformed continuation of a real syllable. It displays exact
    // raw keys and has no guessed reading/candidates; it cannot start a path.
    BopomofoUnresolved,
    Number,
    Symbol,
};

struct MixedSegment {
    MixedSegmentKind kind = MixedSegmentKind::Symbol;
    size_t begin = 0;
    size_t end = 0;
    std::u16string raw;
    std::u16string body_keys;
    char32_t tone_key = 0;
    std::u16string reading;
    std::vector<char32_t> candidates;
    double score = 0;
    bool consumed_boundary = false;
};

struct MixedPath {
    std::vector<MixedSegment> segments;
    std::u16string rendered;
    double score = 0;
    // Explicit boundary repairs are offered in the candidate panel, never used
    // to override an automatic opaque-token interpretation.
    bool boundary_alternative = false;
    // An additional model-refined copy beside its unchanged dictionary repair.
    bool boundary_model_refined = false;
};

struct MixedDecodeResult {
    std::u16string raw;
    std::vector<MixedPath> paths;
    // Path zero is the reversible raw alternative, not a ranking override.
    size_t best_path = 0;
};

// One displayed candidate entry: which path it came from, which character
// candidate of the final Bopomofo segment was chosen, and the complete text.
struct MixedCandidateEntry {
    size_t path_index = 0;
    size_t char_index = 0;
    std::u16string text;
};

// Decodes the complete pending input into complete output paths. The decoder
// has no fcitx runtime dependency: reading lookup is supplied by a callback;
// scoring defaults to the embedded offline lexicon and accepts overrides.
//
// Invariants every returned path satisfies:
//   - every path covers the raw input exactly once, without gaps or rewrites;
//   - the exact raw ASCII path is always the first path;
//   - no two paths render the same output text;
//   - paths are ordered by descending score after the raw path.
class MixedInputDecoder {
public:
    using LookupFn = std::function<std::vector<char32_t>(std::u16string_view)>;
    using FrequencyFn = std::function<double(std::u16string_view)>;
    using LatinScoreFn = std::function<double(std::u16string_view, bool)>;
    using ChineseScoreFn = std::function<double(std::u16string_view, char32_t)>;

    MixedInputDecoder(LookupFn lookup, FrequencyFn frequency,
                      LatinScoreFn latin_score = {}, ChineseScoreFn chinese_score = {});

    MixedDecodeResult decode(std::u16string_view raw, BopomofoKeyboardLayout layout, bool space_tone,
                             std::u16string_view context = {}) const;

    // Best/preferred path first, then raw, an explicit boundary repair when
    // available, and the remaining ranked alternatives.
    // Earlier-character choices and alternative boundaries are lattice paths;
    // remaining rows are filled with final-character homophones.
    std::vector<MixedCandidateEntry> expand_candidates(
        const MixedDecodeResult& result,
        size_t page_size,
        std::optional<size_t> preferred_path = std::nullopt) const;

    static constexpr size_t kTopK = 16;
    static constexpr size_t kMaxSyllableKeys = 6;

private:
    LookupFn lookup_;
    FrequencyFn frequency_;
    LatinScoreFn latin_score_;
    ChineseScoreFn chinese_score_;
};

}  // namespace llavon::ime

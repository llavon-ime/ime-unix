#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bopomofo/keymap.hpp"

namespace llavon::ime {

// A validated single-syllable proposal. Offsets describe the original input;
// body_keys and tone_key are the corrected, replayable physical spelling.
// The caller decides whether lexical/context evidence permits the proposal.
struct RecoveredSyllable {
    size_t begin = 0;
    size_t end = 0;
    std::u16string body_keys;
    char32_t tone_key = 0;
    std::u16string reading;
    std::vector<char32_t> candidates;
    double edit_cost = 0.0;
    bool missing_tone = false;
    bool edited = false;
    bool abbreviated = false;
};

// Layout-specific, table-validated tries are built lazily and shared by copies
// of this object. Search permits one physical edit and/or an omitted tone;
// it never manufactures a reading by overwriting an occupied phonetic slot.
class PhoneticRecovery {
public:
    using LookupFn = std::function<std::vector<char32_t>(std::u16string_view)>;

    explicit PhoneticRecovery(LookupFn lookup);

    std::vector<RecoveredSyllable> recover_at(std::u16string_view raw, size_t begin,
                                             BopomofoKeyboardLayout layout) const;
    std::vector<RecoveredSyllable> recover(std::u16string_view raw,
                                          BopomofoKeyboardLayout layout) const;
    // Candidate-only initial-key shorthand. Each proposal consumes one raw
    // key and supplies a complete, table-supported syllable. The caller must
    // match a whole lexical phrase and keep these proposals non-automatic.
    std::vector<RecoveredSyllable> recover_initial_at(std::u16string_view raw, size_t begin,
                                                     BopomofoKeyboardLayout layout) const;

    static constexpr size_t kMaxBodyKeys = 6;
    static constexpr size_t kMaxRawSpan = kMaxBodyKeys + 2;
    static constexpr size_t kMaxSuggestionsPerPosition = 256;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace llavon::ime

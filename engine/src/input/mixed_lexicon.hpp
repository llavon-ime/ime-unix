#pragma once

#include <string_view>
#include <span>

namespace llavon::ime {

// Immutable, process-shared lexical priors. Token IDs are never language data.
class MixedLexicon {
public:
    static const MixedLexicon& instance();
    double english_frequency(std::u16string_view word) const;
    double english_score(std::u16string_view word, bool allow_prefix) const;
    double chinese_score(std::u16string_view history, char32_t character) const;
    // Existing 2-4 character Chinese words, indexed by prefix. Used to match
    // whole lexical units against reading edges without a homophone product.
    std::span<const char32_t> chinese_phrase_extensions(std::u16string_view prefix) const;
    bool is_chinese_phrase(std::u16string_view word) const;

private:
    MixedLexicon() = default;
};

}  // namespace llavon::ime

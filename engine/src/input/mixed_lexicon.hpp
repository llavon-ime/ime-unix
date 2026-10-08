#pragma once

#include <string_view>
#include <span>

namespace llavon::ime {

// Immutable, process-shared lexical priors. Token IDs are never language data.
class MixedLexicon {
public:
    static const MixedLexicon& instance();
    double english_frequency(std::u16string_view word) const;
    // Strongest dictionary completion of an unfinished spelling, on the same
    // normalized frequency scale as a complete word. No text is completed here.
    double english_prefix_frequency(std::u16string_view prefix) const;
    double english_score(std::u16string_view word, bool allow_prefix) const;
    double chinese_score(std::u16string_view history, char32_t character) const;
    // Existing 2-4 character Chinese words, indexed by prefix. Used to match
    // whole lexical units against reading edges without a homophone product.
    std::span<const char32_t> chinese_phrase_extensions(std::u16string_view prefix) const;
    bool is_chinese_phrase(std::u16string_view word) const;
    // Phrase strength on the same normalized scale as English word frequency.
    double chinese_phrase_frequency(std::u16string_view word) const;
    // Two-character words with Zipf >= 4, indexed once for bounded onset checks.
    std::span<const char32_t> common_chinese_pair_extensions(char32_t first) const;

private:
    MixedLexicon() = default;
};

}  // namespace llavon::ime

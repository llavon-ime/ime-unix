#include "input/mixed_lexicon.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "text/utf.hpp"

namespace llavon::ime {
namespace {

constexpr std::string_view kData[] = {
#include "../../data/mixed_lexicon.inc"
};

std::u16string lowercase(std::u16string_view word) {
    std::u16string result(word);
    for (auto& ch : result) {
        if (ch >= u'A' && ch <= u'Z') ch = static_cast<char16_t>(ch + (u'a' - u'A'));
    }
    return result;
}

struct Data {
    struct PhrasePrefix {
        std::vector<char32_t> next;
        bool word = false;
    };
    std::unordered_map<std::u16string, PhrasePrefix> chinese_phrases;
    std::unordered_map<std::u16string, double> words;
    std::unordered_map<std::u16string, double> prefixes;
    std::array<std::array<double, 27>, 27> pairs{};
    std::array<double, 27> totals{};

    Data() {
        for (const auto chunk : kData) {
            size_t begin = 0;
            while (begin < chunk.size()) {
                const size_t tab = chunk.find('\t', begin);
                const size_t end = chunk.find('\n', tab);
                int frequency = 0;
                (void)std::from_chars(chunk.data() + tab + 1, chunk.data() + end, frequency);
                auto word = utf8_to_u16(chunk.substr(begin, tab - begin));
                const double zipf = static_cast<double>(frequency) / 100.0;
                words.emplace(word, zipf);
                if (word.size() >= 2 && word.size() <= 4 && std::ranges::all_of(word, [](char16_t ch) {
                    return ch >= u'\u3400' && ch <= u'\u9fff';
                })) {
                    chinese_phrases[word].word = true;
                    for (size_t length = 1; length < word.size(); ++length) {
                        auto& next = chinese_phrases[word.substr(0, length)].next;
                        const auto ch = static_cast<char32_t>(word[length]);
                        if (std::ranges::find(next, ch) == next.end()) next.push_back(ch);
                    }
                }
                if (!word.empty() && word.front() >= u'a' && word.front() <= u'z') {
                    for (size_t length = 1; length < word.size(); ++length) {
                        auto& value = prefixes[word.substr(0, length)];
                        value = std::max(value, zipf);
                    }
                    size_t previous = 26;
                    // Sublinear frequency weighting prevents common function
                    // words from overwhelming the spelling model.
                    const double weight = std::pow(10.0, (zipf - 3.0) * 0.25);
                    for (const auto ch : word) {
                        if (ch < u'a' || ch > u'z') continue;
                        const size_t next = static_cast<size_t>(ch - u'a');
                        pairs[previous][next] += weight;
                        totals[previous] += weight;
                        previous = next;
                    }
                    pairs[previous][26] += weight;
                    totals[previous] += weight;
                }
                begin = end + 1;
            }
        }
    }
};

const Data& data() {
    static const Data value;
    return value;
}

}  // namespace

const MixedLexicon& MixedLexicon::instance() {
    static const MixedLexicon value;
    (void)data();
    return value;
}

double MixedLexicon::english_frequency(std::u16string_view word) const {
    const auto it = data().words.find(lowercase(word));
    return it == data().words.end() ? 0.0 : std::clamp(it->second / 8.0, 0.0, 1.0);
}

double MixedLexicon::english_score(std::u16string_view input, bool allow_prefix) const {
    const auto word = lowercase(input);
    const auto& model = data();
    const auto known = model.words.find(word);
    double score = known == model.words.end() ? -1000.0 : std::min(-0.1, known->second * 0.75 - 5.0);
    if (allow_prefix) {
        const auto prefix = model.prefixes.find(word);
        if (prefix != model.prefixes.end()) score = std::max(score, prefix->second * 0.75 - 8.0);
    }
    // Smoothed character bigram likelihood preserves plausible unknown names
    // and technical terms, without declaring every four-letter input English.
    double spelling = -3.0;
    size_t previous = 26;
    for (const auto ch : word) {
        if (ch < u'a' || ch > u'z') {
            spelling -= 0.5;
            previous = 26;
            continue;
        }
        const size_t next = static_cast<size_t>(ch - u'a');
        spelling += 0.38 * std::log((model.pairs[previous][next] + 1.0) /
                                    (model.totals[previous] + 27.0));
        previous = next;
    }
    return std::max(score, spelling);
}

double MixedLexicon::chinese_score(std::u16string_view history, char32_t character) const {
    if (character > 0xFFFF) return -6.5;
    const auto& model = data();
    std::u16string word(1, static_cast<char16_t>(character));
    const auto unigram = model.words.find(word);
    double score = unigram == model.words.end() ? -6.5 : -7.5 + 0.4 * unigram->second;
    // A bounded phrase prior also uses preceding confirmed/client text. This
    // is deliberately a backoff approximation, not a conditional neural LM.
    double phrase_bonus = 0;
    for (size_t length = 1; length <= 3 && length <= history.size(); ++length) {
        word.assign(history.substr(history.size() - length));
        word.push_back(static_cast<char16_t>(character));
        const auto phrase = model.words.find(word);
        if (phrase != model.words.end()) phrase_bonus = std::max(phrase_bonus, (phrase->second - 1.0) * 0.7);
    }
    return score + phrase_bonus;
}

std::span<const char32_t> MixedLexicon::chinese_phrase_extensions(std::u16string_view prefix) const {
    const auto found = data().chinese_phrases.find(std::u16string(prefix));
    if (found == data().chinese_phrases.end()) return {};
    return found->second.next;
}

bool MixedLexicon::is_chinese_phrase(std::u16string_view word) const {
    const auto found = data().chinese_phrases.find(std::u16string(word));
    return found != data().chinese_phrases.end() && found->second.word;
}

}  // namespace llavon::ime

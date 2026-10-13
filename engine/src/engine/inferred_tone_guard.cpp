#include "engine/inferred_tone_guard.hpp"

#include <algorithm>

#include "engine/fallback_engine.hpp"
#include "input/mixed_lexicon.hpp"
#include "text/utf.hpp"

namespace llavon::ime {
namespace {

bool has_tone_alias(const FallbackEngine& fallback, std::u16string_view reading, char32_t character) {
    if (reading.empty() || character == 0) return false;
    constexpr std::u16string_view tones = u" ˊˇˋ˙";
    auto body = std::u16string(reading);
    if (tones.find(body.back()) != std::u16string_view::npos) body.pop_back();
    unsigned aliases = 0;
    for (const auto tone : tones) {
        const auto candidates = fallback.lookup(body + tone);
        if (std::ranges::find(candidates, character) != candidates.end() && ++aliases > 1) return true;
    }
    return false;
}

}  // namespace

std::vector<bool> protect_inferred_tone_words(std::span<const InferredToneSlot> slots,
                                             const FallbackEngine& fallback) {
    std::vector<bool> preserve(slots.size(), false);
    if (std::ranges::none_of(slots, [](const auto& slot) { return slot.eligible && slot.phonetic; })) return preserve;
    const auto& lexicon = MixedLexicon::instance();
    // A source-word window may end inside a different, valid model word.
    // Give each changed character the model's complete lexical support before
    // considering a shorter source window (我要請 -> 我邀請, for example).
    std::vector<bool> model_supported(slots.size(), false);
    for (std::size_t begin = 0; begin < slots.size(); ++begin) {
        std::u16string word;
        for (std::size_t end = begin; end < slots.size() && end < begin + 4; ++end) {
            const auto& slot = slots[end];
            if (!slot.phonetic || slot.reading.empty() || slot.original == 0 || slot.proposed == 0) break;
            word += utf8_to_u16(char32_to_utf8(slot.proposed));
            if (end != begin && lexicon.is_chinese_phrase(word)) {
                std::fill(model_supported.begin() + static_cast<std::ptrdiff_t>(begin),
                    model_supported.begin() + static_cast<std::ptrdiff_t>(end + 1), true);
            }
        }
    }
    for (std::size_t begin = 0; begin < slots.size(); ++begin) {
        std::u16string source_word;
        std::u16string model_word;
        for (std::size_t end = begin; end < slots.size() && end < begin + 4; ++end) {
            const auto& slot = slots[end];
            if (!slot.phonetic || slot.reading.empty() || slot.original == 0 || slot.proposed == 0) break;
            source_word += utf8_to_u16(char32_to_utf8(slot.original));
            model_word += utf8_to_u16(char32_to_utf8(slot.proposed));
            if (end == begin || !lexicon.is_chinese_phrase(source_word) ||
                lexicon.is_chinese_phrase(model_word)) continue;
            for (std::size_t i = begin; i <= end; ++i) {
                const auto& current = slots[i];
                if (current.eligible && !model_supported[i] && current.proposed != current.original &&
                    has_tone_alias(fallback, current.reading, current.original)) preserve[i] = true;
            }
        }
    }
    return preserve;
}

}  // namespace llavon::ime

#pragma once

#include <span>
#include <string>
#include <vector>

namespace llavon::ime {

class FallbackEngine;

struct InferredToneSlot {
    std::u16string reading;
    char32_t original = 0;
    char32_t proposed = 0;
    bool eligible = false;
    // Literals, unfinished readings and unsupported model inputs delimit words.
    bool phonetic = false;
};

// A guessed tone can use a legal alias whose masked model output breaks an
// existing dictionary word. Preserve only that ambiguous-tone source character;
// callers retain the model's other homophones as explicit choices.
std::vector<bool> protect_inferred_tone_words(std::span<const InferredToneSlot> slots,
                                             const FallbackEngine& fallback);

}  // namespace llavon::ime

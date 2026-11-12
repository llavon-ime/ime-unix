#pragma once

#include <optional>
#include <string_view>

#include "bopomofo/keymap.hpp"
#include "input/input_key.hpp"

namespace llavon::ime {

enum class PunctuationStyle {
    // libchewing's table.
    Chewing,
    // macOS Zhuyin: \ types 、, ' and " type quotes that alternate between
    // opening and closing, _ types ＿ and ^ types ＾.
    Apple,
};

// Maps Config::punctuation_style; unknown values use the Chewing table.
PunctuationStyle punctuation_style_from_config(std::string_view value) noexcept;

// The full-width mark an ASCII punctuation key types under a style. Quote keys
// under the Apple style return the opening quote (‘ or “); QuoteAlternation
// decides which side is actually typed.
std::optional<char32_t> punctuation_for_ascii(char32_t key, PunctuationStyle style);

// Resolves the punctuation character a key produces under a layout. Returns
// nullopt when the key is not punctuation, is blocked by a modifier, or the
// layout leaves it to the application.
std::optional<char32_t> chewing_punctuation_for_key(const InputKey& key, BopomofoKeyboardLayout layout,
                                                    PunctuationStyle style = PunctuationStyle::Chewing);

// macOS Zhuyin types ‘ and ’ (and “ and ”) in turn. Tracks, per input
// context, which side the next quote of each kind is.
class QuoteAlternation {
public:
    // Returns the quote to type for an opening quote returned by
    // punctuation_for_ascii, and records it; other marks pass unchanged.
    char32_t take(char32_t mark) noexcept;
    // Records a quote that was typed without take() (decoder output).
    void observe(char32_t mark) noexcept;
    bool single_closes() const noexcept {
        return single_closes_;
    }
    bool double_closes() const noexcept {
        return double_closes_;
    }

private:
    bool single_closes_ = false;
    bool double_closes_ = false;
};

}  // namespace llavon::ime

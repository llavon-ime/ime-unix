#include "input/punctuation.hpp"

namespace llavon::ime {

namespace {

// macOS Zhuyin's marks where they differ from libchewing's (read from the
// system Zhuyin keyboard layouts and checked on a Mac).
std::optional<char32_t> apple_punctuation(char32_t key) {
    switch (key) {
        case U'\\': return U'、';
        case U'\'': return U'‘';
        case U'"': return U'“';
        case U'_': return U'＿';
        case U'^': return U'＾';
        default: return std::nullopt;
    }
}

}  // namespace

PunctuationStyle punctuation_style_from_config(std::string_view value) noexcept {
    return value == "apple" ? PunctuationStyle::Apple : PunctuationStyle::Chewing;
}

std::optional<char32_t> punctuation_for_ascii(char32_t key, PunctuationStyle style) {
    if (style == PunctuationStyle::Apple) {
        if (const auto mark = apple_punctuation(key)) return mark;
    }
    return lookup_chewing_punctuation_key(key);
}

std::optional<char32_t> chewing_punctuation_for_key(const InputKey& key, BopomofoKeyboardLayout layout,
                                                    PunctuationStyle style) {
    if (key.has(InputKeyState::Alt) || key.has(InputKeyState::Super) || key.has(InputKeyState::Meta)) {
        return std::nullopt;
    }
    const char32_t raw_symbol = key.sym;
    const char32_t shifted_symbol = shifted_ascii_symbol(raw_symbol);
    const bool shifted = key.shifted();
    const char32_t symbol = shifted ? shifted_symbol : raw_symbol;

    if (key.has(InputKeyState::Ctrl)) {
        if (const auto punctuation = lookup_microsoft_ctrl_punctuation_key(symbol)) return punctuation;
        return lookup_microsoft_ctrl_punctuation_key(raw_symbol);
    }

    // Compact letter-only layouts leave unshifted punctuation halfwidth.
    if (is_compact_bopomofo_layout(layout) && !shifted) {
        if (punctuation_for_ascii(raw_symbol, style)) return raw_symbol;
        return std::nullopt;
    }

    return punctuation_for_ascii(symbol, style);
}

char32_t QuoteAlternation::take(char32_t mark) noexcept {
    if (mark == U'‘' && single_closes_) mark = U'’';
    if (mark == U'“' && double_closes_) mark = U'”';
    observe(mark);
    return mark;
}

void QuoteAlternation::observe(char32_t mark) noexcept {
    switch (mark) {
        case U'‘': single_closes_ = true; break;
        case U'’': single_closes_ = false; break;
        case U'“': double_closes_ = true; break;
        case U'”': double_closes_ = false; break;
        default: break;
    }
}

}  // namespace llavon::ime

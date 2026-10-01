#include "input/punctuation.hpp"

namespace llavon::ime {

std::optional<char32_t> chewing_punctuation_for_key(const InputKey& key, BopomofoKeyboardLayout layout) {
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
        if (lookup_chewing_punctuation_key(raw_symbol)) return raw_symbol;
        return std::nullopt;
    }

    return lookup_chewing_punctuation_key(symbol);
}

}  // namespace llavon::ime

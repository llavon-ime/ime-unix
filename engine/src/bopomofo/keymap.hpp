#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bopomofo/syllable.hpp"

namespace llavon::ime {

enum class BopomofoKeyboardLayout {
    Standard,
    Hsu,
    Ibm,
    Et,
    GinYieh,
    Et26,
    DachenCp26,
};

enum class BopomofoKeyStatus {
    Rejected,
    Composing,
    Completed,
};

struct BopomofoKeyResult {
    BopomofoKeyStatus status = BopomofoKeyStatus::Rejected;
    std::vector<std::u16string> alternative_readings;
    bool natural_extension = false;
};

std::optional<char32_t> lookup_bopomofo_key(char32_t key, bool accept_uppercase = true);
// Direct layouts have a context-independent map; compact layouts use the
// syllable editor below because a single key can have several meanings.
std::optional<char32_t> lookup_bopomofo_key(char32_t key, BopomofoKeyboardLayout layout,
                                         bool accept_uppercase = true);
BopomofoKeyboardLayout bopomofo_keyboard_layout(std::string_view name);
std::string_view bopomofo_keyboard_layout_name(BopomofoKeyboardLayout layout);
bool is_compact_bopomofo_layout(BopomofoKeyboardLayout layout);
bool is_bopomofo_tone_key(char32_t key, BopomofoKeyboardLayout layout);
// Strict, bounded single-syllable interpretations. Compact layouts retain the
// ordinary reading and may fill a vacant initial after a medial/final; no
// existing component is replaced to manufacture a reading.
std::vector<Syllable> replay_bopomofo_keys(std::u16string_view body, char32_t tone_key,
                                         BopomofoKeyboardLayout layout);
std::optional<char32_t> lookup_chewing_punctuation_key(char32_t key);
std::optional<char32_t> lookup_microsoft_ctrl_punctuation_key(char32_t key);

BopomofoKeyResult apply_bopomofo_key(
    Syllable& syllable,
    BopomofoKeyboardLayout layout,
    char32_t key,
    bool accept_uppercase = true);

}  // namespace llavon::ime

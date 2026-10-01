#include "bopomofo/keymap.hpp"

#include <algorithm>
#include <unordered_map>

namespace llavon::ime {

namespace {

char32_t normalize_ascii_letter(char32_t key) {
    if (key >= U'A' && key <= U'Z') return key + (U'a' - U'A');
    return key;
}

bool is_hsu_end_key(char32_t key, const Syllable& syllable) {
    if (syllable.empty()) return false;
    return key == U'd' || key == U'f' || key == U'j' || key == U's' || key == U' ';
}

// Hsu alternative readings, mirroring libchewing's Hsu::ALT_TABLE. They only
// apply to first-tone readings, which end with a trailing ASCII space.
const std::vector<std::u16string>& hsu_alternative_readings(const Syllable& syllable) {
    static const std::unordered_map<std::u16string, std::vector<std::u16string>> table{
        {u"ㄘ ", {u"ㄟ "}},   {u"ㄧ ", {u"ㄝ "}},   {u"ㄙ ", {u"˙"}},   {u"ㄉ ", {u"ˊ"}},
        {u"ㄈ ", {u"ˇ"}},     {u"ㄜ ", {u"ㄍ "}},   {u"ㄛ ", {u"ㄏ "}}, {u"ㄓ ", {u"ㄐ ", u"ˋ"}},
        {u"ㄤ ", {u"ㄎ "}},   {u"ㄦ ", {u"ㄌ ", u"ㄥ "}}, {u"ㄕ ", {u"ㄒ "}},
        {u"ㄔ ", {u"ㄑ "}},   {u"ㄣ ", {u"ㄋ "}},   {u"ㄢ ", {u"ㄇ "}},
    };
    static const std::vector<std::u16string> empty;
    const auto it = table.find(syllable.text());
    if (it == table.end()) return empty;
    return it->second;
}

// Ordered Hsu conversion rules from libchewing's src/editor/zhuyin_layout/hsu.rs.
BopomofoKeyResult apply_hsu_key(Syllable& syllable, char32_t key) {
    if (is_hsu_end_key(key, syllable)) {
        // Step 1: normalize a singleton initial into its final counterpart.
        if (!syllable.has_medial() && !syllable.has_final()) {
            switch (syllable.initial()) {
                case U'ㄐ':
                    (void)syllable.overwrite(U'ㄓ');
                    break;
                case U'ㄑ':
                    (void)syllable.overwrite(U'ㄔ');
                    break;
                case U'ㄒ':
                    (void)syllable.overwrite(U'ㄕ');
                    break;
                case U'ㄏ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄛ');
                    break;
                case U'ㄍ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄜ');
                    break;
                case U'ㄇ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄢ');
                    break;
                case U'ㄋ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄣ');
                    break;
                case U'ㄎ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄤ');
                    break;
                case U'ㄌ':
                    (void)syllable.remove_initial();
                    (void)syllable.overwrite(U'ㄦ');
                    break;
                default:
                    break;
            }
        }

        // Step 2: apply the delayed ㄍㄧ/ㄍㄩ to ㄐㄧ/ㄐㄩ conversion.
        if (syllable.initial() == U'ㄍ' && (syllable.medial() == U'ㄧ' || syllable.medial() == U'ㄩ')) {
            (void)syllable.overwrite(U'ㄐ');
        }

        // Step 3: apply the tone.
        switch (key) {
            case U'd':
                (void)syllable.overwrite(U'ˊ');
                break;
            case U'f':
                (void)syllable.overwrite(U'ˇ');
                break;
            case U'j':
                (void)syllable.overwrite(U'ˋ');
                break;
            case U's':
                (void)syllable.overwrite(U'˙');
                break;
            default:
                (void)syllable.overwrite(U' ');
                break;
        }

        BopomofoKeyResult result;
        if (syllable.complete()) {
            result.status = BopomofoKeyStatus::Completed;
            result.alternative_readings = hsu_alternative_readings(syllable);
        } else {
            result.status = BopomofoKeyStatus::Composing;
        }
        return result;
    }

    // Map the physical key using the syllable state before this key.
    char32_t symbol = 0;
    switch (key) {
        case U'a':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄟ' : U'ㄘ';
            break;
        case U'b':
            symbol = U'ㄅ';
            break;
        case U'c':
            symbol = U'ㄕ';
            break;
        case U'd':
            symbol = U'ㄉ';
            break;
        case U'e':
            symbol = syllable.has_medial() ? U'ㄝ' : U'ㄧ';
            break;
        case U'f':
            symbol = U'ㄈ';
            break;
        case U'g':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄜ' : U'ㄍ';
            break;
        case U'h':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄛ' : U'ㄏ';
            break;
        case U'i':
            symbol = U'ㄞ';
            break;
        case U'j':
            symbol = U'ㄓ';
            break;
        case U'k':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄤ' : U'ㄎ';
            break;
        case U'l':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄥ' : U'ㄌ';
            break;
        case U'm':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄢ' : U'ㄇ';
            break;
        case U'n':
            symbol = syllable.has_initial() || syllable.has_medial() ? U'ㄣ' : U'ㄋ';
            break;
        case U'o':
            symbol = U'ㄡ';
            break;
        case U'p':
            symbol = U'ㄆ';
            break;
        case U'r':
            symbol = U'ㄖ';
            break;
        case U's':
            symbol = U'ㄙ';
            break;
        case U't':
            symbol = U'ㄊ';
            break;
        case U'u':
            symbol = U'ㄩ';
            break;
        case U'v':
            symbol = U'ㄔ';
            break;
        case U'w':
            symbol = U'ㄠ';
            break;
        case U'x':
            symbol = U'ㄨ';
            break;
        case U'y':
            symbol = U'ㄚ';
            break;
        case U'z':
            symbol = U'ㄗ';
            break;
        default:
            return {};
    }

    // Convert existing ㄍㄧ or ㄍㄩ to ㄐㄧ or ㄐㄩ before inserting the new symbol.
    if (syllable.initial() == U'ㄍ' && (syllable.medial() == U'ㄧ' || syllable.medial() == U'ㄩ')) {
        (void)syllable.overwrite(U'ㄐ');
    }

    // ㄐ/ㄑ/ㄒ must be followed by ㄧ or ㄩ; convert them to ㄓ/ㄔ/ㄕ when the
    // incoming symbol is ㄨ or a final without a medial.
    if (symbol == U'ㄨ' || (is_bopomofo_final(symbol) && !syllable.has_medial())) {
        switch (syllable.initial()) {
            case U'ㄐ':
                (void)syllable.overwrite(U'ㄓ');
                break;
            case U'ㄑ':
                (void)syllable.overwrite(U'ㄔ');
                break;
            case U'ㄒ':
                (void)syllable.overwrite(U'ㄕ');
                break;
            default:
                break;
        }
    }

    // Similarly, when ㄓ/ㄔ/ㄕ is followed by ㄧ or ㄩ, convert them to ㄐ/ㄑ/ㄒ.
    if (symbol == U'ㄧ' || symbol == U'ㄩ') {
        switch (syllable.initial()) {
            case U'ㄓ':
                (void)syllable.overwrite(U'ㄐ');
                break;
            case U'ㄔ':
                (void)syllable.overwrite(U'ㄑ');
                break;
            case U'ㄕ':
                (void)syllable.overwrite(U'ㄒ');
                break;
            default:
                break;
        }
    }

    (void)syllable.overwrite(symbol);
    BopomofoKeyResult result;
    if (syllable.complete()) {
        result.status = BopomofoKeyStatus::Completed;
        result.alternative_readings = hsu_alternative_readings(syllable);
    } else {
        result.status = BopomofoKeyStatus::Composing;
    }
    return result;
}

// ET26 and CP26 use the conventional compact-key state transitions documented
// by libchewing (snapshot 3c4a93aa03d574c7f011ff84e8a2437c2f79b2cf).
// This editor uses our Syllable and completion/rollback contract.
BopomofoKeyResult compact_result(const Syllable& syllable) {
    return {syllable.complete() ? BopomofoKeyStatus::Completed : BopomofoKeyStatus::Composing, {}};
}

BopomofoKeyResult apply_et26_key(Syllable& syllable, char32_t key) {
    if (!syllable.empty() && (key == U' ' || is_bopomofo_tone_key(key, BopomofoKeyboardLayout::Et26))) {
        if (!syllable.has_medial() && !syllable.has_final()) {
            char32_t final = 0;
            switch (syllable.initial()) {
                case U'ㄐ': (void)syllable.overwrite(U'ㄓ'); break;
                case U'ㄒ': (void)syllable.overwrite(U'ㄕ'); break;
                case U'ㄆ': final = U'ㄡ'; break;
                case U'ㄇ': final = U'ㄢ'; break;
                case U'ㄋ': final = U'ㄣ'; break;
                case U'ㄊ': final = U'ㄤ'; break;
                case U'ㄌ': final = U'ㄥ'; break;
                case U'ㄏ': final = U'ㄦ'; break;
                default: break;
            }
            if (final) { (void)syllable.remove_initial(); (void)syllable.overwrite(final); }
        }
        const auto tone = key == U'f' ? U'ˊ' : key == U'j' ? U'ˇ' : key == U'k' ? U'ˋ' : key == U'd' ? U'˙' : U' ';
        (void)syllable.overwrite(tone);
        auto result = compact_result(syllable);
        // First-tone singletons retain the alternate interpretation instead of
        // silently choosing one. Tone-only alternatives cannot become a word.
        if (key == U' ' && result.status == BopomofoKeyStatus::Completed) {
            static const std::unordered_map<std::u16string, std::u16string> alternatives{
                {u"ㄡ ", u"ㄆ "}, {u"ㄤ ", u"ㄊ "}, {u"ㄘ ", u"ㄝ "}, {u"ㄗ ", u"ㄟ "},
                {u"ㄓ ", u"ㄐ "}, {u"ㄦ ", u"ㄏ "}, {u"ㄥ ", u"ㄌ "}, {u"ㄕ ", u"ㄒ "},
                {u"ㄍ ", u"ㄑ "}, {u"ㄣ ", u"ㄋ "}, {u"ㄢ ", u"ㄇ "},
                {u"ㄉ ", u"˙"}, {u"ㄈ ", u"ˊ"}, {u"ㄖ ", u"ˇ"}, {u"ㄎ ", u"ˋ"},
            };
            if (const auto it = alternatives.find(syllable.text()); it != alternatives.end()) {
                result.alternative_readings.push_back(it->second);
            }
        }
        return result;
    }
    const bool has_prefix = syllable.has_initial() || syllable.has_medial();
    char32_t symbol = 0;
    switch (key) {
        case U'a': symbol = U'ㄚ'; break;
        case U'b': symbol = U'ㄅ'; break;
        case U'c': symbol = U'ㄒ'; break;
        case U'd': symbol = U'ㄉ'; break;
        case U'e': symbol = U'ㄧ'; break;
        case U'f': symbol = U'ㄈ'; break;
        case U'g': symbol = U'ㄐ'; break;
        case U'h': symbol = has_prefix ? U'ㄦ' : U'ㄏ'; break;
        case U'i': symbol = U'ㄞ'; break;
        case U'j': symbol = U'ㄖ'; break;
        case U'k': symbol = U'ㄎ'; break;
        case U'l': symbol = has_prefix ? U'ㄥ' : U'ㄌ'; break;
        case U'm': symbol = has_prefix ? U'ㄢ' : U'ㄇ'; break;
        case U'n': symbol = has_prefix ? U'ㄣ' : U'ㄋ'; break;
        case U'o': symbol = U'ㄛ'; break;
        case U'p': symbol = has_prefix ? U'ㄡ' : U'ㄆ'; break;
        case U'q': symbol = has_prefix ? U'ㄟ' : U'ㄗ'; break;
        case U'r': symbol = U'ㄜ'; break;
        case U's': symbol = U'ㄙ'; break;
        case U't': symbol = has_prefix ? U'ㄤ' : U'ㄊ'; break;
        case U'u': symbol = U'ㄩ'; break;
        case U'v': symbol = U'ㄍ'; break;
        case U'w': symbol = has_prefix ? U'ㄝ' : U'ㄘ'; break;
        case U'x': symbol = U'ㄨ'; break;
        case U'y': symbol = U'ㄔ'; break;
        case U'z': symbol = U'ㄠ'; break;
        default: return {};
    }
    if (symbol == U'ㄨ' || (is_bopomofo_final(symbol) && !syllable.has_medial())) {
        if (syllable.initial() == U'ㄐ') (void)syllable.overwrite(U'ㄓ');
        if (syllable.initial() == U'ㄒ') (void)syllable.overwrite(U'ㄕ');
    } else if (is_bopomofo_medial(symbol) && syllable.initial() == U'ㄍ') {
        (void)syllable.overwrite(U'ㄑ');
    }
    (void)syllable.overwrite(symbol);
    return compact_result(syllable);
}

BopomofoKeyResult apply_cp26_key(Syllable& syllable, char32_t key) {
    if (!syllable.empty() && (key == U' ' || is_bopomofo_tone_key(key, BopomofoKeyboardLayout::DachenCp26))) {
        (void)syllable.overwrite(key == U'e' ? U'ˊ' : key == U'r' ? U'ˇ' : key == U'd' ? U'ˋ' : key == U'y' ? U'˙' : U' ');
        return compact_result(syllable);
    }
    const bool has_prefix = syllable.has_initial() || syllable.has_medial();
    const auto toggle = [](char32_t current, char32_t first, char32_t second) { return current == first ? second : first; };
    char32_t symbol = 0;
    switch (key) {
        case U'q': symbol = toggle(syllable.initial(), U'ㄅ', U'ㄆ'); break;
        case U'a': symbol = U'ㄇ'; break;
        case U'z': symbol = U'ㄈ'; break;
        case U'w': symbol = toggle(syllable.initial(), U'ㄉ', U'ㄊ'); break;
        case U's': symbol = U'ㄋ'; break;
        case U'x': symbol = U'ㄌ'; break;
        case U'e': symbol = U'ㄍ'; break;
        case U'd': symbol = U'ㄎ'; break;
        case U'c': symbol = U'ㄏ'; break;
        case U'r': symbol = U'ㄐ'; break;
        case U'f': symbol = U'ㄑ'; break;
        case U'v': symbol = U'ㄒ'; break;
        case U't': symbol = toggle(syllable.initial(), U'ㄓ', U'ㄔ'); break;
        case U'g': symbol = U'ㄕ'; break;
        case U'b': symbol = has_prefix ? U'ㄝ' : U'ㄖ'; break;
        case U'y': symbol = U'ㄗ'; break;
        case U'h': symbol = U'ㄘ'; break;
        case U'n': symbol = has_prefix ? U'ㄥ' : U'ㄙ'; break;
        case U'u':
            // ㄧ -> ㄚ -> ㄧㄚ -> empty, with existing initials preserved.
            if (syllable.final() == U'ㄚ') {
                if (syllable.medial() == U'ㄧ') {
                    (void)syllable.remove_medial(); (void)syllable.remove_final();
                } else { (void)syllable.overwrite(U'ㄧ'); }
            } else if (syllable.has_medial()) {
                if (syllable.medial() == U'ㄧ') (void)syllable.remove_medial();
                (void)syllable.overwrite(U'ㄚ');
            } else { (void)syllable.overwrite(U'ㄧ'); }
            return compact_result(syllable);
        case U'j': symbol = U'ㄨ'; break;
        case U'm':
            if (syllable.medial() == U'ㄩ' && syllable.final() != U'ㄡ') {
                (void)syllable.remove_medial(); (void)syllable.overwrite(U'ㄡ');
            } else if (syllable.final() == U'ㄡ' && syllable.medial() != U'ㄩ') {
                (void)syllable.overwrite(U'ㄩ'); (void)syllable.remove_final();
            } else if (syllable.has_medial()) { (void)syllable.overwrite(U'ㄡ'); }
            else { (void)syllable.overwrite(U'ㄩ'); }
            return compact_result(syllable);
        case U'i': symbol = toggle(syllable.final(), U'ㄛ', U'ㄞ'); break;
        case U'k': symbol = U'ㄜ'; break;
        case U'o': symbol = toggle(syllable.final(), U'ㄟ', U'ㄢ'); break;
        case U'l': symbol = toggle(syllable.final(), U'ㄠ', U'ㄤ'); break;
        case U'p': symbol = toggle(syllable.final(), U'ㄣ', U'ㄦ'); break;
        default: return {};
    }
    (void)syllable.overwrite(symbol);
    return compact_result(syllable);
}

bool natural_extension(const Syllable& before, const Syllable& after, char32_t key,
                       BopomofoKeyboardLayout layout) {
    if (!is_compact_bopomofo_layout(layout)) {
        auto natural = before;
        const auto symbol = lookup_bopomofo_key(key, layout, false);
        return symbol && natural.accept(*symbol);
    }
    if (after.text().size() > before.text().size()) return true;
    if (layout != BopomofoKeyboardLayout::DachenCp26 || before.has_tone()) return false;
    // CP26's intentional cycles, shared by the buffer and lattice replay.
    const bool initial_cycle = !before.has_medial() && !before.has_final() &&
        ((key == U'q' && (before.initial() == U'ㄅ' || before.initial() == U'ㄆ')) ||
         (key == U'w' && (before.initial() == U'ㄉ' || before.initial() == U'ㄊ')) ||
         (key == U't' && (before.initial() == U'ㄓ' || before.initial() == U'ㄔ')));
    const bool medial_cycle =
        (key == U'u' && ((before.medial() == U'ㄧ' && !before.has_final()) ||
                        (!before.has_medial() && before.final() == U'ㄚ'))) ||
        (key == U'm' && ((before.medial() == U'ㄩ' && !before.has_final()) ||
                        (!before.has_medial() && before.final() == U'ㄡ')));
    const bool final_cycle =
        (key == U'i' && (before.final() == U'ㄛ' || before.final() == U'ㄞ')) ||
        (key == U'o' && (before.final() == U'ㄟ' || before.final() == U'ㄢ')) ||
        (key == U'l' && (before.final() == U'ㄠ' || before.final() == U'ㄤ')) ||
        (key == U'p' && (before.final() == U'ㄣ' || before.final() == U'ㄦ'));
    return initial_cycle || medial_cycle || final_cycle;
}

}  // namespace

BopomofoKeyboardLayout bopomofo_keyboard_layout(std::string_view name) {
    if (name == "hsu") return BopomofoKeyboardLayout::Hsu;
    if (name == "ibm") return BopomofoKeyboardLayout::Ibm;
    if (name == "et") return BopomofoKeyboardLayout::Et;
    if (name == "ginyieh") return BopomofoKeyboardLayout::GinYieh;
    if (name == "et26") return BopomofoKeyboardLayout::Et26;
    if (name == "dachen_cp26") return BopomofoKeyboardLayout::DachenCp26;
    return BopomofoKeyboardLayout::Standard;
}

std::string_view bopomofo_keyboard_layout_name(BopomofoKeyboardLayout layout) {
    switch (layout) {
        case BopomofoKeyboardLayout::Standard: return "standard";
        case BopomofoKeyboardLayout::Hsu: return "hsu";
        case BopomofoKeyboardLayout::Ibm: return "ibm";
        case BopomofoKeyboardLayout::Et: return "et";
        case BopomofoKeyboardLayout::GinYieh: return "ginyieh";
        case BopomofoKeyboardLayout::Et26: return "et26";
        case BopomofoKeyboardLayout::DachenCp26: return "dachen_cp26";
    }
    return "standard";
}

bool is_compact_bopomofo_layout(BopomofoKeyboardLayout layout) {
    return layout == BopomofoKeyboardLayout::Hsu || layout == BopomofoKeyboardLayout::Et26 ||
           layout == BopomofoKeyboardLayout::DachenCp26;
}

bool is_bopomofo_tone_key(char32_t key, BopomofoKeyboardLayout layout) {
    switch (layout) {
        case BopomofoKeyboardLayout::Hsu: return key == U'd' || key == U'f' || key == U'j' || key == U's';
        case BopomofoKeyboardLayout::Et26: return key == U'f' || key == U'j' || key == U'k' || key == U'd';
        case BopomofoKeyboardLayout::DachenCp26: return key == U'e' || key == U'r' || key == U'd' || key == U'y';
        case BopomofoKeyboardLayout::Standard:
        case BopomofoKeyboardLayout::Ibm:
        case BopomofoKeyboardLayout::Et:
        case BopomofoKeyboardLayout::GinYieh: {
            const auto symbol = lookup_bopomofo_key(key, layout);
            return symbol && is_bopomofo_tone(*symbol) && *symbol != U' ';
        }
    }
    return false;
}

std::optional<char32_t> lookup_bopomofo_key(char32_t key, BopomofoKeyboardLayout layout, bool accept_uppercase) {
    if (layout == BopomofoKeyboardLayout::Standard) return lookup_bopomofo_key(key, accept_uppercase);
    if (is_compact_bopomofo_layout(layout)) return std::nullopt;
    if (accept_uppercase) key = normalize_ascii_letter(key);
    // Symbol order is ㄅ..ㄙ, ㄧㄨㄩ, ㄚ..ㄦ, then the five tones.
    constexpr std::u32string_view symbols = U"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ ˊˇˋ˙";
    std::u32string_view keys;
    switch (layout) {
        case BopomofoKeyboardLayout::Ibm: keys = U"1234567890-qwertyuiopasdfghjkl;zxcvbn m,./"; break;
        case BopomofoKeyboardLayout::Et: keys = U"bpmfdtnlvkhg7c,./j;'sexuaorwiqzy890-= 2341"; break;
        case BopomofoKeyboardLayout::GinYieh: keys = U"2wsx3edcrfvtgb6yhnujm-['8ik,9ol.0p;/= qaz1"; break;
        case BopomofoKeyboardLayout::Standard:
        case BopomofoKeyboardLayout::Hsu:
        case BopomofoKeyboardLayout::Et26:
        case BopomofoKeyboardLayout::DachenCp26: return std::nullopt;
    }
    const auto index = keys.find(key);
    if (index == std::u32string_view::npos) return std::nullopt;
    return symbols[index];
}

std::optional<char32_t> lookup_bopomofo_key(char32_t key, bool accept_uppercase) {
    if (accept_uppercase) key = normalize_ascii_letter(key);
    static const std::unordered_map<char32_t, char32_t> map{
        {U'1', U'ㄅ'}, {U'2', U'ㄉ'}, {U'5', U'ㄓ'}, {U'8', U'ㄚ'}, {U'9', U'ㄞ'}, {U'0', U'ㄢ'}, {U'-', U'ㄦ'},
        {U'q', U'ㄆ'}, {U'w', U'ㄊ'}, {U'e', U'ㄍ'}, {U'r', U'ㄐ'}, {U't', U'ㄔ'}, {U'y', U'ㄗ'}, {U'u', U'ㄧ'}, {U'i', U'ㄛ'}, {U'o', U'ㄟ'}, {U'p', U'ㄣ'},
        {U'a', U'ㄇ'}, {U's', U'ㄋ'}, {U'd', U'ㄎ'}, {U'f', U'ㄑ'}, {U'g', U'ㄕ'}, {U'h', U'ㄘ'}, {U'j', U'ㄨ'}, {U'k', U'ㄜ'}, {U'l', U'ㄠ'}, {U';', U'ㄤ'},
        {U'z', U'ㄈ'}, {U'x', U'ㄌ'}, {U'c', U'ㄏ'}, {U'v', U'ㄒ'}, {U'b', U'ㄖ'}, {U'n', U'ㄙ'}, {U'm', U'ㄩ'}, {U',', U'ㄝ'}, {U'.', U'ㄡ'}, {U'/', U'ㄥ'},
        {U' ', U' '}, {U'6', U'ˊ'}, {U'3', U'ˇ'}, {U'4', U'ˋ'}, {U'7', U'˙'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

std::optional<char32_t> lookup_chewing_punctuation_key(char32_t key) {
    static const std::unordered_map<char32_t, char32_t> map{
        {U'[', U'「'}, {U']', U'」'}, {U'{', U'『'}, {U'}', U'』'}, {U'\'', U'、'}, {U'<', U'，'},
        {U':', U'：'}, {U'"', U'；'}, {U'>', U'。'}, {U'~', U'～'}, {U'!', U'！'}, {U'@', U'＠'},
        {U'#', U'＃'}, {U'$', U'＄'}, {U'%', U'％'}, {U'^', U'︿'}, {U'&', U'＆'}, {U'*', U'＊'},
        {U'(', U'（'}, {U')', U'）'}, {U'_', U'—'}, {U'+', U'＋'}, {U'=', U'＝'}, {U'\\', U'＼'},
        {U'|', U'｜'}, {U'?', U'？'}, {U',', U'，'}, {U'.', U'。'}, {U';', U'；'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

std::optional<char32_t> lookup_microsoft_ctrl_punctuation_key(char32_t key) {
    static const std::unordered_map<char32_t, char32_t> map{
        {U'!', U'！'}, {U'\'', U'、'}, {U',', U'，'}, {U'.', U'。'}, {U'/', U'？'}, {U';', U'；'},
    };

    const auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

BopomofoKeyResult apply_bopomofo_key(Syllable& syllable, BopomofoKeyboardLayout layout, char32_t key,
                                     bool accept_uppercase) {
    if (accept_uppercase) {
        key = normalize_ascii_letter(key);
    } else if (key >= U'A' && key <= U'Z') {
        return {};
    }

    Syllable candidate = syllable;
    BopomofoKeyResult result;
    if (layout == BopomofoKeyboardLayout::Hsu) {
        result = apply_hsu_key(candidate, key);
    } else if (layout == BopomofoKeyboardLayout::Et26) {
        result = apply_et26_key(candidate, key);
    } else if (layout == BopomofoKeyboardLayout::DachenCp26) {
        result = apply_cp26_key(candidate, key);
    } else {
        const auto symbol = lookup_bopomofo_key(key, layout, accept_uppercase);
        if (!symbol) return {};
        if (!(candidate.accept(*symbol) || candidate.overwrite(*symbol))) return {};
        result.status = is_bopomofo_tone(*symbol) && candidate.complete()
                            ? BopomofoKeyStatus::Completed
                            : BopomofoKeyStatus::Composing;
    }

    if (result.status == BopomofoKeyStatus::Rejected) return result;
    result.natural_extension = natural_extension(syllable, candidate, key, layout);
    syllable = std::move(candidate);
    return result;
}

std::vector<Syllable> replay_bopomofo_keys(std::u16string_view body, char32_t tone_key,
                                         BopomofoKeyboardLayout layout) {
    if (body.empty()) return {};
    std::vector<Syllable> states(1);
    const auto insert = [](auto& into, const Syllable& syllable) {
        if (std::ranges::none_of(into, [&](const auto& existing) { return existing.text() == syllable.text(); })) {
            into.push_back(syllable);
        }
    };
    for (const auto key : body) {
        std::vector<Syllable> next;
        for (const auto& before : states) {
            auto candidate = before;
            const auto result = apply_bopomofo_key(candidate, layout, key);
            if (result.status == BopomofoKeyStatus::Composing && result.natural_extension && !candidate.has_tone()) {
                insert(next, candidate);
            }
            if (is_compact_bopomofo_layout(layout) && !before.has_initial() &&
                (before.has_medial() || before.has_final()) && !is_bopomofo_tone_key(key, layout)) {
                // The key's ordinary standalone initial is an additional
                // interpretation, not a replacement for its contextual final
                // meaning. Tone keys keep their completion semantics; a
                // confirmed syllable is not reopened across that boundary.
                Syllable standalone;
                (void)apply_bopomofo_key(standalone, layout, key);
                if (standalone.has_initial() && !standalone.has_medial() && !standalone.has_final()) {
                    auto reordered = before;
                    if (reordered.accept(standalone.initial())) insert(next, reordered);
                }
            }
        }
        states = std::move(next);
        if (states.empty()) return {};
    }
    std::vector<Syllable> complete;
    for (auto syllable : states) {
        const auto result = apply_bopomofo_key(syllable, layout, tone_key);
        if (result.status == BopomofoKeyStatus::Completed) insert(complete, syllable);
    }
    return complete;
}

}  // namespace llavon::ime

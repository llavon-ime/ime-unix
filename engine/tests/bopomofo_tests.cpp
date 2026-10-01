#include "test_suites.h"
#include "bopomofo/keymap.hpp"
#include "bopomofo/syllable.hpp"
#include "bopomofo/table_engine.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using llavon::ime::apply_bopomofo_key;
using llavon::ime::BopomofoKeyboardLayout;
using llavon::ime::BopomofoKeyStatus;

// Types a sequence of physical Hsu keys into a syllable and returns the final
// syllable text. Returns std::nullopt if any key is rejected.
std::optional<std::u16string> type_hsu(const std::u32string& keys, bool accept_uppercase = true) {
    llavon::ime::Syllable syllable;
    for (const char32_t key : keys) {
        const auto result = apply_bopomofo_key(syllable, BopomofoKeyboardLayout::Hsu, key, accept_uppercase);
        if (result.status == BopomofoKeyStatus::Rejected) return std::nullopt;
    }
    return syllable.text();
}

std::optional<std::u16string> type_hsu_standard(const std::u32string& keys, bool accept_uppercase = true) {
    llavon::ime::Syllable syllable;
    for (const char32_t key : keys) {
        const auto result = apply_bopomofo_key(syllable, BopomofoKeyboardLayout::Standard, key, accept_uppercase);
        if (result.status == BopomofoKeyStatus::Rejected) return std::nullopt;
    }
    return syllable.text();
}

}  // namespace

int run_bopomofo_tests() {
    bool ok = true;
    ok = ok && llavon::ime::lookup_bopomofo_key(U'1') == U'ㄅ';
    ok = ok && llavon::ime::lookup_bopomofo_key(U'4') == U'ˋ';
    ok = ok && llavon::ime::lookup_bopomofo_key(U'q') == U'ㄆ';
    ok = ok && llavon::ime::lookup_bopomofo_key(U'Q') == U'ㄆ';
    ok = ok && llavon::ime::lookup_bopomofo_key(U'S') == U'ㄋ';
    ok = ok && !llavon::ime::lookup_bopomofo_key(U'Q', false).has_value();
    ok = ok && llavon::ime::lookup_bopomofo_key(U' ') == U' ';
    ok = ok && !llavon::ime::lookup_bopomofo_key(U'@').has_value();
    const std::vector<std::pair<char32_t, char32_t>> chewing_punctuation{
        {U'[', U'「'}, {U']', U'」'}, {U'{', U'『'}, {U'}', U'』'}, {U'\'', U'、'}, {U'<', U'，'},
        {U':', U'：'}, {U'"', U'；'}, {U'>', U'。'}, {U'~', U'～'}, {U'!', U'！'}, {U'@', U'＠'},
        {U'#', U'＃'}, {U'$', U'＄'}, {U'%', U'％'}, {U'^', U'︿'}, {U'&', U'＆'}, {U'*', U'＊'},
        {U'(', U'（'}, {U')', U'）'}, {U'_', U'—'}, {U'+', U'＋'}, {U'=', U'＝'}, {U'\\', U'＼'},
        {U'|', U'｜'}, {U'?', U'？'}, {U',', U'，'}, {U'.', U'。'}, {U';', U'；'},
    };
    for (const auto& [key, symbol] : chewing_punctuation) {
        ok = ok && llavon::ime::lookup_chewing_punctuation_key(key) == symbol;
    }
    ok = ok && !llavon::ime::lookup_chewing_punctuation_key(U'`').has_value();
    ok = ok && llavon::ime::lookup_microsoft_ctrl_punctuation_key(U'!') == U'！';
    ok = ok && llavon::ime::lookup_microsoft_ctrl_punctuation_key(U'.') == U'。';

    llavon::ime::Syllable syllable;
    ok = ok && syllable.accept(U'ㄋ');
    ok = ok && syllable.accept(U'ㄧ');
    ok = ok && syllable.accept(U'ˇ');
    ok = ok && syllable.complete();
    ok = ok && syllable.text() == std::u16string(u"ㄋㄧˇ");

    llavon::ime::Syllable tone_only;
    ok = ok && tone_only.accept(U'ˋ');
    ok = ok && !tone_only.complete();
    ok = ok && tone_only.text() == std::u16string(u"ˋ");

    // Syllable structural accessors.
    llavon::ime::Syllable structured;
    ok = ok && !structured.has_initial() && !structured.has_medial() && !structured.has_final() && !structured.has_tone();
    ok = ok && structured.initial() == 0 && structured.medial() == 0 && structured.final() == 0 && structured.tone() == 0;
    ok = ok && structured.accept(U'ㄐ') && structured.accept(U'ㄧ') && structured.accept(U'ㄝ') &&
         structured.accept(U'ˋ');
    ok = ok && structured.initial() == U'ㄐ' && structured.medial() == U'ㄧ' && structured.final() == U'ㄝ' &&
         structured.tone() == U'ˋ';
    ok = ok && structured.has_initial() && structured.has_medial() && structured.has_final() && structured.has_tone();
    ok = ok && structured.remove_tone() && !structured.has_tone() && !structured.complete();
    ok = ok && structured.remove_initial() && !structured.has_initial();
    ok = ok && !structured.remove_initial() && !structured.remove_tone();

    // Fixed Hsu keys.
    ok = ok && type_hsu(U"b") == std::u16string(u"ㄅ");
    ok = ok && type_hsu(U"p") == std::u16string(u"ㄆ");
    ok = ok && type_hsu(U"r") == std::u16string(u"ㄖ");
    ok = ok && type_hsu(U"t") == std::u16string(u"ㄊ");
    ok = ok && type_hsu(U"y") == std::u16string(u"ㄚ");
    ok = ok && type_hsu(U"u") == std::u16string(u"ㄩ");
    ok = ok && type_hsu(U"i") == std::u16string(u"ㄞ");
    ok = ok && type_hsu(U"o") == std::u16string(u"ㄡ");
    ok = ok && type_hsu(U"w") == std::u16string(u"ㄠ");
    ok = ok && type_hsu(U"x") == std::u16string(u"ㄨ");
    ok = ok && type_hsu(U"z") == std::u16string(u"ㄗ");
    ok = ok && !type_hsu(U"q").has_value();

    // Contextual a, e, g, h, k, l, m, n keys.
    ok = ok && type_hsu(U"a") == std::u16string(u"ㄘ");
    ok = ok && type_hsu(U"ba") == std::u16string(u"ㄅㄟ");
    ok = ok && type_hsu(U"e") == std::u16string(u"ㄧ");
    ok = ok && type_hsu(U"be") == std::u16string(u"ㄅㄧ");
    ok = ok && type_hsu(U"bee") == std::u16string(u"ㄅㄧㄝ");
    ok = ok && type_hsu(U"g") == std::u16string(u"ㄍ");
    ok = ok && type_hsu(U"bg") == std::u16string(u"ㄅㄜ");
    ok = ok && type_hsu(U"h") == std::u16string(u"ㄏ");
    ok = ok && type_hsu(U"bh") == std::u16string(u"ㄅㄛ");
    ok = ok && type_hsu(U"k") == std::u16string(u"ㄎ");
    ok = ok && type_hsu(U"bk") == std::u16string(u"ㄅㄤ");
    ok = ok && type_hsu(U"l") == std::u16string(u"ㄌ");
    ok = ok && type_hsu(U"bl") == std::u16string(u"ㄅㄥ");
    ok = ok && type_hsu(U"m") == std::u16string(u"ㄇ");
    ok = ok && type_hsu(U"bm") == std::u16string(u"ㄅㄢ");
    ok = ok && type_hsu(U"n") == std::u16string(u"ㄋ");
    ok = ok && type_hsu(U"bn") == std::u16string(u"ㄅㄣ");

    // d, f, j, s as start keys on an empty syllable.
    ok = ok && type_hsu(U"d") == std::u16string(u"ㄉ");
    ok = ok && type_hsu(U"f") == std::u16string(u"ㄈ");
    ok = ok && type_hsu(U"j") == std::u16string(u"ㄓ");
    ok = ok && type_hsu(U"s") == std::u16string(u"ㄙ");

    // Required Hsu reading cases.
    ok = ok && type_hsu(U"cen ") == std::u16string(u"ㄒㄧㄣ ");
    ok = ok && type_hsu(U"kxj") == std::u16string(u"ㄎㄨˋ");
    ok = ok && type_hsu(U"en ") == std::u16string(u"ㄧㄣ ");
    ok = ok && type_hsu(U"m ") == std::u16string(u"ㄢ ");
    ok = ok && type_hsu(U"hd") == std::u16string(u"ㄛˊ");
    ok = ok && type_hsu(U"g ") == std::u16string(u"ㄜ ");
    ok = ok && type_hsu(U"nf") == std::u16string(u"ㄣˇ");
    ok = ok && type_hsu(U"k ") == std::u16string(u"ㄤ ");
    ok = ok && type_hsu(U"lf") == std::u16string(u"ㄦˇ");
    ok = ok && type_hsu(U"ge ") == std::u16string(u"ㄐㄧ ");
    ok = ok && type_hsu(U"gen ") == std::u16string(u"ㄐㄧㄣ ");
    ok = ok && type_hsu(U"geej") == std::u16string(u"ㄐㄧㄝˋ");
    ok = ok && type_hsu(U"gu ") == std::u16string(u"ㄐㄩ ");
    ok = ok && type_hsu(U"gued") == std::u16string(u"ㄐㄩㄝˊ");
    ok = ok && type_hsu(U"j ") == std::u16string(u"ㄓ ");
    ok = ok && type_hsu(U"v ") == std::u16string(u"ㄔ ");
    ok = ok && type_hsu(U"c ") == std::u16string(u"ㄕ ");
    ok = ok && type_hsu(U"cek") == std::u16string(u"ㄒㄧㄤ");
    ok = ok && type_hsu(U"cke") == std::u16string(u"ㄒㄧㄤ");

    // Delayed ㄍㄧ/ㄍㄩ conversion: intermediate state stays ㄍㄧ until the next key.
    ok = ok && type_hsu(U"ge") == std::u16string(u"ㄍㄧ");
    ok = ok && type_hsu(U"gu") == std::u16string(u"ㄍㄩ");
    ok = ok && type_hsu(U"gen") == std::u16string(u"ㄐㄧㄣ");

    // ㄐ/ㄑ/ㄒ and ㄓ/ㄔ/ㄕ conversion.
    ok = ok && type_hsu(U"ce") == std::u16string(u"ㄒㄧ");
    ok = ok && type_hsu(U"ve") == std::u16string(u"ㄑㄧ");
    ok = ok && type_hsu(U"je") == std::u16string(u"ㄐㄧ");
    ok = ok && type_hsu(U"cx") == std::u16string(u"ㄕㄨ");
    ok = ok && type_hsu(U"ck") == std::u16string(u"ㄕㄤ");
    ok = ok && type_hsu(U"jk") == std::u16string(u"ㄓㄤ");
    ok = ok && type_hsu(U"bk") == std::u16string(u"ㄅㄤ");
    {
        // A final without a medial converts a ㄐ/ㄑ/ㄒ initial to ㄓ/ㄔ/ㄕ.
        llavon::ime::Syllable s;
        s.accept(U'ㄐ');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'k');
        ok = ok && result.status == BopomofoKeyStatus::Composing;
        ok = ok && s.text() == std::u16string(u"ㄓㄤ");
    }

    // Singleton normalization before tone application, including initials that
    // can only be produced by direct semantic construction.
    ok = ok && type_hsu(U"m ") == std::u16string(u"ㄢ ");
    {
        llavon::ime::Syllable s;
        s.accept(U'ㄐ');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'j');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && s.text() == std::u16string(u"ㄓˋ");
    }
    {
        llavon::ime::Syllable s;
        s.accept(U'ㄑ');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'f');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && s.text() == std::u16string(u"ㄔˇ");
    }
    {
        llavon::ime::Syllable s;
        s.accept(U'ㄒ');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'd');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && s.text() == std::u16string(u"ㄕˊ");
    }

    // Uppercase: enabled normalizes before Hsu interpretation, disabled rejects.
    ok = ok && type_hsu(U"CEN ") == std::u16string(u"ㄒㄧㄣ ");
    ok = ok && type_hsu(U"HD") == std::u16string(u"ㄛˊ");
    ok = ok && type_hsu(U"C", true) == std::u16string(u"ㄕ");
    ok = ok && !type_hsu(U"C", false).has_value();
    {
        llavon::ime::Syllable s;
        s.accept(U'ㄏ');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'D', false);
        ok = ok && result.status == BopomofoKeyStatus::Rejected;
        ok = ok && s.text() == std::u16string(u"ㄏ");
    }

    // Standard layout still accepts uppercase and behaves unchanged through
    // apply_bopomofo_key.
    ok = ok && type_hsu_standard(U"su3") == std::u16string(u"ㄋㄧˇ");
    ok = ok && type_hsu_standard(U"SU3") == std::u16string(u"ㄋㄧˇ");
    ok = ok && !type_hsu_standard(U"Q", false).has_value();
    {
        llavon::ime::Syllable s;
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Standard, U'4');
        ok = ok && result.status == BopomofoKeyStatus::Composing;
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Standard, U'u');
        ok = ok && result.status == BopomofoKeyStatus::Composing;
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Standard, U'4');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && s.text() == std::u16string(u"ㄧˋ");
    }

    // Exact alternative-reading lists for first-tone completions.
    {
        llavon::ime::Syllable s;
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'a');
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄟ "});
    }
    {
        llavon::ime::Syllable s;
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'e');
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄝ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U's');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"˙"});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'd');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ˊ"});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'f');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ˇ"});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'g');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄍ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'h');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄏ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'j');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄐ ", u"ˋ"});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'k');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄎ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'l');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄌ ", u"ㄥ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'c');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄒ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'v');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄑ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'n');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄋ "});
    }
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'm');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U' ');
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄇ "});
    }
    {
        // A non-first-tone completion has no alternative readings.
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'a');
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'd');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && result.alternative_readings.empty();
    }
    {
        // An unfinished syllable exposes no alternative readings.
        llavon::ime::Syllable s;
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'a');
        ok = ok && result.status == BopomofoKeyStatus::Composing;
        ok = ok && result.alternative_readings.empty();
    }
    {
        // A first tone inherited across a layout change still receives Hsu
        // alternatives when an ordinary key completes the syllable.
        llavon::ime::Syllable s;
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Standard, U'4');
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Standard, U' ');
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'a');
        ok = ok && result.status == BopomofoKeyStatus::Completed;
        ok = ok && s.text() == std::u16string(u"ㄘ ");
        ok = ok && result.alternative_readings == std::vector<std::u16string>({u"ㄟ "});
    }

    // Rejected keys leave the syllable unchanged.
    {
        llavon::ime::Syllable s;
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'g');
        apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'e');
        const auto before = s.text();
        auto result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'q');
        ok = ok && result.status == BopomofoKeyStatus::Rejected;
        ok = ok && s.text() == before;
        result = apply_bopomofo_key(s, BopomofoKeyboardLayout::Hsu, U'@');
        ok = ok && result.status == BopomofoKeyStatus::Rejected;
        ok = ok && s.text() == before;
    }

    llavon::ime::TableEngine table(LLAVON_IME_TEST_TABLE_PATH);
    auto candidates = table.lookup(u"ㄋㄧˇ");
    ok = ok && !candidates.empty();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

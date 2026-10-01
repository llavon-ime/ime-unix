#include "raw_key_harness.hpp"
#include "bopomofo/keymap.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {
struct LayoutCase {
    std::string_view name;
    std::string_view label;
    std::string_view ni_hao;
    std::string_view xin;
};
// Physical sequences taken from the published layouts, independently of the
// engine's key lookup. Keep digit, punctuation and letter tone keys covered.
constexpr std::array layouts{
    LayoutCase{"standard", "標準", "su3cl3", "vup "},
    LayoutCase{"hsu", "許氏", "nefhwf", "cen "},
    LayoutCase{"ibm", "IBM", "7a,-;,", "eac "},
    LayoutCase{"et", "倚天", "ne3hz3", "ce9 "},
    LayoutCase{"ginyieh", "精業", "d-avla", "b-p "},
    LayoutCase{"et26", "倚天26鍵", "nejhzj", "cen "},
    LayoutCase{"dachen_cp26", "大千26鍵", "surclr", "vup "},
};

HarnessOptions options(std::string_view layout, bool smart = false) {
    HarnessOptions result;
    result.config.keyboard_layout = layout;
    result.config.smart_english = smart;
    return result;
}
}  // namespace

RAWKEY_SUITE("keyboard layouts conventional phrases", keyboard_phrases) {
    for (const auto& layout : layouts) {
        Harness harness(options(layout.name));
        harness.type(layout.ni_hao);
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.expect_commit("你好");
        harness.type(layout.xin);
        RAWKEY_ASSERT(harness.preedit() == "心");
        harness.key("space");
        RAWKEY_ASSERT(harness.has_candidates());
        harness.choose_text("心");
        harness.expect_commit("心");
    }
}

RAWKEY_SUITE("keyboard layouts direct key reference", keyboard_direct_reference) {
    // Reference data ordered by symbol, not obtained from lookup_bopomofo_key.
    constexpr std::u32string_view symbols = U"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ";
    constexpr std::array maps{
        std::pair{"ibm", std::string_view("1234567890-qwertyuiopasdfghjkl;zxcvbn")},
        std::pair{"et", std::string_view("bpmfdtnlvkhg7c,./j;'sexuaorwiqzy890-=")},
        std::pair{"ginyieh", std::string_view("2wsx3edcrfvtgb6yhnujm-['8ik,9ol.0p;/=")},
    };
    for (const auto& [name, keys] : maps) {
        RAWKEY_ASSERT(keys.size() == symbols.size());
        Harness harness(options(name));
        for (std::size_t i = 0; i < keys.size(); ++i) {
            harness.reset();
            harness.key(Key(keys[i]));
            RAWKEY_ASSERT(harness.preedit() == char32_to_utf8(symbols[i]));
        }
    }
}

RAWKEY_SUITE("keyboard layouts tones and editing", keyboard_tones) {
    constexpr std::array cases{
        std::pair{"ibm", std::string_view(" m,./")},
        std::pair{"et", std::string_view(" 2341")},
        std::pair{"ginyieh", std::string_view(" qaz1")},
        std::pair{"et26", std::string_view(" fjkd")},
        std::pair{"dachen_cp26", std::string_view(" erdy")},
    };
    constexpr std::array<std::u16string_view, 5> readings{u"ㄧ ", u"ㄧˊ", u"ㄧˇ", u"ㄧˋ", u"ㄧ˙"};
    for (const auto& [name, tones] : cases) {
        const char medial = std::string_view(name) == "ibm" ? 'a' : std::string_view(name) == "ginyieh" ? '-' :
                            std::string_view(name) == "dachen_cp26" ? 'u' : 'e';
        Harness harness(options(name));
        for (std::size_t i = 0; i < tones.size(); ++i) {
            harness.reset();
            harness.key(Key(medial));
            harness.key(Key(tones[i]));
            RAWKEY_ASSERT(harness.session()->buffer.segments().size() == 1);
            RAWKEY_ASSERT(harness.session()->buffer.segments().front().syllable.text() == readings[i]);
            RAWKEY_ASSERT(harness.session()->buffer.segments().front().reading_finalized);
        }
        harness.reset();
        harness.key(Key(medial));
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.composition_empty());
        harness.set_config("ShiftLetterKeys", "直接放入組字區");
        harness.type("API");
        RAWKEY_ASSERT(harness.preedit() == "api");
        harness.expect_commit("api");
    }
}

RAWKEY_SUITE("keyboard layouts compact conversions and cycles", keyboard_compact) {
    constexpr std::array et26{
        std::pair{"g ", u"ㄓ "}, std::pair{"c ", u"ㄕ "}, std::pair{"p ", u"ㄡ "},
        std::pair{"m ", u"ㄢ "}, std::pair{"n ", u"ㄣ "}, std::pair{"t ", u"ㄤ "},
        std::pair{"l ", u"ㄥ "}, std::pair{"h ", u"ㄦ "}, std::pair{"ve ", u"ㄑㄧ "},
        std::pair{"vx ", u"ㄍㄨ "}, std::pair{"gxn ", u"ㄓㄨㄣ "},
    };
    Harness et(options("et26"));
    for (const auto& [keys, reading] : et26) {
        et.reset(); et.type(keys);
        RAWKEY_ASSERT(et.session()->buffer.segments().front().syllable.text() == reading);
    }
    et.reset(); et.type("p ");
    RAWKEY_ASSERT(et.session()->buffer.segments().front().alternative_readings == std::vector<std::u16string>{u"ㄆ "});
    Harness cp(options("dachen_cp26"));
    constexpr std::array cycles{
        std::pair{"qq", "ㄆ"}, std::pair{"ww", "ㄊ"}, std::pair{"tt", "ㄔ"},
        std::pair{"ii", "ㄞ"}, std::pair{"oo", "ㄢ"}, std::pair{"ll", "ㄤ"}, std::pair{"pp", "ㄦ"},
        std::pair{"u", "ㄧ"}, std::pair{"uu", "ㄚ"}, std::pair{"uuu", "ㄧㄚ"},
        std::pair{"m", "ㄩ"}, std::pair{"mm", "ㄡ"}, std::pair{"mmm", "ㄩ"},
        std::pair{"ju", "ㄨㄚ"}, std::pair{"um", "ㄧㄡ"}, std::pair{"jm", "ㄨㄡ"},
    };
    for (const auto& [keys, reading] : cycles) {
        cp.reset(); cp.type(keys);
        RAWKEY_ASSERT(cp.preedit() == reading);
    }
}

RAWKEY_SUITE("keyboard layouts schema and punctuation", keyboard_settings) {
    for (const auto& layout : layouts) {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.set_config("BopomofoKeyboardLayout", layout.label);
        RAWKEY_ASSERT(harness.config().keyboard_layout == layout.name);
        const auto config = to_json(harness.config());
        RAWKEY_ASSERT(config_from_json(config).keyboard_layout == layout.name);
        harness.type(layout.ni_hao);
        harness.key("Control+,");
        RAWKEY_ASSERT(harness.preedit() == "你好，");
        harness.expect_focus_out_commit("你好，");
    }
}

RAWKEY_SUITE("keyboard layouts smart Chinese English and raw choices", keyboard_smart_choices) {
    for (const auto& layout : layouts) {
        const auto raw = std::string(layout.ni_hao) + "hello";
        Harness harness(options(layout.name, true));
        harness.type(raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        harness.key("Down");
        const auto entries = harness.candidates();
        RAWKEY_ASSERT(std::ranges::find(entries, raw) != entries.end());
        RAWKEY_ASSERT(std::ranges::find(entries, "你好hello") != entries.end());
        harness.choose_text("你好hello");
        harness.expect_commit("你好hello");
        harness.reset();
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.expect_commit(raw);
    }
}

RAWKEY_SUITE("keyboard layouts smart literals and reversible edits", keyboard_smart_edits) {
    constexpr std::array literals{"hello world", "user_name42", "dev@example.com", "https://example.org/docs?v=2", "_cache84"};
    for (const auto& layout : layouts) {
        Harness harness(options(layout.name, true));
        for (const auto literal : literals) {
            harness.reset();
            harness.type(literal);
            if (std::string_view(literal) == "hello world") RAWKEY_ASSERT(harness.preedit() == literal);
            // Structured spellings can share a valid Chinese suffix. Assert
            // reversibility instead of forcing both opposing intents to top-1.
            harness.key("Down");
            if (harness.has_candidates()) harness.choose_text(literal);
            else RAWKEY_ASSERT(harness.preedit() == literal);
            harness.expect_commit(literal);
        }
        harness.reset();
        harness.type(layout.ni_hao);
        const auto shown = harness.preedit();
        const auto raw = harness.session()->pending_token.raw;
        harness.type("x");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == shown);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == raw);
        harness.expect_focus_out_commit(shown);
        harness.reset();
        harness.type(layout.ni_hao);
        harness.key("Escape");
        RAWKEY_ASSERT(harness.preedit() == layout.ni_hao);
        harness.key("Escape");
        RAWKEY_ASSERT(harness.composition_empty());
    }
}

RAWKEY_SUITE("keyboard layouts pending snapshot and repeated smart keys", keyboard_smart_snapshot) {
    for (const auto& layout : layouts) {
        Harness harness(options(layout.name, true));
        harness.type(layout.ni_hao);
        harness.set_config("BopomofoKeyboardLayout", layout.name == "standard" ? "et26" : "standard");
        // Settings changes settle pending keys literally, just as the existing
        // Standard/Hsu contract does, rather than reinterpret them in a new map.
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        RAWKEY_ASSERT(harness.preedit() == layout.ni_hao);
        harness.expect_commit(layout.ni_hao);
    }
    Harness cp(options("dachen_cp26", true));
    cp.type("q");
    cp.type("q");
    RAWKEY_ASSERT(cp.session()->pending_token.raw == u"qq");
    cp.type("llr"); // ㄆㄤˇ: two independent physical-key cycles.
    cp.key("Down");
    const auto result = cp.session()->mixed_decision.result;
    RAWKEY_ASSERT(std::ranges::any_of(result.paths, [](const auto& path) {
        return path.segments.size() == 1 && path.segments.front().reading == u"ㄆㄤˇ";
    }));
}

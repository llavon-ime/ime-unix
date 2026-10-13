#include "raw_key_harness.hpp"
#include "bopomofo/keymap.hpp"
#include "input/mixed_lexicon.hpp"
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
    std::string_view today_use;
    std::string_view open_file;
};
// Physical sequences taken from the published layouts, independently of the
// engine's key lookup. Keep digit, punctuation and letter tone keys covered.
constexpr std::array layouts{
    LayoutCase{"standard", "標準", "su3cl3", "vup ", "rup wu0 m/4", "283d9 2;304"},
    LayoutCase{"hsu", "許氏", "nefhwf", "cen ", "jen tem ulj", "dyfki dkfmj"},
    LayoutCase{"ibm", "IBM", "7a,-;,", "eac ", "qac 6ax db.", "5f,0k 5v,x."},
    LayoutCase{"et", "倚天", "ne3hz3", "ce9 ", "ge9 te8 u-4", "da3ki d0384"},
    LayoutCase{"ginyieh", "精業", "d-avla", "b-p ", "t-p e-0 '/z", "38af9 3;a0z"},
    LayoutCase{"et26", "倚天26鍵", "nejhzj", "cen ", "gen tem ulk", "dajki dtjmk"},
    LayoutCase{"dachen_cp26", "大千26鍵", "surclr", "vup ", "rup wwuoo mnd", "wuurdii wllrood"},
};

HarnessOptions options(std::string_view layout, bool smart = false) {
    HarnessOptions result;
    result.config.keyboard_layout = layout;
    result.config.smart_english = smart;
    return result;
}
// Check language boundaries and readings without requiring the model to choose
// one specific homophone. Adjacent literal graph edges remain one exact word.
std::string mixed_signature(const Harness& harness) {
    const auto* session = harness.session();
    if (!session->mixed_decision.active() ||
        session->mixed_decision.preview_path >= session->mixed_decision.result.paths.size()) {
        return "<no mixed preview>";
    }
    const auto& path = session->mixed_decision.result.paths[session->mixed_decision.preview_path];
    std::string signature;
    std::u16string literal;
    const auto flush_literal = [&] {
        if (!literal.empty()) {
            signature += "[" + u16_to_utf8(literal) + "]";
            literal.clear();
        }
    };
    for (const auto& segment : path.segments) {
        if (segment.consumed_boundary) continue;
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            flush_literal();
            signature += "<" + u16_to_utf8(segment.reading) + ">";
        } else {
            literal += segment.raw;
        }
    }
    flush_literal();
    return signature;
}

void expect_mixed_signature(const Harness& harness, std::string_view expected,
                            std::string_view layout, std::string_view keys) {
    const auto actual = mixed_signature(harness);
    if (actual != expected) {
        throw Failure{std::string(layout) + " keys [" + std::string(keys) + "]: expected " +
                      std::string(expected) + ", got " + actual + " (preview " + harness.preedit() + ")"};
    }
}

void expect_pending_submit(Harness& harness, bool literal_tail) {
    const auto before = harness.preedit();
    RAWKEY_ASSERT(harness.commits().empty());
    RAWKEY_ASSERT(!harness.session()->pending_token.empty());
    harness.key("space");
    RAWKEY_ASSERT(harness.commits().empty());
    RAWKEY_ASSERT(!harness.composition_empty());
    RAWKEY_ASSERT(harness.preedit() == before + (literal_tail ? " " : ""));
    harness.expect_direct_commit(harness.preedit(), Key("Return"));
    RAWKEY_ASSERT(harness.commits().size() == 1);
    RAWKEY_ASSERT(harness.composition_empty());
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

RAWKEY_SUITE("keyboard layouts automatic known English boundaries", keyboard_smart_known_boundaries) {
    constexpr std::string_view ni_hao = "<ㄋㄧˇ><ㄏㄠˇ>";
    for (const auto& layout : layouts) {
        for (const std::string_view word : {"hello", "json"}) {
            for (const bool english_first : {false, true}) {
                const auto raw = english_first ? std::string(word) + std::string(layout.ni_hao)
                                               : std::string(layout.ni_hao) + std::string(word);
                Harness harness(options(layout.name, true));
                harness.type(raw);
                RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
                const auto literal = "[" + std::string(word) + "]";
                expect_mixed_signature(harness, english_first ? literal + std::string(ni_hao)
                                                             : std::string(ni_hao) + literal,
                                       layout.name, raw);
                expect_pending_submit(harness, !english_first);
            }
        }
    }
}

RAWKEY_SUITE("keyboard layouts repeated Chinese phrases stay entirely phonetic", keyboard_smart_repeated_chinese) {
    for (const auto& layout : layouts) {
        for (const int repetitions : {3, 4}) {
            std::string raw;
            std::string expected_text;
            std::string expected_signature;
            for (int index = 0; index < repetitions; ++index) {
                raw += layout.ni_hao;
                expected_text += "你好";
                expected_signature += "<ㄋㄧˇ><ㄏㄠˇ>";
            }
            Harness harness(options(layout.name, true));
            harness.type(raw);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
            expect_mixed_signature(harness, expected_signature, layout.name, raw);
            RAWKEY_ASSERT(harness.preedit() == expected_text);
            expect_pending_submit(harness, false);
        }
    }
}

RAWKEY_SUITE("keyboard layouts resume Chinese after unknown English islands", keyboard_smart_unknown_islands) {
    constexpr std::string_view ni_hao = "<ㄋㄧˇ><ㄏㄠˇ>";
    for (const auto& layout : layouts) {
        for (const std::string_view word : {"quux", "zorbix", "blorpt"}) {
            RAWKEY_ASSERT(MixedLexicon::instance().english_frequency(utf8_to_u16(word)) == 0);
            const auto raw = std::string(layout.ni_hao) + std::string(word) + std::string(layout.ni_hao);
            Harness harness(options(layout.name, true));
            harness.type(raw);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
            expect_mixed_signature(harness, std::string(ni_hao) + "[" + std::string(word) + "]" +
                                               std::string(ni_hao), layout.name, raw);
            expect_pending_submit(harness, false);

            Harness raw_choice(options(layout.name, true));
            raw_choice.type(raw);
            raw_choice.key("Down");
            RAWKEY_ASSERT(raw_choice.has_candidates());
            raw_choice.choose_text(raw);
            RAWKEY_ASSERT(raw_choice.preedit() == raw);
            raw_choice.expect_direct_commit(raw, Key("Return"));
            RAWKEY_ASSERT(raw_choice.composition_empty());
        }
        const auto raw = std::string(layout.ni_hao) + std::string(layout.ni_hao) +
            "quux" + std::string(layout.ni_hao);
        Harness repeated(options(layout.name, true));
        repeated.type(raw);
        expect_mixed_signature(repeated, std::string(ni_hao) + std::string(ni_hao) +
            "[quux]" + std::string(ni_hao), layout.name, raw);
        expect_pending_submit(repeated, false);
    }
}

RAWKEY_SUITE("keyboard layouts mixed complete phrases and spaces", keyboard_smart_complete_phrases) {
    constexpr std::string_view before = "<ㄐㄧㄣ ><ㄊㄧㄢ ><ㄩㄥˋ>";
    constexpr std::string_view after = "<ㄉㄚˇ><ㄎㄞ ><ㄉㄤˇ><ㄢˋ>";
    for (const auto& layout : layouts) {
        for (const std::string_view word : {"vim", "emacs", "json", "quux", "zorbix"}) {
            const auto raw = std::string(layout.today_use) + std::string(word) + std::string(layout.open_file);
            Harness harness(options(layout.name, true));
            harness.type(raw);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
            expect_mixed_signature(harness, std::string(before) + "[" + std::string(word) + "]" +
                                               std::string(after), layout.name, raw);
            expect_pending_submit(harness, false);
        }
    }
}

RAWKEY_SUITE("keyboard layouts preserve explicit spaces around Chinese English boundaries", keyboard_smart_explicit_spaces) {
    for (const auto& layout : layouts) {
        const auto raw = "python " + std::string(layout.ni_hao) + " json";
        Harness harness(options(layout.name, true));
        harness.type(raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        expect_mixed_signature(harness, "[python ]<ㄋㄧˇ><ㄏㄠˇ>[ json]", layout.name, raw);
        RAWKEY_ASSERT(harness.preedit() == "python 你好 json");
        expect_pending_submit(harness, true);
    }
}

RAWKEY_SUITE("Hsu technical English does not borrow neighboring Chinese keys", hsu_technical_boundaries) {
    constexpr std::array cases{
        std::pair{"gen tem uljjsonagjcj", "<ㄐㄧㄣ ><ㄊㄧㄢ ><ㄩㄥˋ>[json]<ㄘㄜˋ><ㄕˋ>"},
        std::pair{"pythonmif", "[python]<ㄇㄞˇ>"},
    };
    for (const auto& [raw, expected] : cases) {
        Harness harness(options("hsu", true));
        harness.type(raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        expect_mixed_signature(harness, expected, "hsu", raw);
        expect_pending_submit(harness, false);
    }
}

RAWKEY_SUITE("compact completed readings survive English compound scoring", compact_compound_readings) {
    constexpr std::array cases{
        std::array{"hsu", "beftext", "<ㄅㄧˇ>[text]"},
        std::array{"hsu", "ijmusic", "<ㄞˋ>[music]"},
        std::array{"hsu", "dedhello", "<ㄉㄧˊ>[hello]"},
        std::array{"dachen_cp26", "xuetext", "<ㄌㄧˊ>[text]"},
        std::array{"dachen_cp26", "clehello", "<ㄏㄠˊ>[hello]"},
        std::array{"dachen_cp26", "surclrsurhello", "<ㄋㄧˇ><ㄏㄠˇ><ㄋㄧˇ>[hello]"},
    };
    for (const auto& [layout, raw, expected] : cases) {
        Harness harness(options(layout, true));
        harness.type(raw);
        expect_mixed_signature(harness, expected, layout, raw);
        expect_pending_submit(harness, true);
    }
}

RAWKEY_SUITE("keyboard layouts preserve technical English words sentences and URLs", keyboard_smart_technical_english) {
    constexpr std::array literals{
        "grpc", "pytest", "json", "async", "clang", "kubernetes", "qwerty283",
        "please use grpc with pytest", "send json using async", "clang builds kubernetes",
        "https://example.org/api?q=json",
    };
    for (const auto& layout : layouts) {
        for (const auto literal : literals) {
            Harness harness(options(layout.name, true));
            harness.type(literal);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(literal));
            if (harness.preedit() != literal) {
                throw Failure{std::string(layout.name) + " literal [" + literal +
                              "] became [" + harness.preedit() + "]"};
            }
            RAWKEY_ASSERT(harness.commits().empty());
            harness.expect_direct_commit(literal, Key("Return"));
            RAWKEY_ASSERT(harness.commits().size() == 1);
            RAWKEY_ASSERT(harness.composition_empty());
        }
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

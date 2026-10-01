#include "raw_key_harness.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {
constexpr std::array layouts{
    std::pair{"standard", "su3cl3"}, std::pair{"hsu", "nefhwf"},
    std::pair{"ibm", "7a,-;,"}, std::pair{"et", "ne3hz3"},
    std::pair{"ginyieh", "d-avla"}, std::pair{"et26", "nejhzj"},
    std::pair{"dachen_cp26", "surclr"},
};
HarnessOptions smart(std::string_view layout) {
    HarnessOptions value;
    value.config.smart_english = true;
    value.config.keyboard_layout = layout;
    return value;
}
} // namespace

RAWKEY_SUITE("smart editing delete displayed Chinese", smart_delete_chinese) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        if (harness.preedit() != "你好") throw Failure{std::string(layout) + " expected 你好, got " + harness.preedit()};
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(std::string(keys).substr(0, 3)));
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.composition_empty());
        RAWKEY_ASSERT(harness.commits().empty());
        harness.type(keys);
        harness.key("Down");
        harness.choose_text("你好");
        harness.type("hello");
        for (int i = 0; i < 5; ++i) harness.key("BackSpace");
        if (harness.preedit() != "你好") throw Failure{std::string(layout) + " after hello deletion: " + harness.preedit()};
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
        // Once committed, deletion belongs to the client application. The IME
        // must not resurrect an old reading or consume its Backspace.
        RAWKEY_ASSERT(!harness.key_accepted("BackSpace"));
        RAWKEY_ASSERT(harness.composition_empty());
    }
}

RAWKEY_SUITE("smart editing raw undo and unfinished keys", smart_edit_reading) {
    Harness harness(smart("standard"));
    harness.type("su3cl3");
    harness.key("Shift+BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "你cl");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "你c");
    harness.type("l3");
    RAWKEY_ASSERT(harness.preedit() == "你好");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.type("cl3");
    RAWKEY_ASSERT(harness.preedit() == "你好");
    harness.key("space");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.expect_commit("你");
    harness.reset();
    harness.type("vup "); // first-tone Space is part of the displayed 心.
    RAWKEY_ASSERT(harness.preedit() == "心");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.composition_empty());
}

RAWKEY_SUITE("smart editing settled middle deletion and manual choices", smart_edit_middle) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        harness.key("Delete");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
        harness.reset();
        harness.type(keys);
        harness.key("Left");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "好");
        harness.expect_commit("好");
        harness.reset();
        harness.type(keys);
        harness.key("Down");
        harness.choose_text("你好");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
        harness.reset();
        harness.type(keys);
        harness.key("Down");
        harness.choose_text(keys);
        harness.key("BackSpace");
        const auto literal = std::string(keys).substr(0, std::string(keys).size() - 1);
        RAWKEY_ASSERT(harness.preedit() == literal);
        harness.expect_commit(literal);
    }
}

RAWKEY_SUITE("smart editing order independent phonetics", smart_order_independent) {
    constexpr std::array cases{
        std::pair{"standard", "us3"}, std::pair{"ibm", "a7,"},
        std::pair{"et", "en3"}, std::pair{"ginyieh", "-da"},
        std::pair{"dachen_cp26", "usr"}, std::pair{"hsu", "enf"}, std::pair{"et26", "enj"},
    };
    for (const auto& [layout, keys] : cases) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Down");
        const auto candidates = harness.candidates();
        RAWKEY_ASSERT(std::ranges::find(candidates, "你") != candidates.end());
        RAWKEY_ASSERT(std::ranges::find(candidates, keys) != candidates.end());
        harness.choose_text("你");
        harness.expect_commit("你");
        harness.reset();
        harness.type(keys);
        const auto shown = harness.preedit();
        if (std::string_view(layout) == "standard") RAWKEY_ASSERT(shown == "你");
        harness.key("BackSpace");
        if (shown == "你") RAWKEY_ASSERT(harness.composition_empty());
        else RAWKEY_ASSERT(harness.preedit() == std::string(keys).substr(0, std::string(keys).size() - 1));
    }
    // All six slot orders of ㄓㄨㄥ, not a special case for ㄋㄧ.
    for (const auto keys : {"5j/ ", "5/j ", "j5/ ", "j/5 ", "/5j ", "/j5 "}) {
        Harness harness(smart("standard"));
        harness.type(keys);
        RAWKEY_ASSERT(harness.preedit() == "中");
        harness.expect_commit("中");
    }
    for (const auto& [body, target] : std::array{
        std::pair{"1ul", "表"}, std::pair{"vu,", "寫"}, std::pair{"2u0", "點"}}) {
        std::string permutation(body);
        std::ranges::sort(permutation);
        do {
            Harness shuffled(smart("standard"));
            const auto raw = permutation + "3";
            shuffled.type(raw);
            shuffled.key("Down");
            const auto choices = shuffled.candidates();
            RAWKEY_ASSERT(std::ranges::find(choices, target) != choices.end());
            RAWKEY_ASSERT(std::ranges::find(choices, raw) != choices.end());
            shuffled.choose_text(target);
            shuffled.expect_commit(target);
        } while (std::next_permutation(permutation.begin(), permutation.end()));
    }
    Harness harness(smart("standard"));
    harness.type("uss3"); // a second ㄋ is not an empty-slot permutation
    harness.key("Down");
    const auto candidates = harness.candidates();
    RAWKEY_ASSERT(std::ranges::find(candidates, "你") == candidates.end());
    RAWKEY_ASSERT(std::ranges::find(candidates, "uss3") != candidates.end() || harness.preedit() == "uss3");
}

RAWKEY_SUITE("smart editing English negative controls", smart_edit_english) {
    for (const auto& [layout, keys] : layouts) {
        (void)keys;
        for (const auto text : {"hello", "world", "testing", "cache", "version", "added", "don't", "end-to-end"}) {
            Harness harness(smart(layout));
            harness.type(text);
            RAWKEY_ASSERT(harness.preedit() == text);
            harness.key("BackSpace");
            const auto expected = std::string(text).substr(0, std::string(text).size() - 1);
            RAWKEY_ASSERT(harness.preedit() == expected);
            harness.expect_commit(expected);
        }
    }
}

RAWKEY_SUITE("smart editing malformed syllables stay local", smart_local_phonetic_errors) {
    constexpr std::array initials{
        std::pair{"standard", "s3"}, std::pair{"hsu", "nf"},
        std::pair{"ibm", "7,"}, std::pair{"et", "n3"},
        std::pair{"ginyieh", "da"}, std::pair{"et26", "nj"},
        std::pair{"dachen_cp26", "sr"},
    };
    for (size_t index = 0; index < layouts.size(); ++index) {
        const auto& [layout, prefix] = layouts[index];
        const auto& [same_layout, keys] = initials[index];
        RAWKEY_ASSERT(std::string_view(layout) == same_layout);
        const auto broken = std::string(3, keys[0]);
        const auto closed = broken + keys[1];
        Harness harness(smart(layout));
        harness.type(prefix);
        harness.type(broken);
        RAWKEY_ASSERT(harness.preedit() == "你好" + broken);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(std::string(prefix) + broken));
        for (size_t key = 0; key < broken.size(); ++key) harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.type(closed);
        harness.type(prefix);
        RAWKEY_ASSERT(harness.preedit() == "你好" + closed + "你好");
        // Discover the error only after two more Chinese characters. Explicit
        // raw undo reaches the original error without deleting good prefix text.
        for (size_t key = 0; key < std::string_view(prefix).size() + closed.size(); ++key) {
            harness.key("Shift+BackSpace");
        }
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.type(prefix);
        RAWKEY_ASSERT(harness.preedit() == "你好你好");
        harness.expect_commit("你好你好");

        harness.reset();
        harness.type(std::string(prefix) + closed);
        harness.key("Down");
        harness.choose_text(std::string(prefix) + closed);
        harness.expect_commit(std::string(prefix) + closed);
        // Enter preserves unresolved raw keys; there is no silent typo fix.
        harness.type(std::string(prefix) + closed + prefix);
        harness.expect_commit("你好" + closed + "你好");
    }
}

RAWKEY_SUITE("smart editing local recovery still allows English reinterpretation", smart_local_error_controls) {
    for (const auto& [layout, prefix] : layouts) {
        (void)prefix;
        for (const auto text : {"nefarious", "nefariously", "nefariousness", "general", "generation",
                                "engineering", "hello", "added", "nefhwf_cache",
                                "https://example.org", "dev@example.com", "config.json"}) {
            Harness harness(smart(layout));
            harness.type(text);
            RAWKEY_ASSERT(harness.preedit() == text);
            harness.expect_commit(text);
        }
        Harness literal(smart(layout));
        literal.type("su3cl3_backup");
        // This identifier already has opposing ASCII/mixed interpretations.
        // A repair state must not take away the exact literal choice.
        literal.key("Down");
        if (literal.has_candidates()) literal.choose_text("su3cl3_backup");
        else RAWKEY_ASSERT(literal.preedit() == "su3cl3_backup");
        literal.expect_commit("su3cl3_backup");
    }
    Harness harness(smart("standard"));
    harness.type("5j/ ru,35. g;4x/3-4");
    RAWKEY_ASSERT(harness.preedit() == "中姐周上冷二");
    const auto& decision = harness.session()->mixed_decision;
    RAWKEY_ASSERT(std::ranges::none_of(decision.result.paths.at(decision.preview_path).segments, [](const auto& segment) {
        return segment.kind == MixedSegmentKind::BopomofoUnresolved;
    }));
    harness.expect_commit("中姐周上冷二");
}

RAWKEY_SUITE("smart valid mixed boundaries from reported sentence", smart_reported_app_boundary) {
    constexpr std::string_view raw = "xu/4j94vu04y94appao6u.3wj61ul ";
    constexpr std::string_view expected = "另外現在app沒有圖標";
    Harness harness(smart("standard"));
    harness.type(raw);
    RAWKEY_ASSERT(harness.preedit() == expected);
    harness.type("o4");
    RAWKEY_ASSERT(harness.preedit() == std::string(expected) + "欸");
    const auto& decision = harness.session()->mixed_decision;
    RAWKEY_ASSERT(std::ranges::none_of(decision.result.paths.at(decision.preview_path).segments, [](const auto& segment) {
        return segment.kind == MixedSegmentKind::BopomofoUnresolved;
    }));
    harness.key("Shift+BackSpace");
    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(std::string(raw) + "o"));
    harness.type("4");
    RAWKEY_ASSERT(harness.preedit() == std::string(expected) + "欸");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == expected);
    harness.expect_commit(expected);
    harness.type("o4");
    RAWKEY_ASSERT(harness.preedit() == "欸");
    harness.type("]");
    harness.expect_commit("欸]");
    harness.type(raw);
    harness.key("Down");
    harness.choose_text(raw);
    harness.expect_commit(raw);
}

RAWKEY_SUITE("smart valid mixed boundaries use lexical evidence without abbreviation exceptions", smart_lexical_islands) {
    for (const auto word : {"app", "api", "ui", "web", "hello", "cache", "github", "testing", "server"}) {
        for (const auto& [reading, character] : std::array{
            std::pair{"ao6", "沒"}, std::pair{"cl3", "好"}, std::pair{"su3", "你"}, std::pair{"o4", "欸"}}) {
            Harness harness(smart("standard"));
            const auto raw = std::string("su3cl3") + word + reading;
            const auto expected = std::string("你好") + word + character;
            harness.type(raw);
            RAWKEY_ASSERT(harness.preedit() == expected);
            harness.expect_commit(expected);
            harness.type(raw);
            harness.key("Down");
            harness.choose_text(raw);
            harness.expect_commit(raw);
        }
    }
    // Supported vowel-only readings remain complete, including weak-prior
    // interjections. Recovery must not label them as malformed syllables.
    for (const auto keys : {"o4", "k4", "-4", "i6"}) {
        Harness harness(smart("standard"));
        harness.type("su3cl3");
        harness.type(keys);
        const auto& decision = harness.session()->mixed_decision;
        RAWKEY_ASSERT(std::ranges::none_of(decision.result.paths.at(decision.preview_path).segments, [](const auto& segment) {
            return segment.kind == MixedSegmentKind::BopomofoUnresolved;
        }));
        RAWKEY_ASSERT(harness.preedit().starts_with("你好"));
        RAWKEY_ASSERT(harness.preedit() != std::string("你好") + keys);
        harness.expect_commit(harness.preedit());
    }
}

RAWKEY_SUITE("smart explicit singleton homophones remain selectable across layouts", smart_singleton_manual_recovery) {
    // Independent physical keys, not a fixture encoder. 巫 is eighth in the
    // ㄨ table: it must remain selectable even though the sentence beam only
    // expands the first four characters before model refinement.
    constexpr std::array keys{
        std::array{"standard", "j ", "g ", "n0 "},
        std::array{"hsu", "x ", "c ", "sm "},
        std::array{"ibm", "s ", "y ", "px "},
        std::array{"et", "x ", "/ ", "s8 "},
        std::array{"ginyieh", "[ ", "h ", "m0 "},
        std::array{"et26", "x ", "c ", "sm "},
        std::array{"dachen_cp26", "j ", "g ", "noo "},
    };
    for (const auto& row : keys) {
        Harness harness(smart(row[0]));
        harness.type(row[1]);
        harness.key("Down");
        harness.choose_text("巫");
        RAWKEY_ASSERT(harness.preedit() == "巫");
        harness.type(row[2]);
        harness.key("Down");
        harness.choose_text("師");
        RAWKEY_ASSERT(harness.preedit() == "巫師");
        harness.type(row[3]);
        harness.expect_commit("巫師三");
        RAWKEY_ASSERT(harness.last_commit() == "巫師三");
        RAWKEY_ASSERT(harness.composition_empty());
    }
}

RAWKEY_SUITE("smart lexical spans recover stranded singleton readings across layouts", smart_singleton_automatic_recovery) {
    constexpr std::array keys{
        std::array{"standard", "j g ", "n0 "}, std::array{"hsu", "x c ", "sm "},
        std::array{"ibm", "s y ", "px "}, std::array{"et", "x / ", "s8 "},
        std::array{"ginyieh", "[ h ", "m0 "}, std::array{"et26", "x c ", "sm "},
        std::array{"dachen_cp26", "j g ", "noo "},
    };
    for (const auto& row : keys) {
        Harness harness(smart(row[0]));
        const auto raw = std::string(row[1]) + row[2];
        harness.type(raw);
        RAWKEY_ASSERT(harness.preedit() == "巫師三");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw.substr(0, raw.size() - 1)));
        harness.type(" ");
        RAWKEY_ASSERT(harness.preedit() == "巫師三");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "巫師");
        harness.type(row[2]);
        RAWKEY_ASSERT(harness.preedit() == "巫師三");
        harness.expect_commit("巫師三");
        RAWKEY_ASSERT(harness.last_commit() == "巫師三");
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.expect_commit(raw);
        RAWKEY_ASSERT(harness.last_commit() == raw);
        // No Chinese anchor: the same ambiguous variables remain literal.
        harness.type(row[1]);
        RAWKEY_ASSERT(harness.preedit() == row[1]);
        harness.expect_commit(row[1]);
    }
    for (const auto& [raw, expected] : std::array{
        std::pair{"g bp6", "詩人"}, std::pair{"t z04", "吃飯"},
        std::pair{"u m04", "醫院"}, std::pair{"y rup ", "資金"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        RAWKEY_ASSERT(harness.preedit() == expected);
        harness.expect_commit(expected);
    }
    for (const auto raw : {"a i u ", "gcc -g ", "hello j g ", "git status ", "j g "}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        RAWKEY_ASSERT(harness.preedit() == raw);
        harness.expect_commit(raw);
    }
}

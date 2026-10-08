#include "raw_key_harness.hpp"
#include "bopomofo/keymap.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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

RAWKEY_SUITE("smart cursor insertion preview and commit stay at the caret", smart_cursor_insert) {
    for (const auto& [layout, keys] : layouts) {
        for (const int moves : {1, 2}) {
            Harness harness(smart(layout));
            harness.type(keys);
            for (int i = 0; i < moves; ++i) harness.key("Left");
            const std::string before = moves == 1 ? "你" : "";
            const std::string after = moves == 1 ? "好" : "你好";
            harness.type(std::string_view(keys).substr(0, 1));
            const auto unfinished = before + std::string(keys).substr(0, 1) + after;
            if (harness.preedit() != unfinished) {
                throw Failure{std::string(layout) + ": middle raw preview expected " + unfinished + ", got " + harness.preedit()};
            }
            RAWKEY_ASSERT(harness.engine().render_state(1).caret == static_cast<size_t>(3 - moves));
            harness.type(std::string_view(keys).substr(1));
            const auto expected = before + "你好" + after;
            RAWKEY_ASSERT(harness.preedit() == expected);
            RAWKEY_ASSERT(harness.engine().render_state(1).caret == static_cast<size_t>(4 - moves));
            RAWKEY_ASSERT(harness.commits().empty());
            harness.expect_commit(expected);
        }
    }
}

RAWKEY_SUITE("smart cursor insertion settles before continued navigation", smart_cursor_settle) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        harness.type(keys);
        harness.key("Right");
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        RAWKEY_ASSERT(harness.preedit() == "你你好好");
        RAWKEY_ASSERT(harness.engine().render_state(1).caret == 4);
        harness.key("Left");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你你好");
        harness.expect_commit("你你好");
    }
}

RAWKEY_SUITE("smart cursor insertion keeps literal islands and raw undo", smart_cursor_mixed) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "你hello好");
        RAWKEY_ASSERT(harness.engine().render_state(1).caret == 6);
        harness.type(keys);
        RAWKEY_ASSERT(harness.preedit() == "你hello你好好");
        for (size_t i = 0; i < std::string_view(keys).size(); ++i) harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你hello好");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你hell好");
        harness.type("o");
        harness.key("Right");
        RAWKEY_ASSERT(harness.preedit() == "你hello好");
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        harness.expect_commit("你hello好");
    }
}

RAWKEY_SUITE("smart cursor insertion candidates and lifecycle keep the suffix", smart_cursor_commit_routes) {
    for (const auto& [layout, keys] : layouts) {
        for (const auto route : {"select", "raw", "candidate-return", "focus-out", "deactivate", "shift-space"}) {
            Harness harness(smart(layout));
            harness.type(keys);
            harness.key("Down");
            harness.choose_text("你好");
            harness.key("Left");
            harness.type(keys);
            const auto expected = std::string("你你好好");
            if (std::string_view(route) == "focus-out") {
                harness.expect_focus_out_commit(expected);
            } else if (std::string_view(route) == "deactivate") {
                harness.engine().deactivate(1);
                RAWKEY_ASSERT(harness.last_commit() == expected);
                RAWKEY_ASSERT(harness.composition_empty());
            } else if (std::string_view(route) == "shift-space") {
                harness.expect_direct_commit(expected + " ", Key("Shift+space"));
            } else {
                harness.key("Down");
                if (std::string_view(route) == "raw") {
                    harness.choose_text(keys);
                    RAWKEY_ASSERT(harness.preedit() == "你" + std::string(keys) + "好");
                    harness.expect_commit("你" + std::string(keys) + "好");
                } else if (std::string_view(route) == "select") {
                    harness.choose_text("你好");
                    RAWKEY_ASSERT(harness.preedit() == expected);
                    RAWKEY_ASSERT(harness.session()->pending_token.empty());
                    RAWKEY_ASSERT(harness.session()->buffer.caret() == 3);
                    harness.expect_commit(expected);
                } else {
                    harness.expect_commit(expected);
                }
            }
        }
    }
}

RAWKEY_SUITE("smart cursor shifted navigation settles the inserted reading before marking", smart_cursor_marking) {
    Harness harness(smart("standard"));
    harness.type("su3cl3");
    harness.key("Left");
    harness.type("su3cl3");
    harness.key("Shift+Left");
    RAWKEY_ASSERT(harness.session()->pending_token.empty());
    RAWKEY_ASSERT(harness.preedit() == "你你好好");
    RAWKEY_ASSERT(harness.session()->buffer.marked_text() == u"好");
    harness.key("Escape");
    RAWKEY_ASSERT(harness.session()->buffer.caret() == 3);
    harness.expect_commit("你你好好");
}

RAWKEY_SUITE("smart cursor insertion commit samples follow the inserted positions", smart_cursor_training) {
    for (const auto text : {"su3cl3", "hellosu3cl3", "hello"}) {
        std::vector<InputEffect::CommitSample> samples;
        auto value = smart("standard");
        value.on_training_commit = [&](const auto& sample, std::u16string_view) { samples.push_back(sample); };
        Harness harness(value);
        harness.type("su3cl3");
        harness.key("Down");
        harness.choose_text("你好");
        harness.key("Left");
        harness.type(text);
        const auto expected = harness.preedit();
        harness.expect_commit(expected);
        RAWKEY_ASSERT(samples.size() == 1);
        const auto& sample = samples.front();
        RAWKEY_ASSERT(sample.answer == utf8_to_u16(expected));
        RAWKEY_ASSERT(sample.entries.front().reading == u"ㄋㄧˇ" && sample.entries.front().manually_selected);
        RAWKEY_ASSERT(sample.entries.back().reading == u"ㄏㄠˇ" && sample.entries.back().manually_selected);
        const bool literal = std::string_view(text).starts_with("hello");
        const size_t start = literal ? 6 : 1;
        if (literal) {
            for (size_t i = 1; i < 6; ++i) RAWKEY_ASSERT(sample.entries[i].literal);
        }
        if (std::string_view(text) != "hello") {
            RAWKEY_ASSERT(sample.entries[start].reading == u"ㄋㄧˇ");
            RAWKEY_ASSERT(sample.entries[start + 1].reading == u"ㄏㄠˇ");
            RAWKEY_ASSERT(!sample.entries[start].literal && !sample.entries[start].manually_selected);
        }
    }
}

RAWKEY_SUITE("smart cursor insertion in a mixed sentence preserves text on both sides", smart_cursor_sentence) {
    Harness harness(smart("standard"));
    harness.type("xu/4j94vu04y94appao6u.3wj61ul ");
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有圖標");
    harness.key("Left");
    harness.key("Left");
    harness.type("hellosu3cl3");
    if (harness.preedit() != "另外現在app沒有hello你好圖標") {
        throw Failure{"mixed sentence middle insertion: " + harness.preedit()};
    }
    RAWKEY_ASSERT(harness.engine().render_state(1).caret == 16);
    harness.key("Delete");
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有hello你好標");
    harness.expect_commit("另外現在app沒有hello你好標");
}

RAWKEY_SUITE("smart cursor insertion can settle for explicit English and repair wrong keys", smart_cursor_explicit_english) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.set_config("ShiftLetterKeys", "directly_put_to_buffer");
        harness.type(keys);
        harness.key("Left");
        harness.type(keys);
        harness.key("A");
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        RAWKEY_ASSERT(harness.preedit() == "你你好a好");
        harness.type("hello");
        harness.key("Left");
        RAWKEY_ASSERT(harness.preedit() == "你你好ahello好");
        harness.expect_commit("你你好ahello好");
    }
    Harness harness(smart("standard"));
    harness.type("su3cl3");
    harness.key("Left");
    harness.type("su3cl3sss");
    RAWKEY_ASSERT(harness.preedit() == "你你好sss好");
    for (int i = 0; i < 3; ++i) harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "你你好好");
    harness.expect_commit("你你好好");
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
        // Compact keys also have a valid conventional interpretation (Hsu
        // enf is ㄧㄣˇ). Its displayed Chinese character has the same deletion
        // contract as the reordered ㄋㄧˇ alternative, not a raw-key deletion.
        const bool displayed_chinese = shown != keys;
        if (displayed_chinese) RAWKEY_ASSERT(utf8_to_u16(shown).size() == 1);
        harness.key("BackSpace");
        if (displayed_chinese) RAWKEY_ASSERT(harness.composition_empty());
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

RAWKEY_SUITE("smart English lexical prefixes stay literal on every key", smart_english_prefixes) {
    for (const auto& [layout, chinese_keys] : layouts) {
        for (const auto word : {"clang", "make", "rfc", "sort", "configure", "migration",
                                "benchmark", "performance", "semicolon", "syntax",
                                "hello", "world", "testing", "cache", "version"}) {
            for (const bool confirmed_prefix : {false, true}) {
                Harness harness(smart(layout));
                const std::string prefix = confirmed_prefix ? "你好" : "";
                if (confirmed_prefix) {
                    harness.type(chinese_keys);
                    harness.key("Down");
                    harness.choose_text("你好");
                }
                std::string typed;
                for (const char key : std::string_view(word)) {
                    typed += key;
                    harness.type(std::string(1, key));
                    if (harness.preedit() != prefix + typed) {
                        throw Failure{std::string(layout) + ": English prefix " + typed + " of " + word +
                                      " became " + harness.preedit()};
                    }
                    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(typed));
                    RAWKEY_ASSERT(harness.commits().empty());
                }
                harness.expect_commit(prefix + word);
            }
        }
    }
}

RAWKEY_SUITE("smart isolated tone keys stay literal without a syllable", smart_isolated_tone_literals) {
    for (const auto& [layout, prefix] : layouts) {
        const auto keyboard = bopomofo_keyboard_layout(layout);
        for (char32_t key = U'!'; key <= U'~'; ++key) {
            if ((key >= U'a' && key <= U'z') || (key >= U'A' && key <= U'Z') ||
                !is_bopomofo_tone_key(key, keyboard)) continue;
            const auto text = std::string(1, static_cast<char>(key));
            Harness harness(smart(layout));
            harness.type(text);
            RAWKEY_ASSERT(harness.preedit() == text);
            RAWKEY_ASSERT(!harness.session()->buffer.has_unfinished_reading());
            harness.key("BackSpace");
            RAWKEY_ASSERT(harness.composition_empty());
            harness.type(text);
            harness.expect_commit(text);
        }
        // A body still completes normally; an isolated tone repair must not
        // take over the actual tone of a pending phonetic syllable.
        Harness harness(smart(layout));
        harness.type(prefix);
        harness.expect_commit("你好");
    }
    // Explicit English intent is valid inside Chinese as well as at the
    // beginning. The filename separator must not become an orphan IBM tone.
    for (const auto& [layout, prefix] : layouts) {
        Harness harness(smart(layout));
        harness.set_config("ShiftLetterKeys", "directly_put_to_buffer");
        harness.type(prefix);
        harness.key("Down");
        harness.choose_text("你好");
        for (const char key : std::string_view("readme")) harness.key(Key(key).with(kCapsLock));
        harness.type(".md");
        if (harness.preedit() != "你好README.md") {
            throw Failure{std::string(layout) + ": expected 你好README.md, got " + harness.preedit()};
        }
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你好README.m");
        harness.type("d");
        harness.expect_commit("你好README.md");
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

RAWKEY_SUITE("smart editing long malformed runs preserve Chinese and exact raw", smart_long_error_runs) {
    constexpr std::array cases{
        std::pair{"standard", 's'}, std::pair{"hsu", 'n'},
        std::pair{"ibm", '7'}, std::pair{"et", 'n'},
        std::pair{"ginyieh", 'd'}, std::pair{"et26", 'n'},
    };
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& [layout, key] = cases[index];
        const auto prefix = std::string(layouts[index].second);
        for (const size_t count : {7U, 12U, 24U}) {
            const auto wrong = std::string(count, key);
            Harness harness(smart(layout));
            harness.type(prefix);
            harness.type(wrong);
            RAWKEY_ASSERT(harness.preedit() == "你好" + wrong);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(prefix + wrong));
            for (size_t i = 0; i < count; ++i) harness.key("BackSpace");
            RAWKEY_ASSERT(harness.preedit() == "你好");
            harness.type(wrong);
            for (size_t i = 0; i < count; ++i) harness.key("Shift+BackSpace");
            RAWKEY_ASSERT(harness.preedit() == "你好");
            harness.type(wrong);
            harness.expect_commit("你好" + wrong);
            harness.type(prefix + wrong);
            harness.key("Down");
            harness.choose_text(prefix + wrong);
            harness.expect_commit(prefix + wrong);
        }
    }
    // Check the observed abrupt transition on each keystroke, and ensure a
    // later completion boundary still admits the next valid Chinese reading.
    for (const auto& [layout, prefix, key, tone] : std::array{
        std::tuple{"standard", "su3cl3", 's', '3'},
        std::tuple{"et", "ne3hz3", 'n', '3'}}) {
        Harness harness(smart(layout));
        harness.type(prefix);
        for (size_t count = 1; count <= 24; ++count) {
            harness.type(std::string(1, key));
            RAWKEY_ASSERT(harness.preedit() == "你好" + std::string(count, key));
        }
        harness.type(std::string(1, tone));
        harness.type(prefix);
        harness.expect_commit("你好" + std::string(24, key) + tone + "你好");
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

RAWKEY_SUITE("smart leading first tone gang is Chinese before any following word", smart_leading_gang) {
    constexpr std::array keys{
        std::pair{"standard", "e; "}, std::pair{"hsu", "gk "},
        std::pair{"ibm", "9v "}, std::pair{"et", "v0 "},
        std::pair{"ginyieh", "r; "}, std::pair{"et26", "vt "},
        std::pair{"dachen_cp26", "ell "},
    };
    for (const auto& [layout, raw] : keys) {
        Harness harness(smart(layout));
        const std::string body = std::string(raw).substr(0, std::string_view(raw).size() - 1);
        harness.type(body);
        RAWKEY_ASSERT(harness.commits().empty());
        harness.key("space");
        if (harness.preedit() != "剛") throw Failure{std::string(layout) + ": leading 剛 became " + harness.preedit()};
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(body));
        harness.key("space");
        RAWKEY_ASSERT(harness.preedit() == "剛");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.composition_empty());
        harness.type(raw);
        harness.expect_commit("剛");
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.expect_commit(raw);
    }
}

RAWKEY_SUITE("smart first tone punctuation keys do not depend on a preceding Chinese anchor", smart_first_tone_symbols) {
    // Independent physical keys for distinct initials/finals. This checks the
    // phonetic path, without pinning unrelated same-reading character choices.
    for (const auto& [raw, reading] : std::array{
        std::pair{"e; ", u"ㄍㄤ "},
        std::pair{"e/ ", u"ㄍㄥ "}, std::pair{"ej/ ", u"ㄍㄨㄥ "},
        std::pair{"fu. ", u"ㄑㄧㄡ "}, std::pair{"vu/ ", u"ㄒㄧㄥ "},
    }) {
        Harness harness(smart("standard"));
        harness.type(raw);
        const auto& decision = harness.session()->mixed_decision;
        const auto& path = decision.result.paths.at(decision.preview_path);
        if (path.segments.size() != 1) throw Failure{std::string(raw) + ": expected one phonetic segment, got " + harness.preedit()};
        RAWKEY_ASSERT(path.segments.front().kind == MixedSegmentKind::Bopomofo);
        RAWKEY_ASSERT(path.segments.front().reading == reading);
        RAWKEY_ASSERT(harness.preedit() != raw);
        harness.expect_commit(harness.preedit());
    }
    // An isolated completed phonetic spelling now prefers Chinese, with the
    // exact literal still available without configuring a replacement rule.
    {
        Harness harness(smart("standard"));
        harness.type("d; ");
        RAWKEY_ASSERT(harness.preedit() == "康");
        harness.key("Down");
        harness.choose_text("d; ");
        harness.expect_commit("d; ");
    }
    for (const auto& [layout, raw, following] : std::array{
        std::tuple{"standard", "e; ", "h96"}, std::tuple{"ginyieh", "r; ", "j9q"},
    }) {
        Harness harness(smart(layout));
        harness.type(raw);
        RAWKEY_ASSERT(harness.preedit() == "剛");
        harness.type(following);
        RAWKEY_ASSERT(harness.preedit() == "剛才");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "剛");
        harness.expect_commit("剛");
    }
    for (const auto& [layout, prefix] : layouts) {
        (void)prefix;
        for (const auto raw : {"hello; ", "world. ", "config.json ", "readme.md ", "foo_bar; ",
                               "https://example.org ", "git status; "}) {
            Harness harness(smart(layout));
            harness.type(raw);
            if (harness.preedit() != raw) throw Failure{std::string(layout) + ": punctuation literal " + raw + " became " + harness.preedit()};
            harness.expect_commit(raw);
        }
    }
    // A phonetic-looking suffix after a literal command is not a leading
    // syllable. Do not give command options the new initial-reading evidence.
    for (const auto layout : {"standard", "ginyieh"}) {
        Harness harness(smart(layout));
        harness.type("gcc -g ");
        RAWKEY_ASSERT(harness.preedit() == "gcc -g ");
        harness.expect_commit("gcc -g ");
    }
}

RAWKEY_SUITE("smart leading reading audit preserves Chinese and exact raw recovery", smart_leading_audit_recovery) {
    // Independent physical keys across layouts: ordinary leading readings
    // should not need manual recovery, and literal intent remains reversible.
    constexpr std::array keys{
        std::array{"standard", "d; ", "w; ", "d/ ", "t. ", "y; "},
        std::array{"hsu", "kk ", "tk ", "kl ", "vo ", "zk "},
        std::array{"ibm", "0v ", "6v ", "0b ", "tz ", "iv "},
        std::array{"et", "k0 ", "t0 ", "k- ", ".y ", ";0 "},
        std::array{"ginyieh", "f; ", "e; ", "f/ ", "y. ", "u; "},
        std::array{"et26", "kt ", "tt ", "kl ", "yp ", "qt "},
        std::array{"dachen_cp26", "dll ", "wwll ", "dn ", "ttmm ", "yll "},
    };
    constexpr std::array words{"康", "湯", "坑", "抽", "髒"};
    for (const auto& row : keys) {
        const auto greeting = std::ranges::find_if(layouts, [&](const auto& entry) {
            return std::string_view(entry.first) == row[0];
        });
        RAWKEY_ASSERT(greeting != layouts.end());
        for (size_t index = 0; index < words.size(); ++index) {
            Harness chinese(smart(row[0]));
            const auto body = std::string(row[index + 1]).substr(0, std::string_view(row[index + 1]).size() - 1);
            std::string typed;
            for (const char key : body) {
                typed += key;
                chinese.type(std::string(1, key));
                RAWKEY_ASSERT(chinese.preedit() == typed);
                RAWKEY_ASSERT(chinese.commits().empty());
            }
            chinese.key("space");
            if (chinese.preedit() != words[index]) {
                throw Failure{std::string(row[0]) + ": leading " + words[index] + " became " + chinese.preedit()};
            }
            RAWKEY_ASSERT(chinese.session()->pending_token.raw == utf8_to_u16(row[index + 1]));
            chinese.key("Shift+BackSpace");
            RAWKEY_ASSERT(chinese.session()->pending_token.raw == utf8_to_u16(body));
            chinese.key("space");
            RAWKEY_ASSERT(chinese.preedit() == words[index]);
            chinese.key("BackSpace");
            RAWKEY_ASSERT(chinese.composition_empty());
            chinese.type(row[index + 1]);
            chinese.expect_commit(words[index]);
            chinese.type(row[index + 1]);
            chinese.key("Down");
            chinese.choose_text(words[index]);
            chinese.expect_commit(words[index]);
            Harness literal(smart(row[0]));
            literal.type(row[index + 1]);
            literal.key("Down");
            literal.choose_text(row[index + 1]);
            literal.type(greeting->second);
            RAWKEY_ASSERT(literal.preedit() == std::string(row[index + 1]) + "你好");
            literal.key("BackSpace");
            RAWKEY_ASSERT(literal.preedit() == std::string(row[index + 1]) + "你");
            literal.key("BackSpace");
            RAWKEY_ASSERT(literal.preedit() == row[index + 1]);
            literal.expect_commit(row[index + 1]);
        }
    }
}

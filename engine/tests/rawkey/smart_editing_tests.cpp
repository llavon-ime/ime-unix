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
    // A Shift punctuation key right after Chinese is Chinese punctuation.
    harness.type("]");
    harness.expect_commit("欸」");
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

RAWKEY_SUITE("smart completed leading reading survives the next reading's keys", smart_leading_tone_not_onset) {
    // A finished syllable's tone key must not be reread as the start of an
    // unfinished next syllable, which turned the displayed 成 back into raw.
    for (const auto& [raw, first, expected] : std::array{
        std::tuple{"t/6dk4", "成", "成客"}, std::tuple{"j;6vu; ", "王", "王鄉"},
        std::tuple{"-6ji3", "兒", "兒我"}}) {
        Harness harness(smart("standard"));
        const std::string_view keys(raw);
        // Every prefix from the completing tone key onward shows the reading.
        const auto tone = keys.find_first_of("6");
        for (std::size_t i = 0; i < keys.size(); ++i) {
            harness.type(std::string(1, keys[i]));
            if (i >= tone && !harness.preedit().starts_with(first)) {
                throw Failure{std::string(raw) + ": prefix " + std::string(keys.substr(0, i + 1)) +
                              " became " + harness.preedit()};
            }
            RAWKEY_ASSERT(harness.commits().empty());
        }
        RAWKEY_ASSERT(harness.preedit() == expected);
        harness.expect_commit(expected);
    }
}

RAWKEY_SUITE("smart pending after Chinese text pays the language switch", smart_leading_after_chinese) {
    // Text before the caret is the same evidence as Chinese earlier in one
    // pending token: once the input has an explicit tone, a separated literal
    // start is a language switch. Context alone never rewrites short English.
    const auto segmentation = [](Harness& harness) {
        std::string text;
        const auto& decision = harness.session()->mixed_decision;
        for (const auto& segment : decision.result.paths.at(decision.preview_path).segments) {
            if (segment.consumed_boundary) continue;
            if (!text.empty()) text += ' ';
            text += std::string(segment.kind == MixedSegmentKind::Bopomofo ? "B:" :
                                segment.kind == MixedSegmentKind::Latin ? "L:" : "S:") + u16_to_utf8(segment.raw);
        }
        return text;
    };
    for (const auto& [before, cursor] : std::array{std::pair{"我想", std::size_t{2}},
                                                   std::pair{"好了。", std::size_t{3}},
                                                   std::pair{"𠮷", std::size_t{2}}}) {
        for (const auto& [raw, expected] : std::array{std::pair{"up m,4", "B:up  B:m,4"},
                                                      std::pair{"co 2ul4", "B:co  B:2ul4"}}) {
            Harness harness(smart("standard"));
            harness.set_surrounding(before, cursor, cursor);
            harness.type(raw);
            if (segmentation(harness) != expected) throw Failure{std::string(raw) + " decoded as " + segmentation(harness)};
            RAWKEY_ASSERT(harness.commits().empty());
            // The exact raw choice remains reachable from the candidate panel.
            harness.key("Down");
            harness.choose_text(raw);
            harness.expect_commit(raw);
        }
        for (const auto& [raw, expected] : std::array{std::pair{"u ek4", "一個"}, std::pair{"t z04", "吃飯"}}) {
            Harness harness(smart("standard"));
            harness.set_surrounding(before, cursor, cursor);
            harness.type(raw);
            RAWKEY_ASSERT(harness.preedit() == expected);
            harness.expect_commit(expected);
        }
        // Literal English, commands and numbers remain literal after Chinese.
        for (const auto raw : {"hello world", "git status ", "a test", "2024 ", "v3.14 ", "u ", "u r", "x y z",
                               "ha ha", "a = b + 1", "ls -la"}) {
            Harness harness(smart("standard"));
            harness.set_surrounding(before, cursor, cursor);
            harness.type(raw);
            if (harness.preedit() != raw) throw Failure{std::string(raw) + " became " + harness.preedit()};
            harness.expect_commit(raw);
        }
    }
    {
        // A Chinese composition settled before the caret is the same evidence.
        Harness harness(smart("standard"));
        harness.type("su3cl3");
        harness.key("Down");
        harness.choose_text("你好");
        harness.type("up m,4");
        RAWKEY_ASSERT(segmentation(harness) == "B:up  B:m,4");
        harness.expect_commit(harness.preedit());
    }
    // Without Chinese before the caret the English word start stays literal.
    for (const auto& [before, cursor] : std::array{std::pair{"", std::size_t{0}},
                                                   std::pair{"I use ", std::size_t{6}}}) {
        Harness harness(smart("standard"));
        harness.set_surrounding(before, cursor, cursor);
        harness.type("up m,4");
        RAWKEY_ASSERT(segmentation(harness) == "L:up S:  B:m,4");
        harness.expect_commit(harness.preedit());
    }
}

namespace {
// The displayed path as kind:raw pieces, e.g. "B:g4 N:100 B:m06". Homophone
// order is not asserted, only how the raw keys were interpreted.
std::string displayed_segmentation(const Harness& harness) {
    const auto& decision = harness.session()->mixed_decision;
    if (!decision.active() || decision.preview_path >= decision.result.paths.size()) return {};
    std::string text;
    for (const auto& segment : decision.result.paths[decision.preview_path].segments) {
        if (segment.consumed_boundary) continue;
        const char* kind = segment.kind == MixedSegmentKind::Bopomofo ? "B" :
            segment.kind == MixedSegmentKind::Number ? "N" :
            segment.kind == MixedSegmentKind::Latin ? "L" : segment.kind == MixedSegmentKind::Symbol ? "S" : "?";
        if (!text.empty()) text += ' ';
        text += std::string(kind) + ":" + u16_to_utf8(segment.raw);
    }
    return text;
}
} // namespace

RAWKEY_SUITE("smart numbers between Chinese readings keep their boundaries", smart_numbers_inside_chinese) {
    // Standard digit keys are also phonetic keys. A number followed by a valid
    // reading is a mixed boundary, not one mistyped syllable swallowing both.
    for (const auto& [raw, expected] : std::array{
        std::pair{"ru84ek6g4100m06", "B:ru84 B:ek6 B:g4 N:100 B:m06"},
        std::pair{"2u41au/6", "B:2u4 N:1 B:au/6"},
        std::pair{"2024su06", "N:2024 B:su06"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (displayed_segmentation(harness) != expected) {
            throw Failure{std::string(raw) + " decoded as " + displayed_segmentation(harness)};
        }
        harness.expect_commit(harness.preedit());
    }
    // Numbers that are not followed by a reading stay exact.
    for (const auto raw : {"100 ", "v3.14 ", "1031p3v2"}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (std::string_view(raw) == "1031p3v2") {
            RAWKEY_ASSERT(displayed_segmentation(harness) == "B:103 B:1p3 L:v2");
        } else {
            RAWKEY_ASSERT(harness.preedit() == raw);
        }
        harness.expect_commit(harness.preedit());
    }
}

RAWKEY_SUITE("smart one letter and Space before a toned reading is a reading", smart_singleton_before_tone) {
    // Without this, a leading single-key reading stayed English for good when
    // the next characters were not a dictionary phrase (t 了沒).
    for (const auto& [raw, expected] : std::array{
        std::pair{"t xk7ao6", "B:t  B:xk7 B:ao6"}, std::pair{"y ek4", "B:y  B:ek4"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (displayed_segmentation(harness) != expected) {
            throw Failure{std::string(raw) + " decoded as " + displayed_segmentation(harness)};
        }
        harness.expect_commit(harness.preedit());
    }
    RAWKEY_ASSERT([] {
        Harness harness(smart("standard"));
        harness.type("t xk7ao6");
        return harness.preedit() == "吃了沒";
    }());
    // Literal letters, words and Space-closed ambiguity are unchanged.
    for (const auto raw : {"a test", "j g ", "hello j g ", "a i u "}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (harness.preedit() != raw) throw Failure{std::string(raw) + " became " + harness.preedit()};
        harness.expect_commit(raw);
    }
}

RAWKEY_SUITE("smart opaque tokens start after explicitly toned Chinese", smart_opaque_after_toned) {
    // Standard digit tones close each reading. An email, identifier or URL
    // that follows must not swallow those completed readings as its prefix.
    for (const auto& [raw, expected] : std::array{
        std::pair{"fu/3ru42l4dev@example.com", "B:fu/3 B:ru4 B:2l4 L:dev@example.com"},
        std::pair{"ji32k7yj/3g4report_2026", "B:ji3 B:2k7 B:yj/3 B:g4 L:report_2026"},
        std::pair{"su3cl3user_name42", "B:su3 B:cl3 L:user_name42"},
        // A dictionary word between readings does not let the name start early.
        std::pair{"ji32k7emailg4test@gmail.com", "B:ji3 B:2k7 L:email B:g4 L:test@gmail.com"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (displayed_segmentation(harness) != expected) {
            throw Failure{std::string(raw) + " decoded as " + displayed_segmentation(harness)};
        }
        // The complete raw spelling stays one candidate away.
        harness.key("Down");
        const auto entries = harness.candidates();
        RAWKEY_ASSERT(std::ranges::find(entries, std::string(raw)) != entries.end());
        harness.key("Escape");
        harness.expect_commit(harness.preedit());
    }
    // Names whose letters merely contain a toned reading stay whole.
    for (const auto raw : {"x86_64", "mp3_file", "h264", "v4l2", "user_2026_name", "abc123_foo"}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (harness.preedit() != raw) throw Failure{std::string(raw) + " became " + harness.preedit()};
        harness.expect_commit(raw);
    }
}

RAWKEY_SUITE("smart Space between Chinese and ASCII is kept as a separator", smart_space_separator) {
    for (const auto& [raw, expected] : std::array{
        std::pair{"ji3m/4 python vu,3t/6g4", "我用 python 寫程式"},
        std::pair{"j;353g4 https://example.com", "網址是 https://example.com"},
        std::pair{"vu04y94 2024 su06", "現在 2024 年"},
        std::pair{"2;304y94 ~/docs/a.txt", "檔案在 ~/docs/a.txt"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        // Readings are model-free here; assert the separators and the ASCII.
        const auto shown = harness.preedit();
        if (utf8_to_u16(shown).size() != utf8_to_u16(expected).size() ||
            std::ranges::count(shown, ' ') != std::ranges::count(std::string_view(expected), ' ')) {
            throw Failure{std::string(raw) + " became " + shown};
        }
        harness.expect_commit(shown);
    }
    // A Space between readings, at the end, or before sentence punctuation
    // still closes the reading.
    for (const auto& [raw, expected] : std::array{std::pair{"su3 cl3", "你好"}, std::pair{"su3 ", "你"},
                                                  std::pair{"su3 ?", "你？"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (harness.preedit() != expected) throw Failure{std::string(raw) + " became " + harness.preedit()};
        harness.expect_commit(expected);
    }
}

RAWKEY_SUITE("smart Escape keeps the raw keys when typing continues", smart_escape_keeps_raw) {
    Harness harness(smart("standard"));
    harness.type("rup wu0 ");
    RAWKEY_ASSERT(harness.preedit() == "今天");
    harness.key("Escape");
    RAWKEY_ASSERT(harness.preedit() == "rup wu0 ");
    // The next keys must not reinterpret the keys the user chose to keep.
    harness.type("au/6wu0 ");
    RAWKEY_ASSERT(harness.preedit() == "rup wu0 明天");
    // Escape still reverts and then clears only the newer pending keys.
    harness.key("Escape");
    RAWKEY_ASSERT(harness.preedit() == "rup wu0 au/6wu0 ");
    harness.key("Escape");
    RAWKEY_ASSERT(harness.preedit() == "rup wu0 ");
    harness.expect_commit("rup wu0 ");
    // Without Escape the same keys stay one Chinese composition.
    Harness plain(smart("standard"));
    plain.type("rup wu0 au/6wu0 ");
    RAWKEY_ASSERT(plain.preedit() == "今天明天");
    plain.expect_commit("今天明天");
}

RAWKEY_SUITE("smart number followed by a first-tone reading keeps the reading", smart_number_first_tone) {
    Harness harness(smart("standard"));
    harness.type("284m, 20zp 5j/ ");
    RAWKEY_ASSERT(displayed_segmentation(harness) == "B:284 B:m,  N:20 B:zp  B:5j/ ");
    harness.expect_commit(harness.preedit());
    for (const auto raw : {"1080p ", "5g ", "4k ", "mp3 ", "iphone15 "}) {
        // Also right after Chinese, where the number resumes a reading.
        Harness after(smart("standard"));
        after.type(std::string("u04qu04g4 ") + raw);
        if (!after.preedit().ends_with(std::string(" ") + raw)) {
            throw Failure{std::string(raw) + " after Chinese became " + after.preedit()};
        }
        Harness literal(smart("standard"));
        literal.type(raw);
        if (literal.preedit() != raw) throw Failure{std::string(raw) + " became " + literal.preedit()};
        literal.expect_commit(raw);
    }
}

RAWKEY_SUITE("smart Shift punctuation after Chinese is Chinese punctuation", smart_sentence_punctuation) {
    for (const auto& [raw, expected] : std::array{
        std::pair{"su3cl3<ji3g4", "你好，我是"}, std::pair{"5p 2k7a87?", "真的嗎？"},
        std::pair{"w94cl3xk7!", "太好了！"}, std::pair{"cl3>", "好。"},
        std::pair{"5j4u4:au/6wu0 ", "注意：明天"}, std::pair{"cl3'", "好、"}}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        const auto shown = harness.preedit();
        // Readings are model-free here; assert the punctuation itself.
        const auto mark = utf8_to_u16(std::string(expected)).find_first_of(u"，。！？：、");
        if (mark == std::u16string::npos || utf8_to_u16(shown).size() != utf8_to_u16(std::string(expected)).size() ||
            utf8_to_u16(shown)[mark] != utf8_to_u16(std::string(expected))[mark]) {
            throw Failure{std::string(raw) + " became " + shown};
        }
        harness.expect_commit(shown);
        // The exact ASCII spelling stays selectable.
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.expect_commit(raw);
    }
    // ASCII syntax and punctuation after English or at the start stay ASCII.
    for (const auto raw : {"a<b", "x>1", "hello!", "ok?", "https://example.com/a?b=c", "f(x)", "foo:bar"}) {
        Harness harness(smart("standard"));
        harness.type(raw);
        if (harness.preedit() != raw) throw Failure{std::string(raw) + " became " + harness.preedit()};
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

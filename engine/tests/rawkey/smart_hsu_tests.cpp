#include "raw_key_harness.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <tuple>

using namespace llavon::ime::rawkey;

namespace {
HarnessOptions hsu_smart() {
    HarnessOptions options;
    options.config.smart_english = true;
    options.config.keyboard_layout = "hsu";
    return options;
}
} // namespace

RAWKEY_SUITE("Hsu completed letter tones recover ordinary Chinese and remain reversible", hsu_completed_words) {
    for (const auto& [keys, text] : std::array{
        std::pair{"mif", "買"}, std::pair{"gof", "狗"}, std::pair{"hif", "海"},
        std::pair{"myf", "馬"}, std::pair{"mef", "米"}, std::pair{"kof", "口"},
        std::pair{"mof", "某"}, std::pair{"bej", "必"}, std::pair{"leof", "柳"},
        std::pair{"ij", "愛"}, std::pair{"vd", "持"}, std::pair{"veof", "糗"},
        std::pair{"naf", "餒"}, std::pair{"vf", "尺"}, std::pair{"goj", "夠"}}) {
        Harness harness(hsu_smart());
        const std::string raw(keys);
        harness.type(raw.substr(0, raw.size() - 1));
        RAWKEY_ASSERT(harness.preedit() == raw.substr(0, raw.size() - 1));
        harness.type(raw.substr(raw.size() - 1));
        if (harness.preedit() != text) throw Failure{raw + " expected " + text + ", got " + harness.preedit()};
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw));
        RAWKEY_ASSERT(harness.commits().empty());
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, raw.size() - 1)));
        harness.type(raw.substr(raw.size() - 1));
        RAWKEY_ASSERT(harness.preedit() == text);
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.composition_empty());
        harness.type(raw);
        harness.expect_commit(text);
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.type("nefhwf");
        RAWKEY_ASSERT(harness.preedit() == raw + "你好");
        harness.expect_commit(raw + "你好");
    }
}

RAWKEY_SUITE("Hsu English typing protects complete words structure and common prefixes", hsu_english_typing) {
    for (const auto word : {"hello", "world", "testing", "added", "adds", "adjust", "before", "after",
                            "software", "hardware", "standard",
                            "definition", "comfortable", "different", "download", "shared", "finished",
                            "classification", "performance", "notification", "configuration", "clang",
                            "make", "rfc", "sort", "email", "record", "user", "password", "microsoft",
                            "my_file_47", "image.png", "data_003", "config.json", "https://example.org/a",
                            "user@example.com", "home/user/bin", "v3.14159", "v127.0.0.1"}) {
        for (const bool prefix : {false, true}) {
            Harness harness(hsu_smart());
            const std::string chinese = prefix ? "你好" : "";
            if (prefix) {
                harness.type("nefhwf");
                harness.key("Down");
                harness.choose_text("你好");
            }
            std::string raw;
            for (const char key : std::string_view(word)) {
                raw += key;
                harness.type(std::string(1, key));
                if (harness.preedit() != chinese + raw) {
                    throw Failure{std::string(word) + " at " + raw + " became " + harness.preedit()};
                }
                RAWKEY_ASSERT(harness.commits().empty());
            }
            harness.expect_commit(chinese + word);
        }
    }
}

RAWKEY_SUITE("Hsu ambiguous rare English completion can reverse a Chinese prefix", hsu_rare_prefix_reversal) {
    // Identical keys cannot be both literal at every prefix and the completed
    // Chinese reading. Keep the common Chinese intent, but never pin its guess
    // when later keys establish the English word (including unlisted suffixes).
    for (const auto suffix : {"arious", "ariously", "ariousness", "hwf_backup"}) {
        Harness harness(hsu_smart());
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type(suffix);
        const auto raw = std::string("nef") + suffix;
        RAWKEY_ASSERT(harness.preedit() == raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw));
        RAWKEY_ASSERT(harness.commits().empty());
        harness.expect_commit(raw);
    }
}

RAWKEY_SUITE("Hsu bare numbers commit literally without a phantom phonetic composition", hsu_numbers) {
    Harness harness(hsu_smart());
    for (const char key : std::string_view("31415926780")) {
        harness.key(Key(key));
        RAWKEY_ASSERT(harness.composition_empty());
        RAWKEY_ASSERT(harness.last_commit() == std::string(1, key));
    }
}

RAWKEY_SUITE("Hsu new Chinese completions can be inserted and recovered at a middle caret", hsu_middle_completion) {
    for (const auto& [keys, text] : std::array{
        std::pair{"mif", "買"}, std::pair{"gof", "狗"}, std::pair{"hif", "海"},
        std::pair{"mef", "米"}, std::pair{"leof", "柳"}}) {
        for (const bool literal : {false, true}) {
            Harness harness(hsu_smart());
            harness.type("nefhwf");
            harness.key("Down");
            harness.choose_text("你好");
            harness.key("Left");
            harness.type(keys);
            if (harness.preedit() != std::string("你") + text + "好") {
                throw Failure{std::string(keys) + " middle insertion became " + harness.preedit()};
            }
            harness.key("Shift+BackSpace");
            const auto body = std::string(keys).substr(0, std::string_view(keys).size() - 1);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(body));
            harness.type(std::string(keys).substr(body.size()));
            RAWKEY_ASSERT(harness.preedit() == std::string("你") + text + "好");
            if (literal) {
                harness.key("Down");
                harness.choose_text(keys);
            }
            harness.expect_commit(std::string("你") + (literal ? keys : text) + "好");
        }
    }
}

RAWKEY_SUITE("Hsu command options do not consume the next Chinese onset", hsu_command_option) {
    Harness harness(hsu_smart());
    harness.type("jey ckj-gjeojkgfefvxdaxhj");
    RAWKEY_ASSERT(harness.preedit() == "加上-g就可以除錯");
    harness.expect_commit("加上-g就可以除錯");
}

RAWKEY_SUITE("Hsu unfinished continuations keep existing Chinese and literal islands", hsu_continuation_stability) {
    for (const auto& [keys, text] : std::array{
        std::pair{"tk hi", "湯hi"}, std::pair{"sn le", "森le"},
        std::pair{"ty ma", "它ma"}, std::pair{"z jen ef", "資金以"},
        std::pair{"cenjcek lef", "信鄉李"}, std::pair{"uljvimd", "用vimd"},
        std::pair{"cgjdeljapij ho", "設定api之ho"}, std::pair{"nefa", "nefa"}}) {
        Harness harness(hsu_smart());
        harness.type(keys);
        if (harness.preedit() != text) throw Failure{std::string(keys) + " became " + harness.preedit()};
        RAWKEY_ASSERT(harness.commits().empty());
        harness.expect_commit(text);
    }
}

RAWKEY_SUITE("Hsu contextual key meanings outrank reordered interpretations without losing alternatives", hsu_ordered_meanings) {
    for (const auto& [keys, text, alternate] : std::array{
        std::tuple{"emf", "眼", "米"}, std::tuple{"emj", "驗", "密"},
        std::tuple{"xhj", "握", "護"}, std::tuple{"xk ", "汪", "哭"},
        std::tuple{"xa ", "威", "粗"}, std::tuple{"enf", "引", "你"}}) {
        Harness harness(hsu_smart());
        harness.type(keys);
        if (harness.preedit() != text) throw Failure{std::string(keys) + " expected " + text + ", got " + harness.preedit()};
        harness.expect_commit(text);
        harness.type(keys);
        harness.key("Down");
        harness.choose_text(alternate);
        harness.expect_commit(alternate);
        harness.type(keys);
        harness.key("Down");
        harness.choose_text(keys);
        harness.expect_commit(keys);
    }
}

RAWKEY_SUITE("Hsu phrase evidence repairs short tone fragments without losing raw editing", hsu_phrase_boundaries) {
    for (const auto& [keys, readings] : std::array{
        std::pair{"dlfedceyjxhfjeojgxhjvuj", "ㄉㄥˇ|ㄧˊ|ㄒㄧㄚˋ|ㄨㄛˇ|ㄐㄧㄡˋ|ㄍㄨㄛˋ|ㄑㄩˋ"},
        std::pair{"kofdijlefeofleldvemd", "ㄎㄡˇ|ㄉㄞˋ|ㄌㄧˇ|ㄧㄡˇ|ㄌㄧㄥˊ|ㄑㄧㄢˊ"},
        std::pair{"dlfedceyjcallxhf", "ㄉㄥˇ|ㄧˊ|ㄒㄧㄚˋ|ㄨㄛˇ"},
        std::pair{"vel vxdcachezijcjedaj", "ㄑㄧㄥ |ㄔㄨˊ|ㄗㄞˋ|ㄕˋ|ㄧˊ|ㄘˋ"},
        std::pair{"uljvimdyfki dkfmj", "ㄩㄥˋ|ㄉㄚˇ|ㄎㄞ |ㄉㄤˇ|ㄢˋ"}}) {
        Harness harness(hsu_smart());
        std::string raw;
        for (const char key : std::string_view(keys)) {
            raw += key;
            harness.type(std::string(1, key));
            RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw));
            RAWKEY_ASSERT(harness.commits().empty());
        }
        const auto actual_readings = [&] {
            const auto& decision = harness.session()->mixed_decision;
            std::string result;
            for (const auto& segment : decision.result.paths.at(decision.preview_path).segments) {
                if (segment.kind != llavon::ime::MixedSegmentKind::Bopomofo) continue;
                if (!result.empty()) result += '|';
                result += llavon::ime::u16_to_utf8(segment.reading);
            }
            return result;
        };
        if (actual_readings() != readings) throw Failure{raw + " selected " + actual_readings()};
        const auto displayed = harness.preedit();
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, raw.size() - 1)));
        harness.type(raw.substr(raw.size() - 1));
        RAWKEY_ASSERT(actual_readings() == readings);
        RAWKEY_ASSERT(harness.preedit() == displayed);
        harness.expect_commit(displayed);
        harness.type(raw);
        const auto& decision = harness.session()->mixed_decision;
        const auto tail_begin = decision.result.paths.at(decision.preview_path).segments.back().begin;
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, tail_begin)));
        harness.reset();
        harness.type(raw);
        harness.key("Down");
        harness.choose_text(raw);
        harness.type("nefhwf");
        RAWKEY_ASSERT(harness.preedit() == raw + "你好");
        harness.expect_commit(raw + "你好");
    }
}

RAWKEY_SUITE("Hsu acronym islands cannot invent a word by changing the neighboring Chinese", hsu_acronym_islands) {
    for (const auto& [prefix, text] : std::array{
        std::pair{"nefhwf", "你好"}, std::pair{"xhfdgs", "我的"},
        std::pair{"cfulj", "使用"}, std::pair{"gl cen ", "更新"},
        std::pair{"jgjggs", "這個"}, std::pair{"gifvld", "改成"}}) {
        for (const auto token : {"id", "ed", "di", "vim"}) {
            Harness harness(hsu_smart());
            const auto raw = std::string(prefix) + token + "jljvkd";
            harness.type(raw);
            const auto expected = std::string(text) + token + "正常";
            if (harness.preedit() != expected) throw Failure{raw + " became " + harness.preedit()};
            harness.expect_commit(expected);
        }
    }
    Harness raw(hsu_smart());
    raw.type("gl cen ed");
    RAWKEY_ASSERT(raw.preedit() == "更新ed");
    raw.expect_commit("更新ed");
}

RAWKEY_SUITE("Hsu repaired phrase boundaries survive immediate and delayed wrong key undo", hsu_phrase_typo_undo) {
    for (const auto keys : {"dlfedceyjxhfjeojgxhjvuj", "kofdijlefeofleldvemd", "dlfedceyjcallxhf",
                            "vel vxdcachezijcjedaj", "uljvimdyfki dkfmj"}) {
        const std::string raw(keys);
        Harness clean(hsu_smart());
        clean.type(raw);
        const auto expected = clean.preedit();
        for (const size_t offset : {size_t{0}, raw.size() / 2, raw.size() - 1}) {
            for (const int kind : {0, 1, 2, 3, 4}) {
                if ((kind == 3 && offset + 1 >= raw.size()) || (kind == 4 && offset == 0)) continue;
                const size_t removed = kind == 4 ? 0 : kind == 3 ? 2 : 1;
                std::string replacement;
                if (kind == 0) replacement = raw[offset] == 'x' ? "n" : "x";
                if (kind == 1) replacement = std::string(2, raw[offset]);
                if (kind == 3) replacement = std::string{raw[offset + 1], raw[offset]};
                if (kind == 4) replacement = " ";
                for (const size_t lag : {size_t{0}, size_t{6}}) {
                    Harness harness(hsu_smart());
                    const auto detected = std::min(raw.size(), offset + removed + lag);
                    harness.type(raw.substr(0, offset) + replacement +
                                 raw.substr(offset + removed, detected - offset - removed));
                    RAWKEY_ASSERT(harness.commits().empty());
                    const auto erase = replacement.size() + detected - offset - removed;
                    for (size_t index = 0; index < erase; ++index) harness.key("Shift+BackSpace");
                    RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, offset)));
                    harness.type(raw.substr(offset));
                    if (harness.preedit() != expected) {
                        throw Failure{raw + ": typo kind " + std::to_string(kind) + " at " + std::to_string(offset) +
                                      " lag " + std::to_string(lag) + " restored " + harness.preedit()};
                    }
                    harness.expect_commit(expected);
                }
            }
        }
    }
}

RAWKEY_SUITE("Hsu unknown island boundaries require natural readings and keep exact spelling", hsu_unknown_island_boundaries) {
    for (const auto& [keys, text] : std::array{
        std::pair{"velfuljquuxgl cen cgjdelj", "請用quux更新設定"},
        std::pair{"jen tem uljemacsdyfki dkfmj", "今天用emacs打開檔案"},
        std::pair{"cfuljllvmel gi jljvkd", "使用llvm應該正常"},
        std::pair{"kmjkmjedbmj", "看看一半"},
        std::pair{"jen tem eddxmj", "今天一段"},
        std::pair{"edggs", "一個"}}) {
        Harness harness(hsu_smart());
        harness.type(keys);
        if (harness.preedit() != text) throw Failure{std::string(keys) + " became " + harness.preedit()};
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(keys));
        harness.expect_commit(text);
    }
}

RAWKEY_SUITE("Hsu repaired Chinese and unknown literal islands stay at the middle caret", hsu_phrase_middle_caret) {
    for (const auto& [keys, text] : std::array{
        std::pair{"dlfedceyj", "等一下"},
        std::pair{"uljvimdyfki dkfmj", "用vim打開檔案"}}) {
        Harness harness(hsu_smart());
        harness.type("nefhwf");
        harness.key("Down");
        harness.choose_text("你好");
        harness.key("Left");
        harness.type(keys);
        const auto expected = std::string("你") + text + "好";
        RAWKEY_ASSERT(harness.preedit() == expected);
        harness.key("Shift+BackSpace");
        const std::string raw(keys);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, raw.size() - 1)));
        harness.type(raw.substr(raw.size() - 1));
        RAWKEY_ASSERT(harness.preedit() == expected);
        harness.expect_commit(expected);
    }
}

RAWKEY_SUITE("Hsu structured boundary repairs are visible without changing automatic opaque intent", hsu_structured_boundary_choices) {
    for (const auto& [keys, automatic, repaired, island] : std::array{
        std::tuple{"cem byfreport.csvjejgaftxldcj", "先byfreport.csvjejgaftxldcj", "先把report.csv寄給同事", "report.csv"},
        std::tuple{"cen dgsssh_keyfkjzijjgjlef", "心dgsssh_keyfkjzijjgjlef", "心的ssh_key放在這裡", "ssh_key"}}) {
        for (const auto route : {"repair", "raw", "escape", "immediate-enter", "middle", "undo"}) {
            Harness harness(hsu_smart());
            const bool middle = std::string_view(route) == "middle";
            if (middle) {
                harness.type("nefhwf");
                harness.key("Down");
                harness.choose_text("你好");
                harness.key("Left");
            }
            harness.type(keys);
            const auto before = (middle ? std::string("你") : "") + automatic + (middle ? "好" : "");
            RAWKEY_ASSERT(harness.preedit() == before);
            RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(keys));
            const auto& decision = harness.session()->mixed_decision;
            RAWKEY_ASSERT(!decision.result.paths.at(decision.preview_path).boundary_alternative);
            const auto alternative = std::ranges::find_if(decision.result.paths, [](const auto& path) {
                return path.boundary_alternative;
            });
            RAWKEY_ASSERT(alternative != decision.result.paths.end());
            RAWKEY_ASSERT(llavon::ime::u16_to_utf8(alternative->rendered) == repaired);
            RAWKEY_ASSERT(std::ranges::count_if(alternative->segments, [&](const auto& segment) {
                return segment.kind == llavon::ime::MixedSegmentKind::Latin &&
                    llavon::ime::u16_to_utf8(segment.raw) == island;
            }) == 1);
            if (std::string_view(route) == "immediate-enter") {
                harness.expect_commit(before);
                continue;
            }
            if (std::string_view(route) == "undo") {
                harness.key("Shift+BackSpace");
                const std::string raw(keys);
                RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(raw.substr(0, raw.size() - 1)));
                harness.type(raw.substr(raw.size() - 1));
                RAWKEY_ASSERT(harness.preedit() == before);
            }
            harness.key("Down");
            RAWKEY_ASSERT(harness.candidate(0) == automatic);
            RAWKEY_ASSERT(harness.candidate(1) == keys);
            RAWKEY_ASSERT(harness.candidate(2) == repaired);
            if (std::string_view(route) == "escape") {
                harness.key("Escape");
                RAWKEY_ASSERT(harness.preedit() == before);
                RAWKEY_ASSERT(harness.session()->pending_token.raw == llavon::ime::utf8_to_u16(keys));
                harness.expect_commit(before);
            } else {
                const bool raw_choice = std::string_view(route) == "raw";
                harness.choose_text(raw_choice ? keys : repaired);
                const auto expected = raw_choice ? std::string(keys) : (middle ? std::string("你") : "") + repaired + (middle ? "好" : "");
                RAWKEY_ASSERT(harness.preedit() == expected);
                RAWKEY_ASSERT(harness.commits().empty());
                RAWKEY_ASSERT(harness.session()->pending_token.empty());
                harness.expect_commit(expected);
            }
        }
    }
}

RAWKEY_SUITE("Hsu structured repair candidates generalize across names and Chinese surroundings", hsu_structured_boundary_names) {
    for (const auto name : {"report.csv", "document.pdf", "image.png", "data.tsv", "file.txt", "client.toml",
                            "ssh_key", "api_token", "user_name", "server_port", "foo_bar", "access_token"}) {
        for (const auto& [prefix, suffix, readings] : std::array{
            std::tuple{"cem byf", "jejgaftxldcj", "ㄒㄧㄢ |ㄅㄚˇ|ㄐㄧˋ|ㄍㄟˇ|ㄊㄨㄥˊ|ㄕˋ"},
            std::tuple{"velfbyf", "fkjzijjgjlef", "ㄑㄧㄥˇ|ㄅㄚˇ|ㄈㄤˋ|ㄗㄞˋ|ㄓㄜˋ|ㄌㄧˇ"},
            std::tuple{"cfulj", "el gi jljvkd", "ㄕˇ|ㄩㄥˋ|ㄧㄥ |ㄍㄞ |ㄓㄥˋ|ㄔㄤˊ"},
            std::tuple{"gl cen ", "aidhxajjljvkd", "ㄍㄥ |ㄒㄧㄣ |ㄘㄞˊ|ㄏㄨㄟˋ|ㄓㄥˋ|ㄔㄤˊ"},
            std::tuple{"jgjggs", "cu ewjgl cen ", "ㄓㄜˋ|ㄍㄜ˙|ㄒㄩ |ㄧㄠˋ|ㄍㄥ |ㄒㄧㄣ "}}) {
            auto options = hsu_smart();
            options.config.candidate_page_size = 3;
            Harness harness(options);
            const auto raw = std::string(prefix) + name + suffix;
            harness.type(raw);
            const auto& decision = harness.session()->mixed_decision;
            RAWKEY_ASSERT(!decision.result.paths.at(decision.preview_path).boundary_alternative);
            const auto repaired = std::ranges::find_if(decision.result.paths, [](const auto& path) {
                return path.boundary_alternative;
            });
            RAWKEY_ASSERT(repaired != decision.result.paths.end());
            std::string joined, covered;
            size_t literal_count = 0;
            for (const auto& segment : repaired->segments) {
                covered += llavon::ime::u16_to_utf8(segment.raw);
                if (segment.kind == llavon::ime::MixedSegmentKind::Bopomofo) {
                    if (!joined.empty()) joined += '|';
                    joined += llavon::ime::u16_to_utf8(segment.reading);
                } else {
                    ++literal_count;
                    RAWKEY_ASSERT(segment.kind == llavon::ime::MixedSegmentKind::Latin);
                    RAWKEY_ASSERT(segment.raw == llavon::ime::utf8_to_u16(name));
                    RAWKEY_ASSERT(segment.begin == std::string_view(prefix).size());
                }
            }
            RAWKEY_ASSERT(literal_count == 1);
            RAWKEY_ASSERT(covered == raw);
            RAWKEY_ASSERT(joined == readings);
            const auto expected = llavon::ime::u16_to_utf8(repaired->rendered);
            const auto automatic = harness.preedit();
            harness.key("Down");
            RAWKEY_ASSERT(harness.candidate(0) == automatic);
            const size_t repair_row = automatic == raw ? 1 : 2;
            RAWKEY_ASSERT(harness.candidate(automatic == raw ? 0 : 1) == raw);
            RAWKEY_ASSERT(harness.candidate(repair_row) == expected);
            harness.key(repair_row == 1 ? "2" : "3");
            RAWKEY_ASSERT(harness.preedit() == expected);
            RAWKEY_ASSERT(harness.commits().empty());
            harness.expect_commit(expected);
        }
    }
}

// SmartEnglish on the Hsu (許氏) layout: lowercase letters are held as a raw
// pending word; the Hsu tone keys d/f/j/s decide Chinese (replay as 注音),
// space decides Chinese (first-tone reading) or English (word + space).
RAWKEY_SUITE("smart hsu", engine_test_smart_hsu) {
    // 1. A known Latin token stays raw; Down exposes its Hsu interpretation.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("h");
        RAWKEY_ASSERT(harness.preedit() == "h");
        harness.key(Key('d'));
        RAWKEY_ASSERT(harness.preedit() == "hd");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "hd");
        RAWKEY_ASSERT(harness.candidate(1) == "哦");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "哦");
}
    // 2. Pending letters render raw until the tone key: "ne" is NOT ㄋㄧ.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("ne");
        RAWKEY_ASSERT(harness.preedit() == "ne");
        harness.key(Key('f'));
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    // 3. Tone keys f/j/s decide Chinese: hw+f -> 好, xh+f -> 我.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hw");
        RAWKEY_ASSERT(harness.preedit() == "hw");
        harness.key(Key('f'));
        RAWKEY_ASSERT(harness.preedit() == "好");
}
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("xh");
        harness.key(Key('f'));
        RAWKEY_ASSERT(harness.preedit() == "我");
}
    // 4. Hsu first tone via space: gen + space -> 今 (ㄐㄧㄣ first tone).
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("gen");
        RAWKEY_ASSERT(harness.preedit() == "gen");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
}
    // 5. Known English wins even when the keys also form a Hsu reading;
    //    Down exposes the Chinese alternative before Space commits English.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("hi");
        RAWKEY_ASSERT(harness.preedit() == "hi");
        harness.key("space");
        RAWKEY_ASSERT(harness.preedit() == "hi ");
        RAWKEY_ASSERT(harness.commits().empty());
        harness.expect_commit("hi ");
        RAWKEY_ASSERT(harness.preedit().empty());
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_space_then_commit("hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("thank");
        harness.expect_space_then_commit("thank ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // 6. Hsu mixed: 你 via nef, then English hello + space commits 你hello .
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("hello");
        harness.expect_space_then_commit("你hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // 7. A Hsu tone-looking letter remains part of the English token when the
    //    combined sequence is not a valid reading.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(Key('d'));
        RAWKEY_ASSERT(harness.preedit() == "hellod");
        harness.expect_space_then_commit("hellod ");
}
    // 8. Backspace pops one pending char at a time.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("ne");
        RAWKEY_ASSERT(harness.preedit() == "ne");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "n");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // 9. A tone key with nothing pending starts a pending word.
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.key(Key('d'));
        RAWKEY_ASSERT(harness.preedit() == "d");
}
    // 10. Hsu Chinese then English: hd -> 哦, then hello + space -> 哦hello .
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.set_config("SmartEnglish", "True");
        harness.type("hd");
        harness.key(Key("Down"));
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "哦");
        harness.type("hello");
        harness.expect_space_then_commit("哦hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Consecutive letter-tone syllables stay Chinese, and an English
    // inflection can follow without a mode switch or lost Chinese prefix.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("hwf");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.type("xhf");
        RAWKEY_ASSERT(harness.preedit() == "你好我");
        harness.expect_commit("你好我");
        harness.type("nef");
        harness.key("space");
        harness.type("adds");
        RAWKEY_ASSERT(harness.preedit() == "你adds");
        harness.expect_space_then_commit("你adds ");
    }
    // A recognized inflection still has reversible Chinese alternatives.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("added");
        RAWKEY_ASSERT(harness.preedit() == "added");
        harness.key("Down");
        RAWKEY_ASSERT(harness.candidate(0) == "added");
        RAWKEY_ASSERT(harness.candidate_count() > 1);
        harness.key("Escape");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "adde");
        harness.type("d");
        RAWKEY_ASSERT(harness.preedit() == "added");
        harness.expect_commit("added");
    }
}

#include "raw_key_harness.hpp"
#include "scripted_service.hpp"
#include "bopomofo/table_engine.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {
struct LayoutFixture {
    std::string_view name;
    std::string_view keys;
    char neighboring_initial;
};

// Direct layouts follow the published symbol order, independent of the
// engine's lookup. Compact phrases below include their contextual keys/cycles.
constexpr std::u16string_view reference_symbols =
    u"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ ˊˇˋ˙";
constexpr std::array layouts{
    LayoutFixture{"standard", "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347", 'a'},
    LayoutFixture{"hsu", "", 'b'},
    LayoutFixture{"ibm", "1234567890-qwertyuiopasdfghjkl;zxcvbn m,./", '6'},
    LayoutFixture{"et", "bpmfdtnlvkhg7c,./j;'sexuaorwiqzy890-= 2341", 'b'},
    LayoutFixture{"ginyieh", "2wsx3edcrfvtgb6yhnujm-['8ik,9ol.0p;/= qaz1", 's'},
    LayoutFixture{"et26", "", 'b'},
    LayoutFixture{"dachen_cp26", "", 'a'},
};

struct PhraseFixture {
    std::string_view label;
    std::array<std::u16string_view, 2> readings;
    // Hsu, ET26, CP26, respectively. First-tone confirmation spaces are part
    // of the full spelling, while shortened spellings omit every tone key.
    std::array<std::string_view, 3> compact_full;
    std::array<std::string_view, 3> compact_short;
};
constexpr std::array phrases{
    PhraseFixture{"你好", {u"ㄋㄧˇ", u"ㄏㄠˇ"},
                  {"nefhwf", "nejhzj", "surclr"}, {"nehw", "nehz", "sucl"}},
    PhraseFixture{"今天", {u"ㄐㄧㄣ ", u"ㄊㄧㄢ "},
                  {"jen tem ", "gen tem ", "rup wwuoo "}, {"jentem", "gentem", "rupwwuoo"}},
    PhraseFixture{"開會", {u"ㄎㄞ ", u"ㄏㄨㄟˋ"},
                  {"ki hxaj", "ki hxqk", "dii cjod"}, {"kihxa", "kihxq", "diicjo"}},
    PhraseFixture{"我要", {u"ㄨㄛˇ", u"ㄧㄠˋ"},
                  {"xhfewj", "xojezk", "jiruld"}, {"xhew", "xoez", "jiul"}},
    PhraseFixture{"謝謝", {u"ㄒㄧㄝˋ", u"ㄒㄧㄝˋ"},
                  {"ceejceej", "cewkcewk", "vubdvubd"}, {"ceecee", "cewcew", "vubvub"}},
    PhraseFixture{"測試", {u"ㄘㄜˋ", u"ㄕˋ"},
                  {"agjcj", "wrkck", "hkdgd"}, {"agc", "wrc", "hkg"}},
};

bool tone_symbol(char16_t symbol) {
    return symbol == u' ' || symbol == u'ˊ' || symbol == u'ˇ' || symbol == u'ˋ' || symbol == u'˙';
}

std::string spelling(const LayoutFixture& layout, const PhraseFixture& phrase, bool shortened) {
    if (layout.keys.empty()) {
        const std::size_t compact_index = layout.name == "hsu" ? 0 : layout.name == "et26" ? 1 : 2;
        return std::string(shortened ? phrase.compact_short[compact_index] : phrase.compact_full[compact_index]);
    }
    RAWKEY_ASSERT(layout.keys.size() == reference_symbols.size());
    std::string raw;
    for (const auto reading : phrase.readings) {
        for (const auto symbol : reading) {
            if (shortened && tone_symbol(symbol)) continue;
            const auto position = reference_symbols.find(symbol);
            RAWKEY_ASSERT(position != std::u16string_view::npos);
            raw += layout.keys[position];
        }
    }
    return raw;
}

HarnessOptions typing_options(std::string_view layout) {
    HarnessOptions result;
    result.config.keyboard_layout = layout;
    result.config.smart_english = true;
    return result;
}

void enable_typing(Harness& harness) {
    harness.set_config("SmartTyping", "True");
    RAWKEY_ASSERT(harness.config().smart_typing);
}

std::string phrase_signature(const PhraseFixture& phrase) {
    std::string signature;
    for (const auto reading : phrase.readings) signature += "<" + u16_to_utf8(reading) + ">";
    return signature;
}

std::string path_signature(const MixedPath& path) {
    std::string signature;
    std::u16string literal;
    const auto flush_literal = [&] {
        if (literal.empty()) return;
        signature += "[" + u16_to_utf8(literal) + "]";
        literal.clear();
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

void expect_preview_signature(const Harness& harness, std::string_view expected,
                              std::string_view layout, std::string_view raw) {
    const auto& decision = harness.session()->mixed_decision;
    RAWKEY_ASSERT(decision.active());
    RAWKEY_ASSERT(decision.preview_path < decision.result.paths.size());
    const auto actual = path_signature(decision.result.paths[decision.preview_path]);
    if (actual != expected) {
        throw Failure{std::string(layout) + " raw [" + std::string(raw) + "]: expected " +
                      std::string(expected) + ", got " + actual + " (" + harness.preedit() + ")"};
    }
}

std::u16string reading_body(std::u16string_view reading) {
    std::u16string body;
    for (const auto symbol : reading) {
        if (!tone_symbol(symbol)) body += symbol;
    }
    return body;
}

template <typename ReadingRange>
void expect_shortened_phrase(const Harness& harness, const ReadingRange& expected_readings,
                             std::string_view expected_text, std::string_view layout,
                             std::string_view raw) {
    const auto& decision = harness.session()->mixed_decision;
    RAWKEY_ASSERT(decision.active());
    RAWKEY_ASSERT(decision.preview_path < decision.result.paths.size());
    const auto& path = decision.result.paths[decision.preview_path];
    const auto characters = utf8_to_u32(expected_text);
    RAWKEY_ASSERT(characters.size() == expected_readings.size());
    static const TableEngine table(LLAVON_IME_TEST_TABLE_PATH);
    std::size_t index = 0;
    for (const auto& segment : path.segments) {
        if (segment.consumed_boundary) continue;
        if (segment.kind != MixedSegmentKind::Bopomofo || index >= expected_readings.size() ||
            reading_body(segment.reading) != reading_body(expected_readings[index])) {
            throw Failure{std::string(layout) + " shortened raw [" + std::string(raw) +
                          "]: unexpected syllable body in " + path_signature(path) +
                          " (" + harness.preedit() + ")"};
        }
        // The user supplied no tone. A legal alternate reading of the same
        // character (會/要, for example) must retain its phonetic body and text.
        const auto canonical_candidates = table.lookup(expected_readings[index]);
        const auto inferred_candidates = table.lookup(segment.reading);
        RAWKEY_ASSERT(std::ranges::find(canonical_candidates, characters[index]) != canonical_candidates.end());
        RAWKEY_ASSERT(std::ranges::find(inferred_candidates, characters[index]) != inferred_candidates.end());
        ++index;
    }
    RAWKEY_ASSERT(index == expected_readings.size());
    if (harness.preedit() != expected_text) {
        throw Failure{std::string(layout) + " shortened raw [" + std::string(raw) +
                      "]: expected text " + std::string(expected_text) + ", got " + harness.preedit()};
    }
}

void confirm_without_submit(Harness& harness, std::string_view raw) {
    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
    RAWKEY_ASSERT(harness.commits().empty());
    harness.key("space");
    RAWKEY_ASSERT(harness.commits().empty());
    RAWKEY_ASSERT(!harness.composition_empty());
    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(std::string(raw) + " "));
}

void submit_visible_preview(Harness& harness) {
    const auto shown = harness.preedit();
    RAWKEY_ASSERT(!shown.empty());
    harness.expect_direct_commit(shown, Key("Return"));
    RAWKEY_ASSERT(harness.commits().size() == 1);
    RAWKEY_ASSERT(harness.composition_empty());
}

std::vector<std::pair<std::string, std::string>> noisy_spellings(const LayoutFixture& layout) {
    const auto full = spelling(layout, phrases.front(), false);
    auto insertion = full;
    // CP26 repeats of u intentionally cycle ㄧ to ㄚ. Repeating the initial
    // exercises an accidental extra key without changing a valid native word.
    insertion.insert(1, 1, layout.name == "dachen_cp26" ? full[0] : full[1]);
    auto deletion = full;
    deletion.erase(1, 1);
    auto substitution = full;
    substitution[0] = layout.neighboring_initial;
    auto transposition = full;
    std::swap(transposition[0], transposition[1]);
    return {{"extra key", std::move(insertion)}, {"missing key", std::move(deletion)},
            {"neighboring key", std::move(substitution)}, {"swapped keys", std::move(transposition)}};
}

std::string wrong_tone_spelling(const LayoutFixture& layout) {
    auto raw = spelling(layout, phrases.front(), false);
    char second_tone = 0;
    if (layout.keys.empty()) {
        second_tone = layout.name == "hsu" ? 'd' : layout.name == "et26" ? 'f' : 'e';
    } else {
        second_tone = layout.keys[reference_symbols.find(u'ˊ')];
    }
    // All independently validated 你好 fixtures have two body keys followed
    // by their tone. Change only the first syllable's explicit tone.
    raw[2] = second_tone;
    return raw;
}

void choose_candidate_across_pages(Harness& harness, std::string_view text) {
    for (unsigned page = 0; page < 16; ++page) {
        const auto candidates = harness.candidates();
        if (std::ranges::find(candidates, text) != candidates.end()) {
            harness.choose_text(text);
            return;
        }
        const auto before = harness.render_page();
        harness.key("Page_Down");
        if (harness.render_page() == before) break;
    }
    throw Failure{"candidate unavailable across pages: " + std::string(text)};
}
}  // namespace

RAWKEY_SUITE("smart typing physical phrase reference", smart_typing_physical_reference) {
    for (const auto& layout : layouts) {
        for (const auto& phrase : phrases) {
            HarnessOptions options;
            options.config.keyboard_layout = layout.name;
            Harness harness(options);
            harness.type(spelling(layout, phrase, false));
            const auto& segments = harness.session()->buffer.segments();
            RAWKEY_ASSERT(segments.size() == phrase.readings.size());
            for (std::size_t index = 0; index < phrase.readings.size(); ++index) {
                if (segments[index].reading() != phrase.readings[index]) {
                    throw Failure{std::string(layout.name) + " physical reference " + std::string(phrase.label)};
                }
            }
        }
        HarnessOptions options;
        options.config.keyboard_layout = layout.name;
        Harness wrong_tone(options);
        wrong_tone.type(wrong_tone_spelling(layout));
        RAWKEY_ASSERT(wrong_tone.session()->buffer.segments().size() == 2);
        RAWKEY_ASSERT(wrong_tone.session()->buffer.segments().front().reading() == u"ㄋㄧˊ");
        RAWKEY_ASSERT(wrong_tone.session()->buffer.segments().back().reading() == u"ㄏㄠˇ");
    }
}

RAWKEY_SUITE("smart typing omitted tones recover complete phrases across layouts", smart_typing_omitted_tones) {
    for (const auto& layout : layouts) {
        for (const auto& phrase : phrases) {
            const auto raw = spelling(layout, phrase, true);
            RAWKEY_ASSERT(raw.size() < spelling(layout, phrase, false).size());
            Harness harness(typing_options(layout.name));
            enable_typing(harness);
            harness.type(raw);
            confirm_without_submit(harness, raw);
            expect_shortened_phrase(harness, phrase.readings, phrase.label, layout.name, raw);
            submit_visible_preview(harness);
        }
    }
}

RAWKEY_SUITE("smart typing submits shortened words with fewer keys and no candidate navigation", smart_typing_direct_submit) {
    for (const auto& layout : layouts) {
        const auto raw = spelling(layout, phrases.front(), true);
        const auto complete = spelling(layout, phrases.front(), false);
        // Short body + Enter replaces full body/tones + Enter. Selection or an
        // additional confirmation key must not erase the saved keystrokes.
        RAWKEY_ASSERT(raw.size() + 1 < complete.size() + 1);
        Harness harness(typing_options(layout.name));
        enable_typing(harness);
        harness.type(raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        RAWKEY_ASSERT(!harness.has_candidates());
        expect_preview_signature(harness, phrase_signature(phrases.front()), layout.name, raw);
        submit_visible_preview(harness);
    }
}

RAWKEY_SUITE("smart typing can shorten Chinese without smart English", smart_typing_chinese_only) {
    Harness harness(typing_options("standard"));
    harness.set_config("SmartEnglish", "False");
    enable_typing(harness);
    harness.type("sucl");
    RAWKEY_ASSERT(harness.session()->pending_token.raw == u"sucl");
    RAWKEY_ASSERT(!harness.has_candidates());
    expect_preview_signature(harness, phrase_signature(phrases.front()), "standard", "sucl");
    submit_visible_preview(harness);
    // Enabling shorter input does not change an ordinary, fully toned Chinese
    // reading into an English/alphanumeric interpretation.
    harness.type("su3cl3");
    RAWKEY_ASSERT(harness.preedit() == "你好");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(harness.commits().size() == 2);
    RAWKEY_ASSERT(harness.composition_empty());
}

RAWKEY_SUITE("smart typing one wrong missing extra or swapped key recovers a phrase", smart_typing_single_key_errors) {
    for (const auto& layout : layouts) {
        for (const auto& [error, raw] : noisy_spellings(layout)) {
            Harness harness(typing_options(layout.name));
            enable_typing(harness);
            harness.type(raw);
            confirm_without_submit(harness, raw);
            expect_preview_signature(harness, phrase_signature(phrases.front()), layout.name,
                                     error + ": " + raw);
            if (harness.preedit() != phrases.front().label) {
                throw Failure{std::string(layout.name) + " " + error + " raw [" + raw +
                              "]: expected 你好, got " + harness.preedit()};
            }
            submit_visible_preview(harness);
        }
    }
}

RAWKEY_SUITE("smart typing corrects one explicit wrong tone across layouts", smart_typing_wrong_tones) {
    for (const auto& layout : layouts) {
        const auto raw = wrong_tone_spelling(layout);
        Harness harness(typing_options(layout.name));
        enable_typing(harness);
        harness.type(raw);
        confirm_without_submit(harness, raw);
        expect_preview_signature(harness, phrase_signature(phrases.front()), layout.name, raw);
        if (harness.preedit() != phrases.front().label) {
            throw Failure{std::string(layout.name) + " wrong tone raw [" + raw +
                          "]: expected 你好, got " + harness.preedit()};
        }
        submit_visible_preview(harness);
    }
}

RAWKEY_SUITE("smart typing initial shortcuts offer Chinese candidates across layouts", smart_typing_initial_shortcuts) {
    constexpr std::array initials{"sc", "nh", "7-", "nh", "dv", "nh", "sc"};
    for (std::size_t index = 0; index < layouts.size(); ++index) {
        const auto& layout = layouts[index];
        const std::string raw = initials[index];
        for (const auto& chosen : {std::string("你好"), raw}) {
            Harness harness(typing_options(layout.name));
            enable_typing(harness);
            harness.type(raw);
            if (harness.preedit() != raw) {
                throw Failure{std::string(layout.name) + " abbreviated raw [" + raw +
                              "] was automatically changed to " + harness.preedit()};
            }
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
            harness.key("Down");
            RAWKEY_ASSERT(harness.has_candidates());
            RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
            try {
                choose_candidate_across_pages(harness, chosen);
            } catch (const Failure& failure) {
                throw Failure{std::string(layout.name) + " initial raw [" + raw + "]: " + failure.message};
            }
            RAWKEY_ASSERT(harness.commits().empty());
            harness.expect_direct_commit(chosen, Key("Return"));
        }
    }
}

RAWKEY_SUITE("smart typing preserves intentional compact cycles and offers a correction", smart_typing_compact_cycle_ambiguity) {
    constexpr std::string_view raw = "suurclr";
    for (const auto& chosen : {std::string("你好"), std::string(raw) + " "}) {
        Harness harness(typing_options("dachen_cp26"));
        enable_typing(harness);
        harness.type(raw);
        confirm_without_submit(harness, raw);
        // CP26 uu means ㄚ, so these exact keys legitimately spell 那好.
        // A possible accidental repeated u is an alternative, not certainty.
        RAWKEY_ASSERT(harness.preedit() == "那好");
        expect_preview_signature(harness, "<ㄋㄚˇ><ㄏㄠˇ>", "dachen_cp26", raw);
        harness.key("Down");
        choose_candidate_across_pages(harness, chosen);
        RAWKEY_ASSERT(harness.commits().empty());
        harness.expect_direct_commit(chosen, Key("Return"));
    }
}

RAWKEY_SUITE("smart typing inferred readings are excluded from training truth", smart_typing_training_provenance) {
    std::vector<InputEffect::CommitSample> samples;
    auto options = typing_options("standard");
    options.on_training_commit = [&](const auto& sample, std::u16string_view) {
        samples.push_back(sample);
    };
    Harness harness(options);
    enable_typing(harness);

    harness.type("sucl");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(samples.empty());

    harness.type("sucl");
    harness.key("Down");
    choose_candidate_across_pages(harness, "你好");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(samples.empty());

    harness.type("sucl");
    harness.key("Left");
    RAWKEY_ASSERT(harness.preedit() == "你好");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(samples.empty());

    harness.type("au3cl3");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(samples.empty());

    harness.type("su3cl3");
    harness.expect_direct_commit("你好", Key("Return"));
    RAWKEY_ASSERT(samples.size() == 1);
    RAWKEY_ASSERT(samples.front().answer == u"你好");
    RAWKEY_ASSERT(samples.front().entries.size() == 2);
    for (std::size_t index = 0; index < phrases.front().readings.size(); ++index) {
        const auto& entry = samples.front().entries[index];
        RAWKEY_ASSERT(entry.reading == phrases.front().readings[index]);
        RAWKEY_ASSERT(entry.character == (index == 0 ? U'你' : U'好'));
        RAWKEY_ASSERT(!entry.literal);
    }
}

RAWKEY_SUITE("smart typing restores a complete shortened sentence with one wrong key", smart_typing_shortened_sentences) {
    const auto& layout = layouts.front();
    const auto today = spelling(layout, phrases[1], true);
    const auto want = spelling(layout, phrases[3], true);
    const auto meeting = spelling(layout, phrases[2], true);
    const auto shortened = today + want + meeting;
    const auto full = spelling(layout, phrases[1], false) + spelling(layout, phrases[3], false) +
                      spelling(layout, phrases[2], false);
    const std::array expected_readings{
        phrases[1].readings[0], phrases[1].readings[1],
        phrases[3].readings[0], phrases[3].readings[1],
        phrases[2].readings[0], phrases[2].readings[1],
    };
    RAWKEY_ASSERT(shortened.size() + 1 < full.size() + 1);
    auto wrong_key = shortened;
    wrong_key[today.size()] = 'h'; // j -> h: adjacent physical keys in 我要.
    for (const auto& raw : {shortened, wrong_key}) {
        Harness harness(typing_options(layout.name));
        enable_typing(harness);
        harness.type(raw);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(raw));
        RAWKEY_ASSERT(!harness.has_candidates());
        expect_shortened_phrase(harness, expected_readings, "今天我要開會", layout.name, raw);
        const auto& decision = harness.session()->mixed_decision;
        unsigned edits = 0;
        for (const auto& segment : decision.result.paths[decision.preview_path].segments) {
            edits += segment.recovery_edits;
        }
        RAWKEY_ASSERT(edits <= 1);
        submit_visible_preview(harness);
    }
}

RAWKEY_SUITE("smart typing Chinese-only incomplete readings keep native editing", smart_typing_native_incomplete_editing) {
    {
        auto options = typing_options("standard");
        options.config.smart_english = false;
        Harness harness(options);
        enable_typing(harness);
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        harness.key("Left");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        RAWKEY_ASSERT(harness.session()->buffer.has_unfinished_reading());
        RAWKEY_ASSERT(harness.session()->buffer.segments().size() == 1);
        RAWKEY_ASSERT(harness.session()->buffer.segments().front().literal == 0);
        RAWKEY_ASSERT(harness.session()->buffer.segments().front().reading() == u"ㄋㄧ");
        harness.key("Right");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        RAWKEY_ASSERT(harness.commits().empty());
    }
    {
        auto options = typing_options("dachen_cp26");
        options.config.smart_english = false;
        Harness harness(options);
        enable_typing(harness);
        harness.type("uu");
        RAWKEY_ASSERT(harness.preedit() == "ㄚ");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == u"uu");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "ㄧ");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == u"u");
        harness.type("u");
        RAWKEY_ASSERT(harness.preedit() == "ㄚ");
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "ㄧ");
        RAWKEY_ASSERT(harness.commits().empty());
    }
}

RAWKEY_SUITE("smart typing Chinese-only model replies preserve an unfinished phonetic tail", smart_typing_native_incomplete_model) {
    std::atomic_bool closed = false;
    test::ScriptedService server([&](int fd, int) {
        test::ScriptedService::handshake(fd, true);
        protocol::SessionId id{};
        id[0] = 31;
        for (;;) {
            const auto message = test::ScriptedService::receive(fd);
            if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                test::ScriptedService::send(fd, protocol::OpenSessionResponse{id, {}});
            } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                RAWKEY_ASSERT(request->padding.size() == 1);
                RAWKEY_ASSERT(request->padding.front().bopomofo() == u"ㄋㄧˇ");
                test::ScriptedService::send(fd, protocol::Prediction{
                    request->session_id, request->request_id, request->buffer_revision, {{U'擬'}}});
            } else if (const auto* close = std::get_if<protocol::CloseSessionRequest>(&message)) {
                test::ScriptedService::send(fd, protocol::CloseSessionResponse{close->session_id, true});
                closed = true;
                return;
            } else {
                RAWKEY_ASSERT(false);
            }
        }
    });
    auto options = typing_options("standard");
    options.config.smart_english = false;
    options.config.smart_model_preview = true;
    options.socket_path = server.path().string();
    Harness harness(options);
    enable_typing(harness);
    harness.type("su3su");
    RAWKEY_ASSERT(harness.preedit().ends_with("ㄋㄧ"));
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.preedit() == "擬ㄋㄧ"; }));
    RAWKEY_ASSERT(harness.session()->pending_token.raw == u"su3su");
    RAWKEY_ASSERT(harness.commits().empty());
    harness.detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!closed.load() && std::chrono::steady_clock::now() < deadline) {
        (void)harness.pump_until([&] { return closed.load(); }, std::chrono::milliseconds(20));
    }
    RAWKEY_ASSERT(closed.load());
    RAWKEY_ASSERT(server.ok());
}

RAWKEY_SUITE("smart typing recovery stays reversible and visible deletion removes characters", smart_typing_reversible_edits) {
    for (const auto& layout : layouts) {
        const auto raw = spelling(layout, phrases.front(), true);
        Harness undo(typing_options(layout.name));
        enable_typing(undo);
        undo.type(raw);
        confirm_without_submit(undo, raw);
        expect_preview_signature(undo, phrase_signature(phrases.front()), layout.name, raw);
        undo.key("Shift+BackSpace");
        RAWKEY_ASSERT(undo.session()->pending_token.raw == utf8_to_u16(raw));
        undo.key("Shift+BackSpace");
        RAWKEY_ASSERT(undo.session()->pending_token.raw == utf8_to_u16(raw.substr(0, raw.size() - 1)));
        undo.type(raw.substr(raw.size() - 1));
        confirm_without_submit(undo, raw);
        expect_preview_signature(undo, phrase_signature(phrases.front()), layout.name, raw);
        submit_visible_preview(undo);

        Harness erase(typing_options(layout.name));
        enable_typing(erase);
        erase.type(raw);
        confirm_without_submit(erase, raw);
        const auto shown = erase.preedit();
        erase.key("BackSpace");
        RAWKEY_ASSERT(erase.preedit() == u16_to_utf8(utf8_to_u16(shown).substr(0, 1)));
        RAWKEY_ASSERT(erase.commits().empty());
        submit_visible_preview(erase);

        Harness literal(typing_options(layout.name));
        enable_typing(literal);
        literal.type(raw);
        literal.key("space");
        literal.key("Down");
        RAWKEY_ASSERT(literal.has_candidates());
        literal.choose_text(raw + " ");
        RAWKEY_ASSERT(literal.preedit() == raw + " ");
        literal.expect_direct_commit(raw + " ", Key("Return"));
    }
}

RAWKEY_SUITE("smart typing shortened Chinese coexists with English literal islands", smart_typing_mixed_input) {
    for (const auto& layout : layouts) {
        const auto raw = "hello " + spelling(layout, phrases.front(), true);
        Harness harness(typing_options(layout.name));
        enable_typing(harness);
        harness.type(raw);
        confirm_without_submit(harness, raw);
        expect_preview_signature(harness, "[hello ]" + phrase_signature(phrases.front()), layout.name, raw);
        harness.type("json");
        harness.key("space");
        RAWKEY_ASSERT(harness.commits().empty());
        expect_preview_signature(harness, "[hello ]" + phrase_signature(phrases.front()) + "[ json ]",
                                 layout.name, raw + " json ");
        submit_visible_preview(harness);
    }
}

RAWKEY_SUITE("smart typing protects known English and complete URLs", smart_typing_english_protection) {
    constexpr std::array literals{
        "hello world", "python", "json", "async", "clang", "kubernetes", "testing", "protobuf", "pnpm",
        "https://example.org/api?q=json", "dev@example.com", "user_name42",
    };
    for (const auto& layout : layouts) {
        for (const auto literal : literals) {
            Harness harness(typing_options(layout.name));
            enable_typing(harness);
            harness.type(literal);
            RAWKEY_ASSERT(harness.preedit() == literal);
            harness.key("space");
            RAWKEY_ASSERT(harness.commits().empty());
            RAWKEY_ASSERT(harness.preedit() == std::string(literal) + " ");
            harness.expect_direct_commit(std::string(literal) + " ", Key("Return"));
        }
    }
}

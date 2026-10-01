#include "raw_key_harness.hpp"

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
    HarnessOptions result;
    result.config.smart_english = true;
    result.config.keyboard_layout = layout;
    return result;
}
} // namespace

RAWKEY_SUITE("smart cursor boundaries escape cancels only the new insertion", smart_cursor_cancel) {
    for (const auto& [layout, keys] : layouts) {
        for (const bool complete : {false, true}) {
            Harness harness(smart(layout));
            harness.type(keys);
            harness.key("Left");
            const auto inserted = complete ? std::string(keys) : std::string(keys).substr(0, 2);
            harness.type(inserted);
            if (complete) {
                harness.key("Down");
                harness.key("Escape");
                RAWKEY_ASSERT(harness.preedit() == "你你好好");
                RAWKEY_ASSERT(!harness.has_candidates());
                harness.key("Escape");
                RAWKEY_ASSERT(harness.preedit() == "你" + inserted + "好");
            }
            harness.key("Escape");
            RAWKEY_ASSERT(harness.preedit() == "你好");
            RAWKEY_ASSERT(harness.session()->pending_token.empty());
            RAWKEY_ASSERT(harness.engine().render_state(1).caret == 1);
            harness.type(keys);
            harness.expect_commit("你你好好");
        }
    }
}

RAWKEY_SUITE("smart cursor boundaries typing cancels a mark without moving the insertion", smart_cursor_mark_input) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Shift+Left");
        RAWKEY_ASSERT(harness.session()->buffer.marked_text() == u"好");
        RAWKEY_ASSERT(harness.session()->buffer.caret() == 1);
        harness.type(keys);
        RAWKEY_ASSERT(harness.preedit() == "你你好好");
        RAWKEY_ASSERT(!harness.session()->buffer.marked_range());
        harness.key("Escape");
        RAWKEY_ASSERT(harness.preedit() == "你" + std::string(keys) + "好");
        harness.key("Escape");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        RAWKEY_ASSERT(harness.engine().render_state(1).caret == 1);
        harness.expect_commit("你好");
    }
}

RAWKEY_SUITE("smart cursor boundaries successive edits inside an English island", smart_cursor_repeated_edits) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        harness.type("hello");
        harness.key("Left");
        harness.key("BackSpace");
        harness.key("Delete");
        RAWKEY_ASSERT(harness.preedit() == "你hel好");
        harness.type(keys);
        RAWKEY_ASSERT(harness.preedit() == "你hel你好好");
        harness.key("Left");
        harness.type("world");
        RAWKEY_ASSERT(harness.preedit() == "你hel你world好好");
        RAWKEY_ASSERT(harness.commits().empty());
        harness.expect_commit("你hel你world好好");
    }
}

RAWKEY_SUITE("smart cursor boundaries unfinished insertion remains raw after navigation", smart_cursor_unfinished_nav) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        const auto raw = std::string(keys).substr(0, 2);
        harness.type(raw);
        RAWKEY_ASSERT(harness.preedit() == "你" + raw + "好");
        harness.key("Right");
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        RAWKEY_ASSERT(harness.preedit() == "你" + raw + "好");
        harness.key("Left");
        harness.key("BackSpace");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.expect_commit("你好");
    }
}

RAWKEY_SUITE("smart cursor boundaries punctuation keypad and symbols stay before the suffix", smart_cursor_symbols) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type(keys);
        harness.key("Left");
        harness.type(keys);
        harness.key("KP_1");
        harness.key("Control+comma");
        if (harness.preedit() != "你你好1，好") {
            throw Failure{std::string(layout) + ": keypad/punctuation insertion: " + harness.preedit()};
        }
        harness.key("grave");
        RAWKEY_ASSERT(harness.has_candidates());
        harness.choose_text("…");
        RAWKEY_ASSERT(harness.preedit() == "你你好1，…好");
        harness.expect_commit("你你好1，…好");
    }
}

RAWKEY_SUITE("smart cursor boundaries modified punctuation keeps its explicit meaning", smart_cursor_ctrl_punctuation) {
    for (const auto& [layout, keys] : layouts) {
        for (const auto& [key, punctuation] : std::array{
            std::pair{"Control+comma", "，"}, std::pair{"Control+period", "。"},
            std::pair{"Control+slash", "？"}, std::pair{"Control+semicolon", "；"}}) {
            for (const bool pending : {false, true}) {
                Harness harness(smart(layout));
                harness.type(keys);
                harness.key("Left");
                if (pending) harness.type(keys);
                harness.key(key);
                const auto expected = std::string(pending ? "你你好" : "你") + punctuation + "好";
                RAWKEY_ASSERT(harness.preedit() == expected);
                RAWKEY_ASSERT(harness.session()->pending_token.empty());
                harness.expect_commit(expected);
            }
        }
    }
}

RAWKEY_SUITE("smart cursor boundaries settings changes preserve exact pending raw in place", smart_cursor_config_change) {
    for (const auto setting : {"layout", "smart"}) {
        Harness harness(smart("standard"));
        harness.type("su3cl3");
        harness.key("Left");
        harness.type("su");
        if (std::string_view(setting) == "layout") {
            harness.set_config("BopomofoKeyboardLayout", "hsu");
        } else {
            harness.set_config("SmartEnglish", "False");
        }
        RAWKEY_ASSERT(harness.preedit() == "你su好");
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        RAWKEY_ASSERT(harness.session()->buffer.caret() == 3);
        harness.type(std::string_view(setting) == "layout" ? "nefhwf" : "su3cl3");
        RAWKEY_ASSERT(harness.preedit() == "你su你好好");
        harness.expect_commit("你su你好好");
    }
}

RAWKEY_SUITE("smart cursor boundaries shortcuts and release events do not consume insertion keys", smart_cursor_shortcuts) {
    Harness harness(smart("standard"));
    harness.type("su3cl3");
    harness.key("Left");
    harness.type("su");
    const auto raw = harness.session()->pending_token.raw;
    const auto shown = harness.preedit();
    for (const auto key : {"Control+c", "Control+v", "Control+z", "Super+a", "Alt+Left"}) {
        RAWKEY_ASSERT(!harness.key_accepted(key));
        RAWKEY_ASSERT(harness.session()->pending_token.raw == raw);
        RAWKEY_ASSERT(harness.preedit() == shown);
        RAWKEY_ASSERT(harness.session()->buffer.caret() == 1);
    }
    auto released = Key('3').input_key();
    released.release = true;
    RAWKEY_ASSERT(!harness.engine().key_event(1, released));
    RAWKEY_ASSERT(harness.session()->pending_token.raw == raw);
    harness.type("3cl3");
    harness.expect_commit("你你好好");
}

RAWKEY_SUITE("smart cursor boundaries literal-only editing does not reclassify identifiers", smart_cursor_literal_editing) {
    for (const auto& [layout, keys] : layouts) {
        Harness harness(smart(layout));
        harness.type("helloworld");
        for (int i = 0; i < 5; ++i) harness.key("Left");
        harness.type(keys);
        const auto expected = "hello" + std::string(keys) + "world";
        RAWKEY_ASSERT(harness.preedit() == expected);
        RAWKEY_ASSERT(harness.session()->pending_token.empty());
        harness.expect_commit(expected);
    }
}

RAWKEY_SUITE("smart cursor boundaries raw candidates retain the prefix and suffix with either cursor setting", smart_cursor_selection_settings) {
    for (const bool move_cursor : {false, true}) {
        auto value = smart("standard");
        value.config.move_cursor_after_selection = move_cursor;
        Harness harness(value);
        harness.type("su3cl3");
        harness.key("Left");
        harness.type("su3cl3");
        harness.key("Down");
        harness.choose_text("你好");
        RAWKEY_ASSERT(harness.preedit() == "你你好好");
        RAWKEY_ASSERT(harness.engine().render_state(1).caret == 3);
        harness.key("Left");
        harness.type("su3cl3");
        harness.key("Down");
        harness.choose_text("su3cl3");
        RAWKEY_ASSERT(harness.preedit() == "你你su3cl3好好");
        harness.expect_commit("你你su3cl3好好");
    }
}

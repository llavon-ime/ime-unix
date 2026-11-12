#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("punctuation", punctuation) {
    {
        Harness harness;
        harness.type("[");
        harness.expect_commit("「");
        harness.type("]");
        harness.expect_commit("」");
        harness.key("?");
        harness.expect_commit("？");
        harness.key("{");
        harness.expect_commit("『");
        harness.key("}");
        harness.expect_commit("』");
        harness.key(":");
        harness.expect_commit("：");
        harness.key("\"");
        harness.expect_commit("；");
    }

    {
        // The comma key is a bopomofo letter in the standard layout.
        Harness harness;
        harness.type(",");
        RAWKEY_ASSERT(harness.preedit() == "ㄝ");
    }

    {
        Harness harness;
        harness.type("su3");
        harness.type("[");
        harness.expect_commit("你「");
    }

    {
        // Ctrl+punctuation uses the Microsoft key map.
        Harness harness;
        harness.key(Key("!").with(kCtrl));
        harness.expect_commit("！");
        harness.key(Key("'").with(kCtrl));
        harness.expect_commit("、");
        harness.key(Key(",").with(kCtrl));
        harness.expect_commit("，");
        harness.key(Key(".").with(kCtrl));
        harness.expect_commit("。");
        harness.key(Key("/").with(kCtrl));
        harness.expect_commit("？");
        harness.key(Key(";").with(kCtrl));
        harness.expect_commit("；");
    }

    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");

        harness.type(",");
        harness.expect_commit(",");
        harness.key("<");
        harness.expect_commit("，");
        harness.type("[");
        harness.expect_commit("[");
        harness.key("{");
        harness.expect_commit("『");
        harness.type(";");
        harness.expect_commit(";");
        harness.key(":");
        harness.expect_commit("：");
        harness.type("'");
        harness.expect_commit("'");
        harness.key("\"");
        harness.expect_commit("；");
        harness.type("b");
        RAWKEY_ASSERT(harness.preedit() == "ㄅ");
    }

    {
        // The numeric keypad keeps its literal meaning while composing: the
        // character joins the preedit instead of committing it, and with
        // nothing to compose the key is left to the application.
        Harness harness;
        harness.type("su3");
        harness.key(Key("KP_Decimal"));
        RAWKEY_ASSERT(harness.preedit() == "你.");
        harness.expect_commit("你.");

        Harness idle;
        RAWKEY_ASSERT(!idle.key_accepted(Key("KP_Decimal")));
        RAWKEY_ASSERT(!idle.key_accepted(Key("KP_Add")));
    }
}

namespace {
HarnessOptions apple_punctuation(std::string_view layout = "standard", bool smart = false) {
    HarnessOptions value;
    value.config.punctuation_style = "apple";
    value.config.keyboard_layout = layout;
    value.config.smart_english = smart;
    return value;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}
}  // namespace

// macOS Zhuyin's marks, read from the system Zhuyin keyboard layouts and
// checked on a Mac: \ is 、, _ is ＿, ^ is ＾, and ' and " alternate between
// the opening and closing quote. Every other mark matches libchewing's.
RAWKEY_SUITE("apple punctuation style", apple_punctuation_style) {
    {
        Harness harness(apple_punctuation());
        harness.type("\\");
        harness.expect_commit("、");
        harness.key("_");
        harness.expect_commit("＿");
        harness.key("^");
        harness.expect_commit("＾");
        harness.type("[");
        harness.expect_commit("「");
        harness.key("<");
        harness.expect_commit("，");
        harness.key("?");
        harness.expect_commit("？");
        harness.key(":");
        harness.expect_commit("：");
    }

    {
        // Quotes alternate across commits and inside one composition.
        Harness harness(apple_punctuation());
        harness.type("'");
        harness.expect_commit("‘");
        harness.type("'");
        harness.expect_commit("’");
        harness.key("\"");
        harness.expect_commit("“");
        harness.type("su3");
        harness.key("\"");
        harness.type("cl3");
        harness.type("'");
        harness.type("'");
        harness.expect_commit("你”好‘’");
    }

    {
        // Each input context keeps its own turn.
        Harness harness(apple_punctuation());
        harness.type("'");
        harness.expect_commit("‘");
        harness.use_context(2);
        harness.type("'");
        harness.expect_commit("‘");
        harness.use_context(1);
        harness.type("'");
        harness.expect_commit("’");
    }

    {
        // Phonetic keys of the layout stay phonetic: ' is ㄘ on ETen.
        Harness harness(apple_punctuation("et"));
        harness.type("\\");
        harness.expect_commit("、");
        harness.type("'");
        RAWKEY_ASSERT(harness.preedit() == "ㄘ");
    }

    {
        // The default style keeps libchewing's marks.
        Harness harness;
        harness.type("\\");
        harness.expect_commit("＼");
        harness.type("'");
        harness.expect_commit("、");
        harness.key("_");
        harness.expect_commit("—");
        harness.key("^");
        harness.expect_commit("︿");
    }
}

RAWKEY_SUITE("apple punctuation style in smart mixed input", apple_punctuation_smart) {
    {
        // After Chinese the quote keys type alternating quotes; the decoder's
        // turns continue into the next composition.
        Harness harness(apple_punctuation("standard", true));
        harness.type("cl3''");
        const auto shown = harness.preedit();
        if (!ends_with(shown, "‘’")) throw Failure{"cl3'' became " + shown};
        harness.expect_commit(shown);
        harness.type("cl3\"");
        const auto opened = harness.preedit();
        if (!ends_with(opened, "“")) throw Failure{"cl3\" became " + opened};
        harness.expect_commit(opened);
        harness.type("cl3\"");
        const auto closed = harness.preedit();
        if (!ends_with(closed, "”")) throw Failure{"second cl3\" became " + closed};
        harness.expect_commit(closed);
    }

    {
        // After English the apostrophe stays ASCII.
        Harness harness(apple_punctuation("standard", true));
        harness.type("don't");
        RAWKEY_ASSERT(harness.preedit() == "don't");
        harness.expect_commit("don't");
    }

    {
        // A quote committed by focus loss still counts.
        Harness harness(apple_punctuation("standard", true));
        harness.type("cl3'");
        const auto shown = harness.preedit();
        if (!ends_with(shown, "‘")) throw Failure{"cl3' became " + shown};
        harness.expect_focus_out_commit(shown);
        harness.type("'");
        harness.expect_commit("’");
    }
}

#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// 智慧型中英文 — mixed Chinese and English inside one composition. With
// SmartEnglish on, lowercase letters typed after already-composed Chinese
// segments form an ambiguous pending word that renders raw; a tone key
// converts it to 注音, space converts a valid natural reading to Chinese or
// otherwise commits it as English.
RAWKEY_SUITE("smart mixed", engine_test_smart_mixed) {
    // Chinese then English word: su3 -> 你, then "hello" is held as a pending
    // word; space commits composition + pending word + trailing space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "你hello");
        harness.expect_space_then_commit("你hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // English then Chinese: "hi" + space commits "hi ", then a fresh Chinese
    // syllable is composed normally.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        RAWKEY_ASSERT(harness.preedit() == "hi");
        harness.expect_space_then_commit("hi ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
}

    // Two Chinese syllables then English: ji3 -> 我, cjo4 -> 會, then the
    // pending "today" is committed as English along with the composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ji3");
        RAWKEY_ASSERT(harness.preedit() == "我");
        harness.type("cjo4");
        RAWKEY_ASSERT(harness.preedit() == "我會");
        harness.type("today");
        RAWKEY_ASSERT(harness.preedit() == "我會today");
        harness.expect_space_then_commit("我會today ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // First-tone Chinese via space, then English: rup + space -> 今, wu0 +
    // space -> 天 (both space decisions, top candidates stay in the
    // composition), then "bye" + space commits "今天bye ".
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        RAWKEY_ASSERT(harness.preedit() == "rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        RAWKEY_ASSERT(harness.preedit() == "今wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今天");
        harness.type("bye");
        RAWKEY_ASSERT(harness.preedit() == "今天bye");
        harness.expect_space_then_commit("今天bye ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // English word in the middle of Chinese typing: the pending "so" commits
    // as English (not a valid natural reading), leaving 你 committed; the
    // following syllable composes fresh.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("so");
        RAWKEY_ASSERT(harness.preedit() == "你so");
        harness.expect_space_then_commit("你so ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
}

    // Tone decision converts only the pending word: ru6 -> 及, then the
    // pending wu0 converts to 天 on space while the earlier 及 stays.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ru6");
        RAWKEY_ASSERT(harness.preedit() == "及");
        harness.type("wu0");
        RAWKEY_ASSERT(harness.preedit() == "及wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "及天");
}

    // English word + punctuation after Chinese: cl3 -> 好, pending "ok" is
    // committed together with the fullwidth period in one commit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
        harness.type("ok");
        RAWKEY_ASSERT(harness.preedit() == "好ok");
        harness.key(Key("Control+period"));
        RAWKEY_ASSERT(harness.preedit() == "好ok。");
        harness.expect_commit("好ok。");
}

    // Chinese + English + Chinese: su3 -> 你, "hi" + space commits "你hi ",
    // then vu84 composes 下 in the fresh buffer.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("hi");
        RAWKEY_ASSERT(harness.preedit() == "你hi");
        harness.expect_space_then_commit("你hi ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("vu84");
        RAWKEY_ASSERT(harness.preedit() == "下");
}

    // Reset the global config so later test files run with the default OFF.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
}
}

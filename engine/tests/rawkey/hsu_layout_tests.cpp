#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// Hsu (許氏) keyboard layout: unshifted punctuation is halfwidth, tones use
// letter keys, and digits join completed composition as literals.
RAWKEY_SUITE("hsu input", hsu_input) {
    Harness harness;
    harness.set_config("BopomofoKeyboardLayout", "許氏");

    harness.type("cen ");
    RAWKEY_ASSERT(harness.preedit() == "心");

    harness.key("space");
    RAWKEY_ASSERT(harness.has_candidates());
    RAWKEY_ASSERT(harness.candidate(0) == "心");

    harness.key("1");
    harness.expect_commit("心");
}

RAWKEY_SUITE("hsu halfwidth punctuation", hsu_halfwidth_punctuation) {
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(",");
        RAWKEY_ASSERT(harness.preedit() == ",");
        harness.expect_commit(",");
    }
    {
        // Shift+comma reaches the engine as the shifted symbol, which the
        // chewing table turns into the fullwidth comma.
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(Key(",").with(kShift));
        RAWKEY_ASSERT(harness.preedit() == "，");
        harness.expect_commit("，");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key("[");
        RAWKEY_ASSERT(harness.preedit() == "[");
        harness.expect_commit("[");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(";");
        RAWKEY_ASSERT(harness.preedit() == ";");
        harness.expect_commit(";");
    }
}

RAWKEY_SUITE("hsu fullwidth punctuation", hsu_fullwidth_punctuation) {
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key("<");
        RAWKEY_ASSERT(harness.preedit() == "，");
        harness.expect_commit("，");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key("{");
        RAWKEY_ASSERT(harness.preedit() == "『");
        harness.expect_commit("『");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.key(":");
        RAWKEY_ASSERT(harness.preedit() == "：");
        harness.expect_commit("：");
    }
}

RAWKEY_SUITE("hsu tones", hsu_tones) {
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        RAWKEY_ASSERT(harness.preedit() == "哦");
        harness.expect_commit("哦");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("geej");
        RAWKEY_ASSERT(harness.preedit() == "界");
        harness.expect_commit("界");
    }
}

RAWKEY_SUITE("hsu digits", hsu_digits) {
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.expect_commit("5");
        harness.type("5");
    }
    {
        Harness harness;
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("cen ");
        harness.type("5");
        RAWKEY_ASSERT(harness.preedit() == "心5");
        harness.expect_commit("心5");
    }
}

RAWKEY_SUITE("hsu alternative reading", hsu_alternative_reading) {
    Harness harness;
    harness.set_config("BopomofoKeyboardLayout", "許氏");
    harness.type("a ");
    RAWKEY_ASSERT(harness.preedit() == "疵");

    harness.key("space");
    RAWKEY_ASSERT(harness.has_candidates());
    RAWKEY_ASSERT(harness.candidate(0) == "疵");

    bool saw_alternative = false;
    for (std::size_t i = 0; i < harness.candidate_count(); ++i) {
        if (harness.candidate(i) == "ㄟ") saw_alternative = true;
    }
    RAWKEY_ASSERT(saw_alternative);

    harness.key("1");
    harness.expect_commit("疵");
}

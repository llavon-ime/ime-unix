#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

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

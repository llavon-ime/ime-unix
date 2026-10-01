#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// SmartEnglish on the standard keyboard: lowercase pending characters render
// as raw ASCII in the preedit until a tone key (3/6/4/7) or space decides
// Chinese vs English. All sequences below are verified against
// bopomofo_char.json.
RAWKEY_SUITE("smart standard", engine_test_smart_standard) {
    // Pending letters stay raw before the tone decision.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "su");
        harness.key(Key("3"));
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    // Each standard tone key converts ㄋㄧ (s, u) to its top candidate.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(Key("3"));
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(Key("6"));
        RAWKEY_ASSERT(harness.preedit() == "泥");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(Key("4"));
        RAWKEY_ASSERT(harness.preedit() == "逆");
}
    // ㄋㄧ˙ is not a reading in the table: keep the whole raw token editable.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        harness.key(Key("7"));
        RAWKEY_ASSERT(harness.preedit() == "su7");
        harness.expect_space_then_commit("su7 ");
}
    // Second tone: ru (ㄐㄧ) + 6 -> 及 (ㄐㄧˊ top candidate).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ru");
        harness.key(Key("6"));
        RAWKEY_ASSERT(harness.preedit() == "及");
}
    // Neutral tone: 2k (ㄉㄜ) + 7 -> 的.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("2k");
        harness.key(Key("7"));
        RAWKEY_ASSERT(harness.preedit() == "的");
}
    // A tone-looking digit in an English token remains editable ASCII.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(Key("6"));
        RAWKEY_ASSERT(harness.preedit() == "hello6");
        harness.expect_space_then_commit("hello6 ");
}
    // Multi-syllable: consecutive tone-key conversions append.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "好你");
}
    // Pending chars include the bopomofo digits: 1l (ㄅㄠ) + 4 -> 報.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("1l");
        RAWKEY_ASSERT(harness.preedit() == "1l");
        harness.key(Key("4"));
        RAWKEY_ASSERT(harness.preedit() == "報");
}
    // Bopomofo finals entered with , . ; are pending chars too.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("g;");
        RAWKEY_ASSERT(harness.preedit() == "g;");
        harness.key(Key("4"));
        RAWKEY_ASSERT(harness.preedit() == "上");
}
    // Space decides Chinese (first tone) for a valid two-char reading: 周.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("5.");
        RAWKEY_ASSERT(harness.preedit() == "5.");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "周");
}
    // After an English space-commit, a fresh pending word still converts.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_space_then_commit("hi ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    // Full syllable vu8 (ㄒㄧㄚ) + 4 -> 下.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("vu8");
        RAWKEY_ASSERT(harness.preedit() == "vu8");
        harness.key(Key("4"));
        RAWKEY_ASSERT(harness.preedit() == "下");
}
    // wu0 (ㄊㄧㄢ) + space -> 天 (first-tone space decision).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("wu0");
        RAWKEY_ASSERT(harness.preedit() == "wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "天");
}
}

#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// The SmartEnglish config option: default OFF preserves existing behavior
// exactly; turning it ON holds pending letters raw until a tone key or space
// decides; it can be toggled at any time. The config is GLOBAL across
// scenarios, so every scenario sets the config it needs explicitly.
RAWKEY_SUITE("smart config", engine_test_smart_config) {
    // Modernized schema/parser must retain legacy decimal spellings and the
    // same rendered candidates, selected character, and submitted text.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.set_config("CandidatePageSize", " +2");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.render_page_size() == 2);
        RAWKEY_ASSERT(harness.candidate(0) == "你");
        harness.key(Key("1"));
        harness.expect_commit("你");
        RAWKEY_ASSERT(harness.preedit().empty());
    }
    // 1. Default OFF: letters are 注音 immediately. "su" renders as ㄋㄧ (not
    //    raw "su"), then the tone key 3 (ˇ) gives 你.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        harness.type("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    // 2. OFF: space with a completed syllable opens the candidate list.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.has_candidates());
}
    // 3. ON: letters are held raw in the preedit; the tone key replays them
    //    as 注音 -> 你.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "su");
        harness.type("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    // 4. Toggle in the same run: ON types English (hello + space -> "hello "),
    //    then a fresh harness sets OFF again and 注音 behavior is restored.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_space_then_commit("hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
}
    // 5. OFF: English letters go through the bopomofo path. "hello" has no
    //    valid table reading, so the space decision drops the segment and the
    //    preedit ends up empty.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("hello");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // 6. Config isolation: explicit set each time. OFF -> 注音, ON -> raw +
    //    tone, OFF -> 注音 again, in three fresh harnesses.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "su");
        harness.type("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
}
    // 7. ON with the Hsu layout: known "hd" stays raw until Down exposes 哦.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("h");
        RAWKEY_ASSERT(harness.preedit() == "h");
        harness.key(Key('d'));
        RAWKEY_ASSERT(harness.preedit() == "hd");
        harness.key(Key("Down"));
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "哦");
}
    // 8. OFF with the Hsu layout keeps existing behavior: "hd" resolves
    //    directly to 哦.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("hd");
        RAWKEY_ASSERT(harness.preedit() == "哦");
}
}

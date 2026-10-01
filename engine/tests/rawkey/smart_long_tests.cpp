#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// SmartEnglish: long and multi-syllable compositions plus consecutive English
// words. Each scenario builds a fresh Harness and enables the
// SmartEnglish config option explicitly (config is global across tests).
RAWKEY_SUITE("smart long", engine_test_smart_long) {
    // 1. Three-syllable Chinese: 你(ㄋㄧˇ)+好(ㄏㄠˇ)+我(ㄨㄛˇ), tone keys
    // decide each syllable; preedit shows the composition, Return commits it.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        harness.type("ji3");
        RAWKEY_ASSERT(harness.preedit() == "你好我");
        harness.expect_commit("你好我");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 2. First-tone sentence: 今(ㄐㄧㄣ)+天(ㄊㄧㄢ) built from first-tone space
    // decisions (the space converts the pending word to Chinese in the
    // preedit, it does not commit), then 好 via a tone key; Return commits.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今天");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "今天好");
        harness.expect_commit("今天好");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 3. Long English word held as a pending word, shown raw in the preedit;
    // space commits it as English with a trailing space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("deadline");
        RAWKEY_ASSERT(harness.preedit() == "deadline");
        harness.expect_space_then_commit("deadline ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 4. Two consecutive English words in one harness: each word + space
    // commits that word (with trailing space), leaving an empty buffer.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        RAWKEY_ASSERT(harness.preedit() == "meeting");
        harness.expect_space_then_commit("meeting ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("report");
        RAWKEY_ASSERT(harness.preedit() == "report");
        harness.expect_space_then_commit("report ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 5. Mixed long line: Chinese composition 會(ㄏㄨㄟˋ)上(ㄕㄤˋ)的(ㄉㄜ˙),
    // then a pending English word; space commits everything in one commit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("cjo4");
        harness.type("g;4");
        harness.type("2k7");
        RAWKEY_ASSERT(harness.preedit() == "會上的");
        harness.type("review");
        harness.expect_space_then_commit("會上的review ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 6. Chinese built from a tone key (及, second tone) interleaved with
    // first-tone space decisions (今 天); the full preedit is 及今天.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ru6");
        RAWKEY_ASSERT(harness.preedit() == "及");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "及今");
        harness.type("wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "及今天");
}

    // 7. Long English word edited with backspace (pops one pending char at a
    // time), then space commits the shortened word as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("github");
        RAWKEY_ASSERT(harness.preedit() == "github");
        harness.key(Key("BackSpace"));
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "gith");
        harness.expect_space_then_commit("gith ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 8. Chinese previews remain reversible: Shift+Backspace explicitly undoes
    // one original key; ordinary Backspace deletes the displayed character.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.key(Key("Shift+BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "你cl");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "你c");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key("Shift+BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "su");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "s");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 9. Long mixed line with punctuation: Chinese 你好, pending English "ok",
    // then Shift+comma commits composition + pending word + fullwidth ，.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.type("ok");
        harness.key(Key("Control+comma"));
        RAWKEY_ASSERT(harness.preedit() == "你好ok，");
        harness.expect_commit("你好ok，");
}

    // 10. Very long composition: 你 好 我 會 上 的 下, one tone-key syllable
    // at a time; verify the preedit segment by segment, then commit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "你好");
        harness.type("ji3");
        RAWKEY_ASSERT(harness.preedit() == "你好我");
        harness.type("cjo4");
        RAWKEY_ASSERT(harness.preedit() == "你好我會");
        harness.type("g;4");
        RAWKEY_ASSERT(harness.preedit() == "你好我會上");
        harness.type("2k7");
        RAWKEY_ASSERT(harness.preedit() == "你好我會上的");
        harness.type("vu84");
        RAWKEY_ASSERT(harness.preedit() == "你好我會上的下");
        harness.expect_commit("你好我會上的下");
        RAWKEY_ASSERT(harness.preedit().empty());
}
}

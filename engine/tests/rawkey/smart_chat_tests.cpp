#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// 聊天情境: realistic chat messages mixing Chinese and English with
// SmartEnglish on. Pending English words are held raw in the preedit; space
// decides English (commit word + trailing space) or Chinese (first-tone
// reading), tone keys decide Chinese.
RAWKEY_SUITE("smart chat", engine_test_smart_chat) {
    // 1. English greeting: raw letters held, space commits word + space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "hello");
        harness.expect_space_then_commit("hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 2. Chinese reply: two tone-key syllables compose into one preedit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "你好");
}

    // 3. English goodbye.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("bye");
        harness.expect_space_then_commit("bye ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 4. Mixed one-line chat: Chinese syllable, then English pending word;
    // space commits composition + pending word + trailing space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.type("good");
        RAWKEY_ASSERT(harness.preedit() == "你good");
        harness.expect_space_then_commit("你good ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 5. First-tone Chinese in chat: space turns a natural reading into
    // Chinese (今), and the next first-tone syllable appends (今天).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.type("wu0");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今天");
}

    // 6. Chat English after Chinese: pending word commits with the
    // composition and a trailing space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "今hello");
        harness.expect_space_then_commit("今hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 7. English with punctuation: Shift+comma commits word + fullwidth ，.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(Key("Control+comma"));
        RAWKEY_ASSERT(harness.preedit() == "hi，");
        harness.expect_commit("hi，");
}

    // 8. Chat particle: tone key 7 (˙) turns the pending word into 的.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("2k7");
        RAWKEY_ASSERT(harness.preedit() == "的");
}

    // 9. Toggle-style chat words, each in its own harness.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("no");
        harness.expect_space_then_commit("no ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("yes");
        harness.expect_space_then_commit("yes ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
}

#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// Edge cases and non-letter keys with a pending word (SmartEnglish).
// Every scenario creates its own Harness and always enables
// SmartEnglish explicitly (the config is global across harnesses).
RAWKEY_SUITE("smart edge", engine_test_smart_edge) {
    // Backspace pops one pending char at a time.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hel");
        RAWKEY_ASSERT(harness.preedit() == "hel");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "he");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit() == "h");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Return commits the pending word as English (Return is consumed).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_commit("hello");
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Escape clears the pending word.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(Key("Escape"));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Punctuation commits composition + pending word + the punctuation.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(Key("Control+comma"));
        RAWKEY_ASSERT(harness.preedit() == "hi，");
        harness.expect_commit("hi，");
}
    // Shift+letter commits composition + uppercase letter.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "hello");
        harness.expect_direct_commit("helloA", Key("A").with(kShift));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Shift+space commits composition + space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_direct_commit("hello ", Key("Shift+space"));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // ASCII punctuation remains exact while an English token is pending.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.key(Key('@'));
        RAWKEY_ASSERT(harness.preedit() == "hello@");
        harness.expect_space_then_commit("hello@ ");
}
    // Arrow keys settle the pending token inside the editable composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(Key("Left"));
        RAWKEY_ASSERT(harness.preedit() == "hi");
        harness.expect_commit("hi");
}
    // Tab does not force a client commit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(Key("Tab"));
        RAWKEY_ASSERT(harness.preedit() == "hi");
        harness.expect_commit("hi");
}
    // Grave settles the token, then opens the symbol menu without committing.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.key(Key("`"));
        RAWKEY_ASSERT(harness.preedit() == "hi");
        RAWKEY_ASSERT(harness.has_candidates());
        harness.key(Key("Escape"));
        harness.expect_commit("hi");
}
    // Keypad keys commit the composition (pending word included).
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hi");
        harness.expect_direct_commit("hi", Key("KP_Enter"));
        RAWKEY_ASSERT(harness.preedit().empty());
}
    // Space confirms the preview without stealing focus; Down still opens the
    // settled reading's candidate list.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
}
}

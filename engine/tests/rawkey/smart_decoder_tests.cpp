#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// Mixed-input decoder scenarios: structured ASCII followed by Chinese keeps a
// complete mixed candidate, while Email/URL/path/code/number input is never
// silently rewritten. The exact raw ASCII path always remains selectable.
RAWKEY_SUITE("smart decoder", engine_test_smart_decoder) {
    // URL + Chinese: the complete mixed candidate commits both parts.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com5j/");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        harness.choose_text("https://example.com中");
        RAWKEY_ASSERT(harness.preedit() == "https://example.com中");
        harness.expect_commit("https://example.com中");
}

    // Filesystem path + Chinese.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("/tmp/283");
        RAWKEY_ASSERT(harness.preedit() == "/tmp/283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "/tmp/283");
        RAWKEY_ASSERT(harness.candidate(1) == "/tmp/打");
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == "/tmp/打");
        harness.expect_commit("/tmp/打");
}

    // Identifier + Chinese.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello_world283");
        RAWKEY_ASSERT(harness.preedit() == "hello_world283");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "hello_world283");
        RAWKEY_ASSERT(harness.candidate(1) == "hello_world打");
        harness.key(Key("2"));
        harness.expect_commit("hello_world打");
}

    // Versions, IPs, ports, dates and decimals commit as exact ASCII.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("v1.2.3");
        harness.expect_space_then_commit("v1.2.3 ");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("192.168.1.1");
        harness.expect_space_then_commit("192.168.1.1 ");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("localhost:8080");
        harness.expect_space_then_commit("localhost:8080 ");
}
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("2026-08-08");
        harness.expect_space_then_commit("2026-08-08 ");
}

    // mp3 is a known English token and does not open candidates automatically.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("mp3");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.expect_space_then_commit("mp3 ");
}

    // Backspace removes an explicitly applied Chinese candidate, then the next
    // numeric token remains raw until Chinese is requested again.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("283");
        RAWKEY_ASSERT(harness.preedit() == "打");
        harness.key(Key("Down"));
        harness.choose_text("打");
        RAWKEY_ASSERT(harness.preedit() == "打");
        harness.key(Key("BackSpace"));
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("284");
        RAWKEY_ASSERT(harness.preedit() == "大");
        harness.key(Key("Down"));
        harness.choose_text("大");
        RAWKEY_ASSERT(harness.preedit() == "大");
        harness.expect_commit("大");
}

    // Hsu: a tone-looking key inside an English token re-decodes without
    // consuming letters as candidates.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("hellod");
        RAWKEY_ASSERT(harness.preedit() == "hellod");
        harness.expect_space_then_commit("hellod ");
}

    // Hsu: Chinese sequences after a domain prefix keep the mixed candidate.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("mail.google.comnef");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "mail.google.comnef");
        RAWKEY_ASSERT(harness.candidate(1) == "mail.google.com你");
        harness.choose_text("mail.google.com你");
        harness.expect_commit("mail.google.com你");
}

    // Every complete path shown by the decoder must be selectable, including
    // paths containing more than one Bopomofo segment.
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "標準"},
                             {"SmartEnglish", "True"},
                             {"SelectionKeys", "數字鍵"}});
        harness.type("hello283su3");
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.candidate(0) == "hello打你");
        harness.choose_text("hello打你");
        RAWKEY_ASSERT(harness.preedit() == "hello打你");
        harness.expect_commit("hello打你");
}

    // FocusOut commits the captured raw input exactly once.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("https://example.com/v1.2.3");
        harness.expect_focus_out_commit("https://example.com/v1.2.3");
        harness.activate();
}
}

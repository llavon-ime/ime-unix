#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// SmartEnglish (智慧型中英文) work/office scenarios: meeting notes, status
// reports and technical chat. With SmartEnglish=ON, lowercase pending chars
// are held ambiguously; a tone key resolves them as 注音 while space resolves
// a valid reading as Chinese (first tone) or otherwise commits English.
RAWKEY_SUITE("smart work", engine_test_smart_work) {
    // 1. Meeting word: "meeting" + space commits as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("meeting");
        harness.expect_space_then_commit("meeting ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 2. Report status: 報 (1l4) + 告 (el4) compose 報告, Return commits it.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("1l4");
        RAWKEY_ASSERT(harness.preedit() == "報");
        harness.type("el4");
        RAWKEY_ASSERT(harness.preedit() == "報告");
        harness.expect_commit("報告");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 3. Deadline word: "deadline" + space commits as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("deadline");
        harness.expect_space_then_commit("deadline ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 4. Review request: "review" + space commits as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("review");
        harness.expect_space_then_commit("review ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 5. Chinese sentence: 會 (cjo4) 上 (g;4) 的 (2k7) combine to 會上的.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("cjo4");
        RAWKEY_ASSERT(harness.preedit() == "會");
        harness.type("g;4");
        RAWKEY_ASSERT(harness.preedit() == "會上");
        harness.type("2k7");
        RAWKEY_ASSERT(harness.preedit() == "會上的");
        harness.expect_commit("會上的");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 6. Mixed status line: 你 (su3) followed by English "good" + space
    // commits both: "你good ".
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

    // 7. Bug report: "bug" + space commits English, then 的 (2k7) resumes
    // Chinese composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("bug");
        harness.expect_space_then_commit("bug ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("2k7");
        RAWKEY_ASSERT(harness.preedit() == "的");
        harness.expect_commit("的");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 8. First-tone work word: 說 (gxi + space, Chinese first-tone decision),
    // then 及 (ru6, second tone via tone key) appends to the composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("gji");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "說");
        harness.type("ru6");
        RAWKEY_ASSERT(harness.preedit() == "說及");
        harness.expect_commit("說及");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 9. GitHub: "github" + space commits as English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("github");
        harness.expect_space_then_commit("github ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 10. Test result: "test" + space then "ok" + space, both English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("test");
        harness.expect_space_then_commit("test ");
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("ok");
        harness.expect_space_then_commit("ok ");
        RAWKEY_ASSERT(harness.preedit().empty());
}
}

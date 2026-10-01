#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

namespace {

}  // namespace

// Candidate list interaction after a Chinese preview is explicitly confirmed
// and opened with Down (SmartEnglish ON).
RAWKEY_SUITE("smart candidate", engine_test_smart_candidate) {
    // 1. Confirm the preview with Space, then open candidates with Down.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate_count() > 1);
}

    // 2. Select a non-top candidate with a digit.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "你");
        const std::string second = harness.candidate(1);
        RAWKEY_ASSERT(!second.empty());
        harness.key(Key("2"));
        RAWKEY_ASSERT(harness.preedit() == second);
        harness.expect_commit(second);
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 3. Navigate the candidate list after conversion: Down moves the cursor,
    // a second Space (space_selects_candidate, the default) selects it.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.cursor_index() == 0);
        const std::string second = harness.candidate(1);
        RAWKEY_ASSERT(!second.empty());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.cursor_index() == 1);
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit() == second);
        harness.expect_commit(second);
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 4. Escape closes the candidate list, keeping the converted Chinese.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        harness.key(Key("Escape"));
        RAWKEY_ASSERT(!harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit() == "你");
}

    // Space selects a mixed raw candidate without committing it. Return is
    // the explicit mixed-candidate commit action.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello_world283");
        RAWKEY_ASSERT(harness.preedit() == "hello_world283");
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.candidate(0) == "hello_world283");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit() == "hello_world283");
        harness.expect_commit("hello_world283");
}

    // 5. English after selecting a Chinese candidate: the digit selects the
    // candidate into the preedit, Return commits it, then the next pending
    // word types English.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        const std::string top = harness.candidate(0);
        harness.key(Key("1"));
        RAWKEY_ASSERT(harness.preedit() == top);
        harness.expect_commit(top);
        RAWKEY_ASSERT(harness.preedit().empty());
        harness.type("ok");
        RAWKEY_ASSERT(harness.preedit() == "ok");
        harness.expect_space_then_commit("ok ");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 6. A space-converted syllable opens candidates only on Down.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        RAWKEY_ASSERT(harness.preedit() == "rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.key(Key(" "));
        RAWKEY_ASSERT(!harness.has_candidates());
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
}

    // 7. Select a candidate from a space-converted syllable.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("rup");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        harness.key(Key("1"));
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.expect_commit("今");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 8. An English pending word never opens the candidate list on space.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        RAWKEY_ASSERT(harness.preedit() == "hello");
        harness.expect_space_then_commit("hello ");
        RAWKEY_ASSERT(harness.preedit().empty());
        RAWKEY_ASSERT(!harness.has_candidates());
}

    // 9. Return commits Chinese directly after the tone conversion.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
        harness.expect_commit("好");
        RAWKEY_ASSERT(harness.preedit().empty());
}

    // 10. Letters while the candidate list is open close it and start a new
    // pending word appended to the composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("su3");
        harness.key(Key(" "));
        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.has_candidates());
        harness.type("hi");
        RAWKEY_ASSERT(!harness.has_candidates());
        RAWKEY_ASSERT(harness.preedit() == "你hi");
}
}

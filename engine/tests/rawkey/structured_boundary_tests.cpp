#include "raw_key_harness.hpp"

#include <algorithm>
#include <string>

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("structured tokens offer reversible Chinese prefix", structured_chinese_prefix) {
    for (const char* layout : {"標準", "許氏"}) {
        const std::string prefix = std::string_view(layout) == "標準" ? "su3cl3" : "nefhwf";
        for (const char* literal : {"user_name42", "dev@example.com", "https://example.org/docs?v=2",
                                    "example.org", "florpington_name42", "user_2026_name"}) {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"},
                                 {"SelectionKeys", "數字鍵"}});
            harness.type(prefix + literal);
            const auto expected = std::string("你好") + literal;
            RAWKEY_ASSERT(harness.preedit() == prefix + literal);
            RAWKEY_ASSERT(harness.commits().empty());
            // A better automatic interpretation must not remove the original
            // reversible spelling from the actual candidate window.
            harness.key("Down");
            const auto entries = harness.candidates();
            RAWKEY_ASSERT(std::ranges::find(entries, prefix + literal) != entries.end());
            RAWKEY_ASSERT(std::ranges::find(entries, expected) != entries.end());
            harness.choose_text(expected);
            harness.expect_commit(expected);
            RAWKEY_ASSERT(harness.last_commit() == expected);
        }
        {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"},
                                 {"SelectionKeys", "數字鍵"}});
            const auto raw = prefix + "dev@example.com";
            harness.type(raw);
            harness.key("Down");
            harness.choose_text(raw);
            harness.expect_commit(raw);
        }
        {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            harness.type(prefix + "user_name42");
            // Delete exactly the displayed literal suffix, then restore it.
            // Removing '_' must not change the earlier displayed raw prefix
            // into Chinese as a side effect of deleting a character.
            for (int key = 0; key < 7; ++key) harness.key("BackSpace");
            RAWKEY_ASSERT(harness.preedit() == prefix + "user");
            harness.type("_name42");
            RAWKEY_ASSERT(harness.preedit() == prefix + "user_name42");
            harness.expect_focus_out_commit(prefix + "user_name42");
        }
        {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            harness.type(prefix + "dev@example.con");
            harness.key("BackSpace");
            harness.type("m");
            harness.expect_commit(prefix + "dev@example.com");
        }
    }
}

RAWKEY_SUITE("structured literal names remain exact", structured_literal_names) {
    for (const char* layout : {"標準", "許氏"}) {
        for (const char* literal : {"a_b", "a@example.com", "z@example.net", "florpington_name42",
                                    "qwerty@example.org", "xylophonic.org", "abc123_foo", "x86_64",
                                    "mp3_file", "__private_name", "custom+v2://host/path",
                                    "user_2026_name", "v2.1.0", "fooBar_name42"}) {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            harness.type(literal);
            // Shift-uppercase may commit the preceding portion by existing
            // policy. Check the complete client text, not just the pending tail.
            std::string before;
            for (const auto& commit : harness.commits()) before += commit;
            before += harness.preedit();
            if (before != literal) {
                throw Failure{std::string(layout) + ": " + literal + " => " + before};
            }
            harness.key("Return");
            std::string after;
            for (const auto& commit : harness.commits()) after += commit;
            RAWKEY_ASSERT(after == literal);
            RAWKEY_ASSERT(harness.composition_empty());
        }
    }
}

RAWKEY_SUITE("unseen opaque names are not Chinese", opaque_structured_names) {
    // Regression discovered only after freezing the independent holdout. It is
    // now development data, not evidence for a further tuned score.
    for (const char* layout : {"標準", "許氏"}) {
        for (const char* suffix : {"_cache", "@mail.invalid", ".invalid", "://host.invalid/path"}) {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            const auto raw = std::string("hwfmcmemtgjlgqgvwxefgvuy") + suffix;
            harness.type(raw);
            RAWKEY_ASSERT(harness.preedit() == raw);
            harness.expect_commit(raw);
        }
    }
}

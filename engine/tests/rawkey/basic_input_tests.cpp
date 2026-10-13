#include "raw_key_harness.hpp"

#include <cstdlib>

using namespace llavon::ime::rawkey;

// Types "su3" (ㄋㄧˇ in the standard layout), opens the candidate list with
// Space, selects with a digit, and commits with Return.
RAWKEY_SUITE("basic input", basic_input) {
    Harness harness;

    harness.type("su3");
    // A completed syllable converts to its top candidate immediately.
    RAWKEY_ASSERT(harness.preedit() == "你");

    harness.key("space");
    RAWKEY_ASSERT(harness.has_candidates());
    const std::string first = harness.candidate(0);
    RAWKEY_ASSERT(!first.empty());

    // 1 selects the first candidate, which commits it directly.
    harness.key("1");
    harness.expect_commit(first);
}

RAWKEY_SUITE("singleton ei preserves the input character", singleton_ei) {
    HarnessOptions options;
    options.config.smart_english = false;
    // Point at an isolated real service to repeat this regression with model
    // inference; the default run still checks offline rendering and commits.
    if (const auto* socket = std::getenv("LLAVON_IME_TEST_TOKEN_ALIAS_SOCKET"); socket && *socket)
        options.socket_path = socket;
    Harness harness(options);

    harness.type("o ");
    RAWKEY_ASSERT(harness.preedit() == "ㄟ");
    RAWKEY_ASSERT(harness.session()->buffer.segment_reading(0) == u"ㄟ ");
    harness.key("Down");
    RAWKEY_ASSERT(harness.has_candidates());
    RAWKEY_ASSERT(harness.candidates() == std::vector<std::string>{"ㄟ"});
    harness.key("Escape");
    harness.expect_commit("ㄟ");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
    RAWKEY_ASSERT(harness.last_commit() == "ㄟ");

    harness.type("o su3");
    RAWKEY_ASSERT(harness.preedit() == "ㄟ你");
    RAWKEY_ASSERT(harness.session()->buffer.segment_reading(0) == u"ㄟ ");
    harness.expect_commit("ㄟ你");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
    RAWKEY_ASSERT(harness.last_commit() == "ㄟ你");

    harness.type("o4");
    RAWKEY_ASSERT(harness.preedit() == "欸");
    RAWKEY_ASSERT(harness.session()->buffer.segment_reading(0) == u"ㄟˋ");
    harness.expect_commit("欸");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
    RAWKEY_ASSERT(harness.last_commit() == "欸");
}

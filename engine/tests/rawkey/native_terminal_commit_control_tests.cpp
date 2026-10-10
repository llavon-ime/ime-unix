#include "raw_key_harness.hpp"

namespace llavon::ime::rawkey {

// Shared-engine side of the Docker terminal transport control. This does not
// assert native application delivery; the separate passive Readline oracle
// and protocol trace must check that independently.
RAWKEY_SUITE("rapid retyped raw keys produce two exact terminal control commits", terminal_commit_control) {
    Harness harness;
    harness.activate();
    harness.key("s");
    RAWKEY_ASSERT(harness.preedit() == "ㄋ");
    harness.key("u");
    harness.key("p");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧㄣ");
    harness.key("BackSpace");
    harness.key("p");
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    harness.key("3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.expect_commit("你");
    RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{"你"});
    for (const auto key : {"s", "u", "p"}) harness.key(key);
    for (int i = 0; i < 3; ++i) harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit().empty());
    RAWKEY_ASSERT(!harness.key_accepted("BackSpace"));
    for (const auto key : {"s", "u", "p"}) harness.key(key);
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    harness.key("3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.expect_commit("你");
    RAWKEY_ASSERT((harness.commits() == std::vector<std::string>{"你", "你"}));
}

}  // namespace llavon::ime::rawkey

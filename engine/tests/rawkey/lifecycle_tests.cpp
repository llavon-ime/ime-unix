#include "raw_key_harness.hpp"

#include "protocol/protocol.hpp"

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("update readiness observes composition without committing", update_readiness) {
    Harness harness;
    harness.set_config("SmartEnglish", "False");
    harness.type("su");
    RAWKEY_ASSERT(!harness.update_ready());
    for (int attempt = 0; attempt < 3; ++attempt) {
        RAWKEY_ASSERT(!harness.composition_empty());
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        RAWKEY_ASSERT(harness.commits().empty());
    }
    harness.key("3");
    RAWKEY_ASSERT(!harness.composition_empty());
    RAWKEY_ASSERT(harness.preedit() == "你");
    RAWKEY_ASSERT(harness.commits().empty());
    harness.expect_commit("你");
    RAWKEY_ASSERT(harness.update_ready());
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(harness.commits().size() == 1);

    harness.type("su");
    harness.key("Escape");
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(harness.commits().size() == 1);
}

RAWKEY_SUITE("update readiness covers every attached client", update_all_clients) {
    Harness harness;
    harness.set_config("SmartEnglish", "False");
    harness.type("su");
    harness.use_context(2);
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(!harness.update_ready());
    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(!harness.update_ready());
    harness.use_context(1);
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    RAWKEY_ASSERT(harness.commits().size() == 1);
    harness.key("Escape");
    RAWKEY_ASSERT(harness.update_ready());
    RAWKEY_ASSERT(harness.preedit().empty());
    RAWKEY_ASSERT(harness.commits().size() == 1);
}

RAWKEY_SUITE("lifecycle", lifecycle) {
    // Focus-out commits a complete composition.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.expect_focus_out_commit("你");
        RAWKEY_ASSERT(harness.preedit().empty());
    }

    // Focus-out also commits an unresolved smart-English pending token.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        harness.expect_focus_out_commit("hello");
        RAWKEY_ASSERT(harness.preedit().empty());
    }

    // An unfinished bopomofo reading is discarded rather than committed.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su");
        harness.focus_out();
        RAWKEY_ASSERT(harness.preedit().empty());
        RAWKEY_ASSERT(harness.session()->context_text.empty());
    }

    // An explicit client reset never commits, even with a complete segment.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        auto* session = harness.session();
        RAWKEY_ASSERT(session != nullptr);
        const auto generation = session->prediction.generation;
        harness.reset();
        RAWKEY_ASSERT(harness.preedit().empty());
        RAWKEY_ASSERT(session->empty());
        RAWKEY_ASSERT(session->buffer.empty());
        RAWKEY_ASSERT(session->context_text.empty());
        RAWKEY_ASSERT(session->prediction.generation == generation + 1);
    }

    // Config changes settle exact pending keys as literals, clear the mixed
    // decision, invalidate prediction, and close an existing model session.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("hello");
        auto* session = harness.session();
        RAWKEY_ASSERT(session != nullptr);
        llavon::ime::protocol::SessionId session_id{};
        session_id[0] = 0x44;
        session->prediction.session_id = session_id;
        const auto generation = session->prediction.generation;

        harness.set_config("SmartEnglish", "False");
        RAWKEY_ASSERT(session->pending_token.empty());
        RAWKEY_ASSERT(!session->mixed_decision.active());
        RAWKEY_ASSERT(session->buffer.commit_text() == std::u16string(u"hello"));
        RAWKEY_ASSERT(!session->prediction.session_open());
        RAWKEY_ASSERT(session->prediction.generation == generation + 1);
    }
}

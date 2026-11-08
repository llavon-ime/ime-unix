#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

namespace {

}  // namespace

// Down/Up move the cursor within the page; Home/End jump to list bounds.
RAWKEY_SUITE("candidate navigation tests", engine_test_candidate_navigation_tests) {
    {
        Harness harness;
        harness.type("su3");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.has_candidates());
        RAWKEY_ASSERT(harness.cursor_index() == 0);
        const std::string first = harness.candidate(0);

        harness.key(Key("Down"));
        RAWKEY_ASSERT(harness.cursor_index() == 1);
        RAWKEY_ASSERT(harness.candidate(1) != first);

        harness.key(Key("Up"));
        RAWKEY_ASSERT(harness.cursor_index() == 0);

        harness.key(Key("End"));
        RAWKEY_ASSERT(harness.cursor_index() ==
                     static_cast<int>(harness.candidate_count()) - 1);

        harness.key(Key("Home"));
        RAWKEY_ASSERT(harness.cursor_index() == 0);
        harness.expect_commit(first);

        // Boundary navigation must preserve the composition before the
        // candidate panel has been opened.
        Harness composing;
        composing.type("su3");
        RAWKEY_ASSERT(!composing.has_candidates());
        const std::string composition = composing.preedit();
        for (int i = 0; i < 3; ++i) {
            composing.key(Key("Left"));
            RAWKEY_ASSERT(!composing.has_candidates());
            RAWKEY_ASSERT(composing.preedit() == composition);
        }
        composing.expect_commit(first);

        // McBopomofo clamps candidate targeting at composition boundaries.
        // With the default before-cursor mode, Down at the leading edge opens
        // candidates for the first segment instead of leaking to the client.
        Harness before_boundary;
        before_boundary.type("su3");
        before_boundary.key(Key("Left"));
        RAWKEY_ASSERT(!before_boundary.has_candidates());
        before_boundary.key(Key("Down"));
        RAWKEY_ASSERT(before_boundary.has_candidates());
        before_boundary.expect_commit(before_boundary.candidate(0));

        // The symmetric after-cursor case clamps the trailing edge to the last
        // segment, so Down still opens a valid candidate list there.
        Harness after_boundary;
        after_boundary.set_config("SelectPhrase", "after_cursor");
        after_boundary.type("su3");
        RAWKEY_ASSERT(!after_boundary.has_candidates());
        after_boundary.key(Key("Down"));
        RAWKEY_ASSERT(after_boundary.has_candidates());
        after_boundary.expect_commit(after_boundary.candidate(0));

        // Left/Right flip pages with a small page size.
        Harness pager;
        pager.set_config("CandidatePageSize", "3");
        pager.type("su3");
        pager.key(Key(" "));
        RAWKEY_ASSERT(pager.candidate_count() == 3);
        const std::string first_page = pager.candidate(0);

        // Repeated previous-page requests at the first page must keep the
        // candidate panel populated.
        for (int i = 0; i < 3; ++i) {
            pager.key(Key("Left"));
            RAWKEY_ASSERT(pager.has_candidates());
            RAWKEY_ASSERT(pager.candidate_count() == 3);
            RAWKEY_ASSERT(pager.candidate(0) == first_page);
            RAWKEY_ASSERT(pager.cursor_index() == 0);
        }

        pager.key(Key("Right"));
        RAWKEY_ASSERT(pager.candidate_count() == 3);
        RAWKEY_ASSERT(pager.candidate(0) != first_page);

        pager.key(Key("Left"));
        RAWKEY_ASSERT(pager.candidate(0) == first_page);

        // Page_Up / Page_Down flip pages and reset the cursor.
        pager.key(Key("Page_Down"));
        RAWKEY_ASSERT(pager.candidate_count() == 3);
        RAWKEY_ASSERT(pager.candidate(0) != first_page);
        RAWKEY_ASSERT(pager.cursor_index() == 0);

        pager.key(Key("Page_Up"));
        RAWKEY_ASSERT(pager.candidate(0) == first_page);
        RAWKEY_ASSERT(pager.cursor_index() == 0);
        pager.expect_commit(pager.candidate(0));

        // Tab opens the candidate list; a second Tab expands it, a third collapses.
        Harness tab;
        tab.set_config("CandidatePageSize", "3");
        tab.type("su3");
        tab.key(Key("Tab"));
        RAWKEY_ASSERT(tab.has_candidates());
        const size_t collapsed = tab.candidate_count();

        tab.key(Key("Tab"));
        RAWKEY_ASSERT(tab.candidate_count() > collapsed);

        tab.key(Key("Tab"));
        RAWKEY_ASSERT(tab.candidate_count() == collapsed);
        tab.expect_commit(tab.candidate(0));

        // CandidateTabAction=next_candidate keeps the first Tab for opening
        // the list, then moves the cursor and follows it across pages.
        Harness tab_next;
        tab_next.set_config("CandidatePageSize", "3");
        tab_next.set_config("CandidateTabAction", "next_candidate");
        tab_next.type("su3");
        tab_next.key(Key("Tab"));
        RAWKEY_ASSERT(tab_next.has_candidates());
        RAWKEY_ASSERT(tab_next.cursor_index() == 0);
        const std::string first_tab_candidate = tab_next.candidate(0);

        tab_next.key(Key("Tab"));
        RAWKEY_ASSERT(tab_next.cursor_index() == 1);
        RAWKEY_ASSERT(tab_next.candidate(1) != first_tab_candidate);

        tab_next.key(Key("Tab"));
        RAWKEY_ASSERT(tab_next.cursor_index() == 2);

        tab_next.key(Key("Tab"));
        RAWKEY_ASSERT(tab_next.render_page() == 1);
        RAWKEY_ASSERT(tab_next.cursor_index() == 0);
        tab_next.expect_commit(tab_next.candidate(0));

        // A digit selects the candidate at that index.
        Harness digit;
        digit.type("su3");
        digit.key(Key(" "));
        const std::string second = digit.candidate(1);
        RAWKEY_ASSERT(!second.empty());
        digit.key(Key("2"));
        RAWKEY_ASSERT(digit.preedit() == second);
        digit.expect_commit(second);

        // Return selects the candidate under the cursor.
        Harness enter;
        enter.type("su3");
        enter.key(Key(" "));
        enter.key(Key("Down"));
        const std::string second_enter = enter.candidate(1);
        enter.key(Key("Return"));
        RAWKEY_ASSERT(enter.preedit() == second_enter);
        enter.expect_commit(second_enter);

        // ChooseCandidateUsingSpace=True: a second Space selects the cursor
        // candidate; False: Space only opens the list.
        Harness space_true;
        space_true.set_config("ChooseCandidateUsingSpace", "True");
        space_true.type("su3");
        space_true.key(Key(" "));
        RAWKEY_ASSERT(space_true.has_candidates());
        const std::string top = space_true.candidate(0);
        space_true.key(Key(" "));
        RAWKEY_ASSERT(!space_true.has_candidates());
        RAWKEY_ASSERT(space_true.preedit() == top);
        space_true.expect_commit(top);

        Harness space_false;
        space_false.set_config("ChooseCandidateUsingSpace", "False");
        space_false.type("su3");
        space_false.key(Key(" "));
        space_false.key(Key(" "));
        RAWKEY_ASSERT(space_false.has_candidates());
        RAWKEY_ASSERT(space_false.cursor_index() == 0);
        space_false.key(Key("Return"));
        RAWKEY_ASSERT(space_false.preedit() == top);
        space_false.expect_commit(top);

        // Escape closes the candidate list, keeping the composition; a second
        // Escape keeps the composition intact.
        Harness esc;
        esc.type("su3");
        esc.key(Key(" "));
        esc.key(Key("Escape"));
        RAWKEY_ASSERT(!esc.has_candidates());
        RAWKEY_ASSERT(esc.preedit() == top);
        esc.key(Key("Escape"));
        RAWKEY_ASSERT(esc.preedit() == top);
        esc.expect_commit(top);

        // EscKeyClearsEntireComposingBuffer=True: Escape clears the buffer.
        Harness esc_clear;
        esc_clear.set_config("EscKeyClearsEntireComposingBuffer", "True");
        esc_clear.type("su3");
        esc_clear.key(Key("Escape"));
        RAWKEY_ASSERT(esc_clear.preedit().empty());
        RAWKEY_ASSERT(!esc_clear.has_candidates());
}

    // Selecting a candidate changes the composition revision and the chosen
    // padding the model sees, so it must request a fresh prediction even when
    // no earlier request is in flight.
    {
        Harness harness;
        harness.set_config("SmartEnglish", "False");
        harness.type("su3");
        harness.key(Key(" "));
        RAWKEY_ASSERT(harness.has_candidates());

        harness.settle_prediction();
        RAWKEY_ASSERT(!harness.session()->prediction.pending);
        const size_t revision_before = harness.session()->buffer.revision();

        harness.key(Key("1"));
        const auto* state = harness.session();
        // The revision and key are set when the request starts and survive the
        // request finishing, so they are the stable proof that a fresh request
        // was made. `pending` is not: with no service behind the socket the
        // failed attempt can already be drained here, which is why asserting it
        // only held on the platform whose connect failure arrived later.
        RAWKEY_ASSERT(state->prediction.revision == state->buffer.revision());
        RAWKEY_ASSERT(state->prediction.revision > revision_before);
        RAWKEY_ASSERT(state->prediction.key == state->buffer.raw_composition());
}
}

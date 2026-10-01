#include "test_suites.h"

#include <cstdlib>

#include "input/candidate_view.hpp"

int run_candidate_view_tests() {
    using llavon::ime::CandidateView;

    bool ok = true;

    // Collapsed lists use the configured page size; expanded lists show every
    // candidate at once, but an empty list falls back to the configured size.
    CandidateView view;
    ok = ok && view.page_size(10, 25) == 10;
    ok = ok && view.page_size(10, 0) == 10;
    view.expanded = true;
    ok = ok && view.page_size(10, 25) == 25;
    ok = ok && view.page_size(10, 0) == 10;
    view.expanded = false;

    // Page offsets follow the page size.
    view.page = 2;
    ok = ok && view.page_offset(10, 25) == 20;
    view.expanded = true;
    ok = ok && view.page_offset(10, 25) == 50;
    view.expanded = false;
    view.reset();
    ok = ok && view.page == 0 && view.cursor == 0 && !view.expanded;

    // Cursor movement wraps inside the current page and never crosses pages.
    view.page = 0;
    ok = ok && !view.move_cursor(0, 10, 25);
    ok = ok && view.move_cursor(1, 10, 25) && view.cursor == 1;
    view.cursor = 0;
    ok = ok && view.move_cursor(-1, 10, 25) && view.cursor == 9;
    view.cursor = 9;
    ok = ok && view.move_cursor(1, 10, 25) && view.cursor == 0;
    view.page = 2;
    view.cursor = 24;
    ok = ok && view.move_cursor(1, 10, 25) && view.cursor == 20;
    view.reset();
    ok = ok && !view.move_cursor(1, 10, 0);

    // Paging clamps at both ends and optionally keeps the row offset.
    view.cursor = 13;
    ok = ok && view.page_by(1, true, 10, 25) && view.page == 1 && view.cursor == 13;
    ok = ok && view.page_by(1, true, 10, 25) && view.page == 2 && view.cursor == 23;
    ok = ok && !view.page_by(1, true, 10, 25);
    ok = ok && view.page_by(-2, false, 10, 25) && view.page == 0 && view.cursor == 23;
    ok = ok && !view.page_by(-1, false, 10, 25);
    view.reset();
    ok = ok && !view.page_by(1, true, 10, 0);

    // Setting the cursor clamps into range and realigns the page.
    view.cursor = 3;
    ok = ok && view.set_cursor(24, 10, 25) && view.cursor == 24 && view.page == 2;
    ok = ok && !view.set_cursor(24, 10, 25);
    ok = ok && view.set_cursor(-5, 10, 25) && view.cursor == 0 && view.page == 0;
    ok = ok && !view.set_cursor(0, 10, 25);
    ok = ok && !view.set_cursor(1, 10, 0);

    // Clamping repairs an out-of-range cursor and its page.
    view.cursor = 99;
    view.page = 7;
    view.clamp(10, 25);
    ok = ok && view.cursor == 24 && view.page == 2;
    view.expanded = true;
    view.clamp(10, 25);
    ok = ok && view.cursor == 24 && view.page == 0;
    view.expanded = false;
    view.clamp(10, 0);
    ok = ok && view.cursor == 0 && view.page == 0;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

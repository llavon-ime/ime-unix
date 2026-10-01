#include "test_suites.h"

#include <cstdlib>
#include <unordered_set>

#include "symbol/symbol_menu.hpp"

int run_symbol_menu_tests() {
    bool ok = true;

    const auto symbols = llavon::ime::mcbopomofo_symbols();
    ok = ok && symbols.size() == 217;
    ok = ok && symbols.front() == U'　';
    ok = ok && symbols[1] == U'，';
    ok = ok && symbols[8] == U'！';
    ok = ok && symbols[18] == U'（';
    ok = ok && symbols.back() == U'ω';

    std::unordered_set<char32_t> unique;
    for (const char32_t symbol : symbols) {
        ok = ok && symbol != 0;
        ok = ok && !(symbol >= 0xD800 && symbol <= 0xDFFF);
        ok = ok && symbol <= 0x10FFFF;
        ok = ok && unique.insert(symbol).second;
    }

    llavon::ime::SymbolMenuState state;
    ok = ok && !state.active();
    const auto initial_epoch = state.epoch();
    state.open();
    const auto first_epoch = state.epoch();
    ok = ok && state.active() && first_epoch == initial_epoch + 1;
    ok = ok && state.matches(first_epoch);

    // First menu level lists categories and direct symbols.
    ok = ok && state.menu().size() == 13;
    ok = ok && state.menu()[0] == U"…";
    ok = ok && state.menu()[2] == U"常用符號";
    ok = ok && !state.in_category();

    // Selecting a direct symbol picks it and keeps the menu level.
    char32_t symbol = 0;
    ok = ok && state.select(0, symbol) && symbol == U'…';
    ok = ok && !state.in_category();

    // Selecting a category descends to its symbol list.
    ok = ok && !state.select(2, symbol);
    ok = ok && state.in_category();
    ok = ok && state.menu().size() == 30;
    ok = ok && state.menu()[0] == U"，";
    ok = ok && state.select(0, symbol) && symbol == U'，';

    // Back returns to the first level.
    state.back();
    ok = ok && !state.in_category();
    ok = ok && state.menu()[2] == U"常用符號";

    // Out-of-range selection is rejected.
    ok = ok && !state.select(state.menu().size(), symbol);

    state.close();
    ok = ok && !state.active() && !state.matches(first_epoch);
    state.open();
    ok = ok && state.active() && state.epoch() > first_epoch;
    ok = ok && !state.matches(first_epoch);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

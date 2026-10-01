#include "test_suites.h"

#include <cstdlib>
#include <optional>

#include "input/punctuation.hpp"

int run_punctuation_tests() {
    using llavon::ime::BopomofoKeyboardLayout;
    using llavon::ime::chewing_punctuation_for_key;
    using llavon::ime::input_key_state;
    using llavon::ime::InputKey;
    using llavon::ime::InputKeyState;

    const auto standard = BopomofoKeyboardLayout::Standard;
    const auto hsu = BopomofoKeyboardLayout::Hsu;
    bool ok = true;

    const auto symbol = [](char32_t value) {
        InputKey key;
        key.sym = value;
        return key;
    };

    // The standard layout commits the fullwidth punctuation of the symbol row.
    ok = ok && chewing_punctuation_for_key(symbol(U','), standard) == U'，';
    ok = ok && chewing_punctuation_for_key(symbol(U'.'), standard) == U'。';
    ok = ok && chewing_punctuation_for_key(symbol(U';'), standard) == U'；';
    ok = ok && chewing_punctuation_for_key(symbol(U'['), standard) == U'「';
    ok = ok && chewing_punctuation_for_key(symbol(U']'), standard) == U'」';
    ok = ok && chewing_punctuation_for_key(symbol(U'='), standard) == U'＝';
    ok = ok && chewing_punctuation_for_key(symbol(U'~'), standard) == U'～';
    ok = ok && !chewing_punctuation_for_key(symbol(U'a'), standard).has_value();
    ok = ok && !chewing_punctuation_for_key(symbol(U' '), standard).has_value();

    // A keysym that already carries the folded Shift still resolves.
    ok = ok && chewing_punctuation_for_key(symbol(U'<'), standard) == U'，';
    ok = ok && chewing_punctuation_for_key(symbol(U':'), standard) == U'：';
    InputKey shift_state;
    shift_state.sym = U',';
    shift_state.states = input_key_state(InputKeyState::Shift);
    ok = ok && chewing_punctuation_for_key(shift_state, standard) == U'，';

    // The Hsu layout commits the halfwidth key itself without Shift and the
    // fullwidth punctuation once Shift is folded in.
    ok = ok && chewing_punctuation_for_key(symbol(U','), hsu) == U',';
    ok = ok && chewing_punctuation_for_key(symbol(U';'), hsu) == U';';
    ok = ok && !chewing_punctuation_for_key(symbol(U'a'), hsu).has_value();
    ok = ok && chewing_punctuation_for_key(symbol(U'<'), hsu) == U'，';
    ok = ok && chewing_punctuation_for_key(symbol(U':'), hsu) == U'：';

    // Ctrl maps the Microsoft punctuation set, with or without Shift.
    InputKey ctrl_bang;
    ctrl_bang.sym = U'!';
    ctrl_bang.states = input_key_state(InputKeyState::Ctrl);
    ok = ok && chewing_punctuation_for_key(ctrl_bang, standard) == U'！';
    InputKey ctrl_comma;
    ctrl_comma.sym = U',';
    ctrl_comma.states = input_key_state(InputKeyState::Ctrl);
    ok = ok && chewing_punctuation_for_key(ctrl_comma, standard) == U'，';
    InputKey ctrl_letter;
    ctrl_letter.sym = U'a';
    ctrl_letter.states = input_key_state(InputKeyState::Ctrl);
    ok = ok && !chewing_punctuation_for_key(ctrl_letter, standard).has_value();

    // Alt/Super/Meta hand the key back to the application.
    for (const auto modifier : {InputKeyState::Alt, InputKeyState::Super, InputKeyState::Meta}) {
        InputKey key;
        key.sym = U',';
        key.states = input_key_state(modifier);
        ok = ok && !chewing_punctuation_for_key(key, standard).has_value();
        ok = ok && !chewing_punctuation_for_key(key, hsu).has_value();
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

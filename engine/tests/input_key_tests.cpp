#include "test_suites.h"

#include <cstdlib>

#include "input/input_key.hpp"

int run_input_key_tests() {
    using llavon::ime::input_key_state;
    using llavon::ime::InputKey;
    using llavon::ime::InputKeyState;
    using llavon::ime::is_shifted_ascii_symbol;
    using llavon::ime::keysym::Left;
    using llavon::ime::keysym::Tab;
    using llavon::ime::normalize_key;
    using llavon::ime::shifted_ascii_symbol;

    bool ok = true;

    ok = ok && shifted_ascii_symbol(U'1') == U'!';
    ok = ok && shifted_ascii_symbol(U'2') == U'@';
    ok = ok && shifted_ascii_symbol(U'3') == U'#';
    ok = ok && shifted_ascii_symbol(U'4') == U'$';
    ok = ok && shifted_ascii_symbol(U'5') == U'%';
    ok = ok && shifted_ascii_symbol(U'6') == U'^';
    ok = ok && shifted_ascii_symbol(U'7') == U'&';
    ok = ok && shifted_ascii_symbol(U'8') == U'*';
    ok = ok && shifted_ascii_symbol(U'9') == U'(';
    ok = ok && shifted_ascii_symbol(U'0') == U')';
    ok = ok && shifted_ascii_symbol(U'-') == U'_';
    ok = ok && shifted_ascii_symbol(U'=') == U'+';
    ok = ok && shifted_ascii_symbol(U'[') == U'{';
    ok = ok && shifted_ascii_symbol(U']') == U'}';
    ok = ok && shifted_ascii_symbol(U'\\') == U'|';
    ok = ok && shifted_ascii_symbol(U';') == U':';
    ok = ok && shifted_ascii_symbol(U'\'') == U'"';
    ok = ok && shifted_ascii_symbol(U',') == U'<';
    ok = ok && shifted_ascii_symbol(U'.') == U'>';
    ok = ok && shifted_ascii_symbol(U'/') == U'?';
    ok = ok && shifted_ascii_symbol(U'`') == U'~';

    // Letters, digits, and already shifted symbols pass through unchanged.
    ok = ok && shifted_ascii_symbol(U'a') == U'a';
    ok = ok && shifted_ascii_symbol(U'Z') == U'Z';
    ok = ok && shifted_ascii_symbol(U'7') == U'&';
    ok = ok && shifted_ascii_symbol(U'!') == U'!';
    ok = ok && shifted_ascii_symbol(U' ') == U' ';

    ok = ok && is_shifted_ascii_symbol(U'!');
    ok = ok && is_shifted_ascii_symbol(U'~');
    ok = ok && is_shifted_ascii_symbol(U'<');
    ok = ok && !is_shifted_ascii_symbol(U'1');
    ok = ok && !is_shifted_ascii_symbol(U',');
    ok = ok && !is_shifted_ascii_symbol(U'a');
    ok = ok && !is_shifted_ascii_symbol(U' ');

    // A folded shifted symbol counts as shifted even without the state.
    InputKey plain;
    plain.sym = U',';
    ok = ok && !plain.shifted();
    InputKey folded;
    folded.sym = U'<';
    ok = ok && folded.shifted();
    InputKey shifted_state;
    shifted_state.sym = U',';
    shifted_state.states = input_key_state(InputKeyState::Shift);
    ok = ok && shifted_state.shifted();

    // CapsLock and an empty state do not block; every other modifier does.
    InputKey key;
    ok = ok && !key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::CapsLock);
    ok = ok && !key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Shift);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Ctrl);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Alt);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Hyper);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Super);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Super2);
    ok = ok && key.has_blocking_modifier();
    key.states = input_key_state(InputKeyState::Meta);
    ok = ok && key.has_blocking_modifier();
    key.states = InputKeyState::CapsLock | InputKeyState::Shift | InputKeyState::Ctrl;
    ok = ok && key.has_blocking_modifier() && key.has(InputKeyState::Ctrl) && !key.has(InputKeyState::Alt);

    // The struct defaults describe a released-of-modifiers press.
    InputKey defaults;
    ok = ok && defaults.sym == 0 && defaults.states == 0 && !defaults.caps_lock && !defaults.release;

    // Shift normalization: fcitx5 folds Shift into the keysym while the macOS
    // host reports the unshifted keysym plus the Shift state; both shapes must
    // reach the rules identically.
    const auto shifted_key = [](char32_t symbol) {
        InputKey value;
        value.sym = symbol;
        value.states = input_key_state(InputKeyState::Shift);
        return value;
    };
    ok = ok && normalize_key(shifted_key(U'a')).sym == U'A';
    ok = ok && normalize_key(shifted_key(U'a')).states == 0;
    ok = ok && normalize_key(shifted_key(U'z')).sym == U'Z';
    ok = ok && normalize_key(shifted_key(U'2')).sym == U'@';
    ok = ok && normalize_key(shifted_key(U',')).sym == U'<';
    ok = ok && normalize_key(shifted_key(U',')).states == 0;
    ok = ok && normalize_key(shifted_key(U'`')).sym == U'~';
    // A keysym that already carries the shift keeps working, and the redundant
    // state is dropped.
    ok = ok && normalize_key(shifted_key(U'A')).sym == U'A';
    ok = ok && normalize_key(shifted_key(U'A')).states == 0;
    ok = ok && normalize_key(shifted_key(U'@')).sym == U'@';
    ok = ok && normalize_key(shifted_key(U'@')).states == 0;

    // Keys without a shifted form keep the state: marking and Shift+space
    // rules read it.
    ok = ok && normalize_key(shifted_key(U' ')).sym == U' ';
    ok = ok && normalize_key(shifted_key(U' ')).has(InputKeyState::Shift);
    ok = ok && normalize_key(shifted_key(Left)).sym == Left;
    ok = ok && normalize_key(shifted_key(Left)).has(InputKeyState::Shift);
    ok = ok && normalize_key(shifted_key(Tab)).has(InputKeyState::Shift);

    // fcitx5 also folds Shift out of BackSpace, Escape, Delete and the keypad
    // keys because their shifted form is already in the keysym.
    ok = ok && !normalize_key(shifted_key(0xff08)).has(InputKeyState::Shift); // BackSpace
    ok = ok && !normalize_key(shifted_key(0xff1b)).has(InputKeyState::Shift); // Escape
    ok = ok && !normalize_key(shifted_key(0xffff)).has(InputKeyState::Shift); // Delete
    ok = ok && !normalize_key(shifted_key(0xffb1)).has(InputKeyState::Shift); // KP_1
    ok = ok && !normalize_key(shifted_key(0xff8d)).has(InputKeyState::Shift); // KP_Enter
    // Navigation keys keep it, so Shift+arrows still mark text.
    ok = ok && normalize_key(shifted_key(0xff0d)).has(InputKeyState::Shift); // Return
    ok = ok && normalize_key(shifted_key(0xff63)).has(InputKeyState::Shift); // Insert

    // Shift combined with another modifier stays a shortcut.
    InputKey ctrl_shift;
    ctrl_shift.sym = U'a';
    ctrl_shift.states = InputKeyState::Shift | InputKeyState::Ctrl;
    ok = ok && normalize_key(ctrl_shift).sym == U'a';
    ok = ok && normalize_key(ctrl_shift).has(InputKeyState::Shift);
    ok = ok && normalize_key(ctrl_shift).has(InputKeyState::Ctrl);

    // Shortcut modifiers ignore the raw Meta bit the fcitx5 adapter merges in.
    InputKey meta;
    meta.states = input_key_state(InputKeyState::Meta);
    ok = ok && meta.has_blocking_modifier() && !meta.has_shortcut_modifier();
    InputKey plain_shift;
    plain_shift.states = input_key_state(InputKeyState::Shift);
    ok = ok && plain_shift.has_shortcut_modifier();

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

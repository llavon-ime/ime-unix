#include "raw_key_harness.hpp"

#include <algorithm>

using namespace llavon::ime::rawkey;

namespace {

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

}  // namespace

// ShiftBackSpaceRemovesTone follows macOS Zhuyin: Shift+Delete removes the last
// input, so a completed character goes back to its reading without the tone.
RAWKEY_SUITE("shift backspace removes the previous tone", shift_backspace_removes_tone) {
    {
        // Off by default: Shift+BackSpace deletes the character.
        Harness off;
        off.type("su3cl3");
        off.key("Shift+BackSpace");
        RAWKEY_ASSERT(off.preedit() == "你");
    }

    {
        Harness on;
        on.set_config("ShiftBackSpaceRemovesTone", "True");
        on.type("su3cl3");
        on.key("Shift+BackSpace");
        RAWKEY_ASSERT(on.preedit() == "你ㄏㄠ");
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄏㄠ");
        RAWKEY_ASSERT(!on.session()->buffer.segment_complete(1));
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");

        // Another tone completes it again.
        on.key(Key('4'));
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄏㄠˋ");
        RAWKEY_ASSERT(on.session()->buffer.segment_complete(1));

        // On an unfinished reading it behaves like BackSpace.
        on.key("Shift+BackSpace");
        on.key("Shift+BackSpace");
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄏ");

        // Plain BackSpace still deletes.
        on.key("BackSpace");
        on.key("BackSpace");
        RAWKEY_ASSERT(on.composition_empty());
    }

    {
        // The character before the caret is the one reopened.
        Harness middle;
        middle.set_config("ShiftBackSpaceRemovesTone", "True");
        middle.type("su3cl3");
        middle.key("Left");
        middle.key("Shift+BackSpace");
        RAWKEY_ASSERT(middle.session()->buffer.segment_reading(0) == u"ㄋㄧ");
        RAWKEY_ASSERT(middle.session()->buffer.segment_reading(1) == u"ㄏㄠˇ");
        middle.key(Key('4'));
        RAWKEY_ASSERT(middle.session()->buffer.segment_reading(0) == u"ㄋㄧˋ");
        RAWKEY_ASSERT(middle.session()->buffer.segments().size() == 2);
    }
}

// EscKeyCommitsComposingBuffer follows macOS Zhuyin: Escape commits the
// composition as shown instead of discarding it.
RAWKEY_SUITE("escape commits the composition", escape_commits_composition) {
    {
        Harness commit;
        commit.set_config("EscKeyCommitsComposingBuffer", "True");
        commit.type("su3cl3");
        const auto shown = commit.preedit();
        RAWKEY_ASSERT(!shown.empty());
        commit.key("Escape");
        RAWKEY_ASSERT(commit.composition_empty());
        RAWKEY_ASSERT(commit.last_commit() == shown);
    }

    {
        // An open candidate list closes first; the next Escape commits.
        Harness list;
        list.set_config("EscKeyCommitsComposingBuffer", "True");
        list.type("su3");
        list.key("Down");
        RAWKEY_ASSERT(list.has_candidates());
        const auto before = list.commits().size();
        list.key("Escape");
        RAWKEY_ASSERT(!list.has_candidates());
        RAWKEY_ASSERT(list.commits().size() == before);
        RAWKEY_ASSERT(list.preedit() == "你");
        list.key("Escape");
        RAWKEY_ASSERT(list.composition_empty());
        RAWKEY_ASSERT(list.last_commit() == "你");
    }

    {
        // It takes precedence over clearing the whole buffer.
        Harness both;
        both.set_configs({{"EscKeyClearsEntireComposingBuffer", "True"}, {"EscKeyCommitsComposingBuffer", "True"}});
        both.type("su3");
        both.key("Escape");
        RAWKEY_ASSERT(both.last_commit() == "你");
    }
}

// KeepInvalidReading follows macOS Zhuyin: a reading without characters stays
// in the composition, tone included, so it can be corrected in place.
RAWKEY_SUITE("invalid reading stays editable", keep_invalid_reading) {
    {
        // Off by default: the reading is discarded, earlier text stays.
        Harness off;
        off.type("su3xj83");
        RAWKEY_ASSERT(off.preedit() == "你");
        RAWKEY_ASSERT(off.session()->buffer.segments().size() == 1);
    }

    {
        Harness on;
        on.set_config("KeepInvalidReading", "True");
        on.type("su3xj83");
        RAWKEY_ASSERT(on.preedit() == "你ㄌㄨㄚˇ");
        RAWKEY_ASSERT(on.session()->buffer.segments().size() == 2);
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄌㄨㄚˇ");
        RAWKEY_ASSERT(!on.session()->buffer.segment_complete(1));
        RAWKEY_ASSERT(on.session()->buffer.has_unfinished_reading());

        // Backspace edits the kept reading, starting with its tone.
        on.key("BackSpace");
        RAWKEY_ASSERT(on.preedit() == "你ㄌㄨㄚ");

        // A wrong tone again keeps it; replacing the initial and retyping
        // the tone then composes a character.
        on.key(Key('3'));
        RAWKEY_ASSERT(on.preedit() == "你ㄌㄨㄚˇ");
        on.key(Key('c'));
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄏㄨㄚˇ");
        on.key(Key('4'));
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(1) == u"ㄏㄨㄚˋ");
        RAWKEY_ASSERT(on.session()->buffer.segment_complete(1));
        RAWKEY_ASSERT(on.preedit().find("ㄏ") == std::string::npos);
        on.expect_commit(on.preedit());
    }

    {
        // Enter commits the kept reading as typed.
        Harness commit;
        commit.set_config("KeepInvalidReading", "True");
        commit.type("xj83");
        RAWKEY_ASSERT(commit.preedit() == "ㄌㄨㄚˇ");
        commit.expect_commit("ㄌㄨㄚˇ");
    }
}

// ToneKeyOverwritesPrevious follows macOS Zhuyin: with no reading in progress,
// a tone key rewrites the tone of the character behind the caret.
RAWKEY_SUITE("tone key overwrites the previous character", tone_key_overwrites_previous) {
    {
        // Off by default: the existing reading is left untouched.
        Harness off;
        off.type("su3");
        RAWKEY_ASSERT(off.preedit() == "你");
        off.key(Key('4'));
        RAWKEY_ASSERT(off.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");
    }

    {
        Harness on;
        on.set_config("ToneKeyOverwritesPrevious", "True");
        on.type("su3");
        RAWKEY_ASSERT(on.preedit() == "你");

        on.key(Key('4'));
        RAWKEY_ASSERT(on.session()->buffer.segments().size() == 1);
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(0) == u"ㄋㄧˋ");
        RAWKEY_ASSERT(on.preedit() != "你");

        // The candidate list now belongs to the new reading.
        on.key("Down");
        RAWKEY_ASSERT(on.has_candidates());
        const auto candidates = on.candidates();
        RAWKEY_ASSERT(contains(candidates, "逆"));
        RAWKEY_ASSERT(!contains(candidates, "你"));
        on.key("Escape");

        // Another tone rewrites it again rather than stacking marks.
        on.key(Key('3'));
        RAWKEY_ASSERT(on.session()->buffer.segments().size() == 1);
        RAWKEY_ASSERT(on.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");
        on.expect_commit(on.preedit());
    }

    {
        // Only the character behind the caret changes.
        Harness phrase;
        phrase.set_config("ToneKeyOverwritesPrevious", "True");
        phrase.type("su3cl3");
        RAWKEY_ASSERT(phrase.session()->buffer.segments().size() == 2);
        phrase.key(Key('4'));
        RAWKEY_ASSERT(phrase.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");
        RAWKEY_ASSERT(phrase.session()->buffer.segment_reading(1) == u"ㄏㄠˋ");

        // Moving the caret retargets the rewrite to the earlier character.
        phrase.key("Left");
        phrase.key(Key('4'));
        RAWKEY_ASSERT(phrase.session()->buffer.segments().size() == 2);
        RAWKEY_ASSERT(phrase.session()->buffer.segment_reading(0) == u"ㄋㄧˋ");
        RAWKEY_ASSERT(phrase.session()->buffer.segment_reading(1) == u"ㄏㄠˋ");
    }

    {
        // A manual choice belonged to the old reading and is dropped.
        Harness chosen;
        chosen.set_config("ToneKeyOverwritesPrevious", "True");
        chosen.type("su3cl3");
        chosen.key("Left");
        chosen.key("Down");
        RAWKEY_ASSERT(chosen.has_candidates());
        chosen.choose_text("擬");
        RAWKEY_ASSERT(chosen.session()->buffer.segments()[0].manually_chosen);
        RAWKEY_ASSERT(chosen.session()->buffer.caret() == 1);
        chosen.key(Key('4'));
        RAWKEY_ASSERT(chosen.session()->buffer.segment_reading(0) == u"ㄋㄧˋ");
        RAWKEY_ASSERT(!chosen.session()->buffer.segments()[0].manually_chosen);
        RAWKEY_ASSERT(chosen.preedit().find("擬") == std::string::npos);
    }

    {
        // A reading without characters keeps the key's ordinary meaning.
        Harness invalid;
        invalid.set_config("ToneKeyOverwritesPrevious", "True");
        invalid.type("su3");
        invalid.key(Key('7'));
        RAWKEY_ASSERT(invalid.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");
    }

    {
        // A reading in progress takes the tone as usual.
        Harness composing;
        composing.set_config("ToneKeyOverwritesPrevious", "True");
        composing.type("su3cl");
        composing.key(Key('4'));
        RAWKEY_ASSERT(composing.session()->buffer.segments().size() == 2);
        RAWKEY_ASSERT(composing.session()->buffer.segment_reading(0) == u"ㄋㄧˇ");
        RAWKEY_ASSERT(composing.session()->buffer.segment_reading(1) == u"ㄏㄠˋ");
    }

    {
        // Smart English keeps its reversible raw-key handling for tone keys.
        Harness smart_off;
        smart_off.set_config("SmartEnglish", "True");
        smart_off.type("su3");
        smart_off.key(Key('4'));

        Harness smart;
        smart.set_config("SmartEnglish", "True");
        smart.set_config("ToneKeyOverwritesPrevious", "True");
        smart.type("su3");
        smart.key(Key('4'));
        RAWKEY_ASSERT(smart.preedit() == smart_off.preedit());
        RAWKEY_ASSERT(smart.session()->buffer.raw_composition() == smart_off.session()->buffer.raw_composition());
    }
}

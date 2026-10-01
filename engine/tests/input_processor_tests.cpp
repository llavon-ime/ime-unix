#include "test_suites.h"

#include <algorithm>
#include <cstdlib>

#include "input/input_processor.hpp"

int run_input_processor_tests() {
    using llavon::ime::CandidateKeyAction;
    using llavon::ime::CandidateKeyConfig;
    using llavon::ime::CandidateView;
    using llavon::ime::handle_candidate_key;
    using llavon::ime::input_key_state;
    using llavon::ime::InputKey;
    using llavon::ime::InputKeyState;
    using llavon::ime::selection_index_for_key;

    bool ok = true;

    constexpr auto kUp = llavon::ime::keysym::Up;
    constexpr auto kDown = llavon::ime::keysym::Down;
    constexpr auto kLeft = llavon::ime::keysym::Left;
    constexpr auto kRight = llavon::ime::keysym::Right;
    constexpr auto kHome = llavon::ime::keysym::Home;
    constexpr auto kEnd = llavon::ime::keysym::End;

    const auto key = [](char32_t symbol, std::uint32_t states = 0) {
        InputKey value;
        value.sym = symbol;
        value.states = states;
        return value;
    };
    const CandidateKeyConfig defaults;

    ok = ok && selection_index_for_key(U'a', "asdfghjkl", 9, true) == 0;
    ok = ok && selection_index_for_key(U's', "asdfghjkl", 9, true) == 1;
    ok = ok && selection_index_for_key(U'l', "asdfghjkl", 9, true) == 8;
    ok = ok && !selection_index_for_key(U'z', "asdfghjkl", 9, true).has_value();
    ok = ok && !selection_index_for_key(U'g', "asdfghjkl", 4, true).has_value();
    ok = ok && selection_index_for_key(U'f', "asdfghjkl", 4, true) == 3;
    ok = ok && selection_index_for_key(U'A', "asdfghjkl", 9, true) == 0;
    ok = ok && !selection_index_for_key(U'A', "asdfghjkl", 9, false).has_value();
    ok = ok && selection_index_for_key(U'1', "1234567890", 10, true) == 0;
    ok = ok && selection_index_for_key(U'0', "1234567890", 10, true) == 9;
    ok = ok && !selection_index_for_key(U'a', "", 0, true).has_value();

    // An empty list is not claimed by the candidate rules.
    CandidateView empty_view;
    ok = ok && handle_candidate_key(key(kUp), defaults, empty_view, 0, false, false).action ==
                   CandidateKeyAction::Unhandled;

    // Up/Down wrap inside the page; a single-row list consumes without redraw.
    CandidateView view;
    ok = ok && handle_candidate_key(key(kUp), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.cursor == 9 && view.page == 0;
    view.cursor = 9;
    ok = ok && handle_candidate_key(key(kDown), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.cursor == 0;
    view.cursor = 0;
    ok = ok && handle_candidate_key(key(kUp), defaults, view, 1, false, false).action ==
                   CandidateKeyAction::Handled &&
         view.cursor == 0;

    // Left/Right page while possible and otherwise leave the key to the app.
    view.reset();
    view.cursor = 13;
    ok = ok && handle_candidate_key(key(kLeft), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::PassThrough;
    ok = ok && handle_candidate_key(key(kRight), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.page == 1 && view.cursor == 13;
    ok = ok && handle_candidate_key(key(kRight), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.page == 2;
    ok = ok && handle_candidate_key(key(kRight), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::PassThrough;
    CandidateView single_page;
    ok = ok && handle_candidate_key(key(kRight), defaults, single_page, 5, false, false).action ==
                   CandidateKeyAction::PassThrough;

    // Home/End clamp to the list ends and consume without redraw once there.
    view.reset();
    ok = ok && handle_candidate_key(key(kEnd), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.cursor == 24 && view.page == 2;
    ok = ok && handle_candidate_key(key(kEnd), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Handled;
    ok = ok && handle_candidate_key(key(kHome), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Redraw &&
         view.cursor == 0 && view.page == 0;
    ok = ok && handle_candidate_key(key(kHome), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::Handled;

    // Return (both forms) activates the row under the cursor.
    ok = ok && handle_candidate_key(key(0xff0d), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::ActivateCursor;
    ok = ok && handle_candidate_key(key(0xff8d), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::ActivateCursor;

    // Space selects the current row without using Return's explicit mixed
    // candidate commit action.
    ok = ok && handle_candidate_key(key(U' '), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::SelectIndex;
    CandidateKeyConfig no_space;
    no_space.space_selects_candidate = false;
    ok = ok && handle_candidate_key(key(U' '), no_space, view, 25, false, false).action ==
                   CandidateKeyAction::Unhandled;

    // Digits select from the current page and stop under a blocking modifier.
    view.reset();
    ok = ok && handle_candidate_key(key(U'3'), defaults, view, 25, false, false).action ==
                   CandidateKeyAction::SelectIndex;
    CandidateKeyConfig digits;
    digits.selection_keys = "1234567890";
    auto outcome = handle_candidate_key(key(U'3'), digits, view, 25, false, false);
    ok = ok && outcome.action == CandidateKeyAction::SelectIndex && outcome.index == 2;
    view.page = 1;
    outcome = handle_candidate_key(key(U'3'), digits, view, 25, false, false);
    ok = ok && outcome.action == CandidateKeyAction::SelectIndex && outcome.index == 12;
    outcome = handle_candidate_key(key(U'3', input_key_state(InputKeyState::Ctrl)), digits, view, 25, false, false);
    ok = ok && outcome.action == CandidateKeyAction::SelectIndex && outcome.index == 12;
    view.reset();

    // A candidate list is explicitly opened with Down. Configured alphabetic
    // keys select their row there; normal mixed typing never opens it.
    CandidateKeyConfig home_row;
    home_row.selection_keys = "asdfghjkl";
    home_row.selection_key_count = 9;
    outcome = handle_candidate_key(key(U'l'), home_row, view, 25, false, false);
    ok = ok && outcome.action == CandidateKeyAction::SelectIndex && outcome.index == 8;
    outcome = handle_candidate_key(key(U'A'), home_row, view, 25, false, false);
    ok = ok && outcome.action == CandidateKeyAction::SelectIndex && outcome.index == 0;
    ok = ok && handle_candidate_key(key(U'a'), home_row, view, 25, true, false).action ==
                    CandidateKeyAction::SelectIndex;

    // Unmatched keys fall through; punctuation is swallowed while listing.
    ok = ok && handle_candidate_key(key(U'x'), home_row, view, 25, false, false).action ==
                   CandidateKeyAction::Unhandled;
    ok = ok && handle_candidate_key(key(U','), home_row, view, 25, false, true).action ==
                   CandidateKeyAction::Handled;
    ok = ok && handle_candidate_key(key(U','), home_row, view, 25, true, true).action ==
                   CandidateKeyAction::Unhandled;

    using llavon::ime::handle_symbol_menu_key;
    using llavon::ime::SymbolMenuKeyAction;
    using llavon::ime::SymbolMenuState;

    const auto menu_outcome = [](SymbolMenuState& menu, CandidateView& menu_view, const InputKey& pressed,
                                 std::size_t count = 13, const CandidateKeyConfig& config = CandidateKeyConfig{}) {
        return handle_symbol_menu_key(pressed, config, menu, menu_view, count);
    };

    SymbolMenuState menu;
    menu.open();
    CandidateView menu_view;

    // Escape and the menu key close the menu.
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Escape)).action ==
                   SymbolMenuKeyAction::CloseMenu;
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::grave)).action ==
                   SymbolMenuKeyAction::CloseMenu;

    // Backspace leaves a category level and closes from the root.
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::BackSpace)).action ==
                   SymbolMenuKeyAction::CloseMenu;
    char32_t ignored = 0;
    ok = ok && !menu.select(2, ignored);
    menu_view.cursor = 5;
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::BackSpace)).action ==
                   SymbolMenuKeyAction::Redraw &&
         !menu.in_category() && menu_view.cursor == 0;

    // Navigation keys move within the menu and are always consumed.
    menu_view.reset();
    ok = ok && menu_outcome(menu, menu_view, key(kUp)).action == SymbolMenuKeyAction::Redraw &&
         menu_view.cursor == 9;
    ok = ok && menu_outcome(menu, menu_view, key(kLeft)).action == SymbolMenuKeyAction::Handled;
    ok = ok && menu_outcome(menu, menu_view, key(kRight)).action == SymbolMenuKeyAction::Redraw &&
         menu_view.page == 1;
    ok = ok && menu_outcome(menu, menu_view, key(kRight)).action == SymbolMenuKeyAction::Handled;
    ok = ok && menu_outcome(menu, menu_view, key(kHome)).action == SymbolMenuKeyAction::Redraw &&
         menu_view.cursor == 0 && menu_view.page == 0;
    ok = ok && menu_outcome(menu, menu_view, key(kEnd)).action == SymbolMenuKeyAction::Redraw &&
         menu_view.cursor == 12;
    ok = ok && menu_outcome(menu, menu_view, key(kEnd)).action == SymbolMenuKeyAction::Handled;
    menu_view.reset();
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Page_Down)).action ==
                   SymbolMenuKeyAction::Redraw &&
         menu_view.page == 1 && menu_view.cursor == 10;
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Page_Down)).action ==
                   SymbolMenuKeyAction::Handled;
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Page_Up)).action ==
                   SymbolMenuKeyAction::Redraw &&
         menu_view.page == 0 && menu_view.cursor == 0;

    // Tab toggles the expanded page and redraws.
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Tab)).action ==
                   SymbolMenuKeyAction::Redraw &&
         menu_view.expanded;
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Tab)).action ==
                   SymbolMenuKeyAction::Redraw &&
         !menu_view.expanded;

    // Return, keypad enter, and Space activate the cursor row.
    menu_view.reset();
    ok = ok && menu_outcome(menu, menu_view, key(llavon::ime::keysym::Return)).action ==
                   SymbolMenuKeyAction::ActivateCursor;
    ok = ok && menu_outcome(menu, menu_view, key(0xff8d)).action == SymbolMenuKeyAction::ActivateCursor;
    ok = ok && menu_outcome(menu, menu_view, key(U' ')).action == SymbolMenuKeyAction::ActivateCursor;
    CandidateKeyConfig no_space_select;
    no_space_select.space_selects_candidate = false;
    ok = ok && menu_outcome(menu, menu_view, key(U' '), 13, no_space_select).action ==
                   SymbolMenuKeyAction::Handled;

    // Selection keys pick the matching row of the current page.
    ok = ok && menu_outcome(menu, menu_view, key(U'a'), 13, home_row).action ==
                   SymbolMenuKeyAction::SelectIndex;
    auto menu_selection = menu_outcome(menu, menu_view, key(U'a'), 13, home_row);
    ok = ok && menu_selection.index == 0;
    menu_view.page = 1;
    menu_selection = menu_outcome(menu, menu_view, key(U'a'), 13, home_row);
    ok = ok && menu_selection.index == 10;
    menu_view.reset();
    menu_selection = menu_outcome(menu, menu_view, key(U'3'), 13, digits);
    ok = ok && menu_selection.action == SymbolMenuKeyAction::SelectIndex && menu_selection.index == 2;

    // Unrelated keys are consumed without effects.
    ok = ok && menu_outcome(menu, menu_view, key(U'x'), 13, home_row).action == SymbolMenuKeyAction::Handled;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include <filesystem>

#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "input/input_processor.hpp"

int run_input_processor_process_tests() {
    using llavon::ime::Config;
    using llavon::ime::default_config;
    using llavon::ime::FallbackEngine;
    using llavon::ime::InputKey;
    using llavon::ime::InputProcessor;
    using llavon::ime::InputResetReason;
    using llavon::ime::InputSession;
    using llavon::ime::MixedInputDecoder;
    using llavon::ime::PhraseOverrideStore;
    using llavon::ime::protocol::Prediction;

    bool ok = true;

    FallbackEngine fallback(std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH));
    MixedInputDecoder decoder([&fallback](std::u16string_view reading) { return fallback.lookup(reading); },
                              [&fallback](std::u16string_view word) { return fallback.latin_frequency(word); });
    PhraseOverrideStore overrides(std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path() /
                                  "unused_phrase_overrides.txt");
    InputProcessor processor(fallback, decoder, overrides);

    Config config = default_config();
    config.smart_english = false;
    InputSession session;

    const auto key = [](char32_t symbol, std::uint32_t states = 0) {
        InputKey value;
        value.sym = symbol;
        value.states = states;
        return value;
    };

    // Typing a full syllable completes a segment and asks for a prediction;
    // the fallback candidates are ready before the model answers.
    bool prediction_requested = false;
    for (const char32_t symbol : {u's', u'u', u'3'}) {
        const auto effect = processor.process(key(symbol), session, config);
        ok = ok && effect.handled && effect.redraw;
        prediction_requested = prediction_requested || effect.request_prediction;
    }
    ok = ok && prediction_requested;
    ok = ok && session.buffer.segments().size() == 1 && session.buffer.segments().front().complete();
    ok = ok && session.buffer.segment_candidates(0) != nullptr && !session.buffer.segment_candidates(0)->empty();

    // A model prediction merges into the same segment and must keep the
    // table's homophones, otherwise the user cannot pick another character.
    session.prediction.segment_indices = {0};
    Prediction prediction;
    prediction.candidates = {{U'你', U'擬'}};
    processor.apply_prediction(session, prediction);
    ok = ok && !session.buffer.segment_candidates(0)->empty();
    {
        const auto* merged_candidates = session.buffer.segment_candidates(0);
        ok = ok && merged_candidates != nullptr &&
             std::find(merged_candidates->begin(), merged_candidates->end(), U'妳') != merged_candidates->end();
    }

    // Candidate routing must work before any frontend renders the panel. The
    // processor opens the list, then selects directly from semantic state.
    const auto open = processor.process(key(U' '), session, config);
    ok = ok && open.handled && open.redraw && session.choosing_candidate();
    ok = ok && session.displayed_candidates.empty();
    const auto select = processor.process(key(U'1'), session, config);
    ok = ok && select.handled && select.redraw && !session.choosing_candidate();

    // Return commits the selected candidate.
    const auto commit = processor.process(key(llavon::ime::keysym::Return), session, config);
    ok = ok && commit.handled && commit.commit == std::u16string(u"你");
    ok = ok && session.buffer.empty() && !session.prediction.pending;

    // Escape with an empty composition is not claimed by the rules.
    const auto escape = processor.process(key(llavon::ime::keysym::Escape), session, config);
    ok = ok && !escape.handled;

    // CapsLock pass-through resets the composition without consuming the key.
    InputSession capped;
    (void)processor.process(key(u's'), capped, config);
    InputKey caps_locked = key(u's');
    caps_locked.caps_lock = true;
    Config no_caps_chinese = config;
    no_caps_chinese.caps_lock_inputs_bopomofo = false;
    const auto reset = processor.process(caps_locked, capped, no_caps_chinese);
    ok = ok && !reset.handled && capped.buffer.empty();

    processor.reset_state(session, config);
    ok = ok && session.empty();
    processor.sync_state(session, config);
    ok = ok && session.empty();

    InputSession focus_out;
    for (const char32_t symbol : {u's', u'u', u'3'}) {
        (void)processor.process(key(symbol), focus_out, config);
    }
    const auto generation = focus_out.prediction.generation;
    const auto focus_effect = processor.reset(focus_out, config, InputResetReason::FocusOut, false);
    ok = ok && focus_effect.commit == std::u16string(u"你") && focus_effect.redraw;
    ok = ok && focus_out.empty() && focus_out.buffer.empty();
    ok = ok && focus_out.prediction.generation == generation + 1;

    InputSession unfinished;
    for (const char32_t symbol : {u's', u'u'}) {
        (void)processor.process(key(symbol), unfinished, config);
    }
    const auto unfinished_effect = processor.reset(unfinished, config, InputResetReason::FocusOut, false);
    ok = ok && unfinished_effect.commit.empty() && unfinished.empty();

    InputSession changed;
    Config smart = config;
    smart.smart_english = true;
    for (const char32_t symbol : std::u32string(U"hello")) {
        (void)processor.process(key(symbol), changed, smart);
    }
    ok = ok && !changed.pending_token.empty();
    const auto config_generation = changed.prediction.generation;
    processor.prepare_for_config_change(changed);
    ok = ok && changed.pending_token.empty() && !changed.mixed_decision.active();
    ok = ok && changed.buffer.commit_text() == std::u16string(u"hello");
    ok = ok && changed.prediction.generation == config_generation + 1;

    // A phrase override pins the whole composition, and the pin must survive
    // both model predictions and further typing. Editing the pinned readings
    // or picking another candidate releases it.
    {
        ok = ok && overrides.replace({});
        const std::vector<std::u16string> readings{u"ㄋㄧˇ", u"ㄏㄠˇ"};
        ok = ok && overrides.add(u"你好", readings);

        InputSession pinned;
        for (const char32_t symbol : std::u32string(U"su3cl3")) {
            (void)processor.process(key(symbol), pinned, config);
        }
        ok = ok && pinned.buffer.segments().size() == 2;
        processor.apply_phrase_override(pinned);
        ok = ok && pinned.buffer.rendered_composition() == std::u16string(u"你好");
        ok = ok && pinned.buffer.segments()[0].phrase_override_chosen;
        ok = ok && pinned.buffer.segments()[1].phrase_override_chosen;

        // Model predictions refresh alternatives without dropping the pin.
        pinned.prediction.segment_indices = {0, 1};
        Prediction pinned_prediction;
        pinned_prediction.candidates = {{U'擬'}, {U'號'}};
        processor.apply_prediction(pinned, pinned_prediction);
        processor.apply_phrase_override(pinned);
        ok = ok && pinned.buffer.rendered_composition().rfind(u"你好", 0) == 0;
        {
            // The pinned segment keeps the table's homophones, so the user
            // can still replace the forced character by selecting another
            // candidate.
            const auto* forced_candidates = pinned.buffer.segment_candidates(0);
            ok = ok && forced_candidates != nullptr &&
                 std::find(forced_candidates->begin(), forced_candidates->end(), U'妳') != forced_candidates->end();
        }

        // Typing more syllables keeps the pinned prefix for the rest of the
        // composition instead of reverting to model output.
        for (const char32_t symbol : std::u32string(U"su3")) {
            (void)processor.process(key(symbol), pinned, config);
        }
        ok = ok && pinned.buffer.segments().size() == 3;
        processor.apply_phrase_override(pinned);
        ok = ok && pinned.buffer.segments()[0].phrase_override_chosen;
        ok = ok && pinned.buffer.segments()[1].phrase_override_chosen;
        pinned.prediction.segment_indices = {0, 1, 2};
        Prediction extended_prediction;
        extended_prediction.candidates = {{U'擬'}, {U'號'}, {U'泥'}};
        processor.apply_prediction(pinned, extended_prediction);
        processor.apply_phrase_override(pinned);
        ok = ok && pinned.buffer.rendered_composition().rfind(u"你好", 0) == 0;
        ok = ok && pinned.buffer.rendered_composition().size() == 3;

        // Selecting another candidate releases the whole pin.
        ok = ok && pinned.buffer.select_candidate(0, 1, false);
        processor.apply_phrase_override(pinned);
        ok = ok && !pinned.buffer.segments()[1].phrase_override_chosen;

        // Editing inside the pinned readings releases it as well.
        InputSession edited;
        for (const char32_t symbol : std::u32string(U"su3cl3")) {
            (void)processor.process(key(symbol), edited, config);
        }
        processor.apply_phrase_override(edited);
        ok = ok && edited.buffer.segments()[0].phrase_override_chosen;
        (void)processor.process(key(llavon::ime::keysym::BackSpace), edited, config);
        processor.apply_phrase_override(edited);
        ok = ok && !edited.buffer.segments()[0].phrase_override_chosen;

        // The phrase applies wherever its readings appear in the
        // composition, not only when it covers the whole sequence.
        InputSession middle;
        for (const char32_t symbol : std::u32string(U"su3su3cl3")) {
            (void)processor.process(key(symbol), middle, config);
        }
        ok = ok && middle.buffer.segments().size() == 3;
        processor.apply_phrase_override(middle);
        ok = ok && !middle.buffer.segments()[0].phrase_override_chosen;
        ok = ok && middle.buffer.segments()[1].phrase_override_chosen;
        ok = ok && middle.buffer.segments()[2].phrase_override_chosen;
        ok = ok && middle.buffer.rendered_composition() == std::u16string(u"你你好");
        middle.prediction.segment_indices = {0, 1, 2};
        Prediction middle_prediction;
        middle_prediction.candidates = {{U'擬'}, {U'擬'}, {U'號'}};
        processor.apply_prediction(middle, middle_prediction);
        processor.apply_phrase_override(middle);
        ok = ok && middle.buffer.rendered_composition() == std::u16string(u"擬你好");

        ok = ok && overrides.replace({});
        std::error_code cleanup_error;
        std::filesystem::remove(overrides.path(), cleanup_error);
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

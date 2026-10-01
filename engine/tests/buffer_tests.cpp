#include "test_suites.h"

#include <cstdlib>
#include <string>

#include "bopomofo/keymap.hpp"
#include "buffer/composition_buffer.hpp"
#include "engine/fallback_engine.hpp"

namespace {

void apply_fallback_to_last_segment(llavon::ime::CompositionBuffer& buffer, const llavon::ime::FallbackEngine& fallback) {
    const auto segment = buffer.last_edited_segment();
    if (!segment || !buffer.segment_complete(*segment)) return;

    const auto predictions = fallback.predict(buffer);
    if (*segment < predictions.size()) {
        (void)buffer.set_segment_candidates(*segment, predictions[*segment].candidates, false);
        const auto* candidates = buffer.segment_candidates(*segment);
        if (candidates == nullptr || candidates->empty()) (void)buffer.remove_segment(*segment);
    }
}

bool type_keys(llavon::ime::CompositionBuffer& buffer, const llavon::ime::FallbackEngine& fallback,
               const std::u32string& keys) {
    for (const char32_t key : keys) {
        const auto mapped = llavon::ime::lookup_bopomofo_key(key);
        if (!mapped || !buffer.add_bopomofo(*mapped)) return false;
        if (llavon::ime::is_bopomofo_tone(*mapped)) apply_fallback_to_last_segment(buffer, fallback);
    }
    return true;
}

bool type_hsu_keys(llavon::ime::CompositionBuffer& buffer, const llavon::ime::FallbackEngine& fallback,
                   const std::u32string& keys) {
    for (const char32_t key : keys) {
        const auto input = buffer.add_bopomofo_key(key, llavon::ime::BopomofoKeyboardLayout::Hsu);
        if (!input) return false;
        if (input->completed) apply_fallback_to_last_segment(buffer, fallback);
    }
    return true;
}

}  // namespace

int run_buffer_tests() {
    bool ok = true;

    llavon::ime::CompositionBuffer buffer;
    ok = ok && buffer.empty();
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U'ˇ');
    ok = ok && !buffer.empty();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧ");
    buffer.clear();
    ok = ok && buffer.empty();

    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U'ˇ');
    llavon::ime::FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
    const auto initial_predictions = fallback.predict(buffer);
    ok = ok && initial_predictions.size() == 1;
    ok = ok && !initial_predictions.front().candidates.empty();

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇㄏㄠˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好");
    ok = ok && buffer.commit_text() == std::u16string(u"你好");
    ok = ok && buffer.completed_segment_indices().size() == 2;

    // Phrase overrides pin each character for prediction without masquerading as
    // an explicit candidate choice. An explicit choice still wins afterward.
    ok = ok && buffer.apply_phrase_override(std::u32string_view(U"歐陽"));
    ok = ok && buffer.rendered_composition() == std::u16string(u"歐陽");
    ok = ok && buffer.segments()[0].manually_chosen && buffer.segments()[0].phrase_override_chosen;
    ok = ok && !buffer.manually_chosen_segment_at_caret().has_value();
    ok = ok && !buffer.set_segment_candidates(0, {U'你'}, true);
    // Refreshing candidates keeps the pin: alternatives are replaced while the
    // forced character stays selected at the front.
    ok = ok && buffer.refresh_segment_candidates(0, {U'擬'});
    ok = ok && buffer.segments()[0].selected_candidate() == U'歐';
    ok = ok && buffer.segments()[0].phrase_override_chosen;
    ok = ok && buffer.rendered_composition() == std::u16string(u"歐陽");
    ok = ok && buffer.clear_phrase_override_choices();
    ok = ok && !buffer.segments()[0].manually_chosen && !buffer.segments()[0].phrase_override_chosen;
    ok = ok && buffer.select_candidate(0, 0, false);
    ok = ok && !buffer.refresh_segment_candidates(0, {U'你'});
    ok = ok && !buffer.apply_phrase_override(std::u32string_view(U"宇文"));
    ok = ok && buffer.rendered_composition() == std::u16string(u"歐陽");

    // Range apply pins only the requested segments, so a stored phrase can
    // sit anywhere in the composition.
    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3su3");
    ok = ok && buffer.segments().size() == 3;
    ok = ok && buffer.apply_phrase_override(1, std::u32string_view(U"歐陽"));
    ok = ok && !buffer.segments()[0].phrase_override_chosen;
    ok = ok && buffer.segments()[1].phrase_override_chosen;
    ok = ok && buffer.segments()[2].phrase_override_chosen;
    ok = ok && buffer.rendered_composition() == std::u16string(u"你歐陽");
    ok = ok && buffer.clear_phrase_override_choices(1, 2);
    ok = ok && !buffer.segments()[1].phrase_override_chosen;

    // Marking selects a range of segments for phrase override storage: the caret
    // moves while the anchor stays, plain cursor moves and edits cancel it.
    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && !buffer.marked_range().has_value();
    ok = ok && buffer.extend_selection(-1);
    ok = ok && buffer.marked_range() == std::make_pair<size_t, size_t>(1, 2);
    ok = ok && buffer.marked_text() == std::u16string(u"好");
    ok = ok && buffer.marked_readings().size() == 1;
    ok = ok && buffer.extend_selection(-1);
    ok = ok && buffer.marked_range() == std::make_pair<size_t, size_t>(0, 2);
    ok = ok && buffer.marked_text() == std::u16string(u"你好");
    const auto marked = buffer.marked_readings();
    ok = ok && marked.size() == 2 && marked[0] == std::u16string(u"ㄋㄧˇ") && marked[1] == std::u16string(u"ㄏㄠˇ");
    ok = ok && buffer.extend_selection(1);
    ok = ok && buffer.marked_range() == std::make_pair<size_t, size_t>(1, 2);
    ok = ok && buffer.clear_selection();
    ok = ok && !buffer.marked_range().has_value();
    ok = ok && buffer.caret() == 2;
    ok = ok && !buffer.clear_selection();

    // Cancelling a mark restores its original caret. Plain cursor movement and
    // text edits still cancel it.
    ok = ok && buffer.extend_selection(-1);
    ok = ok && buffer.move_cursor_right();
    ok = ok && !buffer.marked_range().has_value();
    ok = ok && buffer.extend_selection(-1);
    ok = ok && buffer.backspace();
    ok = ok && !buffer.marked_range().has_value();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄏㄠˇ");

    // Chewing inserts Hsu's unmapped top-row digits into completed composition.
    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && buffer.add_literal(U'0');
    ok = ok && buffer.add_literal(U'！');
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好0！");

    // Digits bell instead of changing an unfinished Hsu syllable.
    buffer.clear();
    ok = ok && buffer.add_bopomofo_key(U'm', llavon::ime::BopomofoKeyboardLayout::Hsu).has_value();
    ok = ok && !buffer.add_bopomofo_key(U'0', llavon::ime::BopomofoKeyboardLayout::Hsu).has_value();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄇ");

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"SU3CL3");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好");

    const auto target = buffer.candidate_target(llavon::ime::CandidateTarget::BeforeCursor);
    ok = ok && target && *target == 1;
    ok = ok && !buffer.manually_chosen_segment_at_caret().has_value();
    const auto* target_candidates = target ? buffer.segment_candidates(*target) : nullptr;
    if (target_candidates != nullptr && target_candidates->size() > 1) {
        ok = ok && buffer.select_candidate(*target, 1, false);
        ok = ok && buffer.manually_chosen_segment_at_caret() == target;
        ok = ok && !buffer.empty();
        ok = ok && buffer.rendered_composition().size() == 2;
    }

    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你");
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.add_bopomofo(U' ');
    ok = ok && buffer.segment_complete(0);

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ˋ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ˋ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"ˋ");
    ok = ok && !buffer.segment_complete(0);
    ok = ok && buffer.add_bopomofo(U'ㄧ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄧˋ");

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"4u4");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄧˋ");
    ok = ok && buffer.rendered_composition().size() == 1;

    // The production raw-key API preserves permissive Standard tone-first
    // input and only finalizes on the last tone key.
    buffer.clear();
    auto standard_input = buffer.add_bopomofo_key(U'4', llavon::ime::BopomofoKeyboardLayout::Standard);
    ok = ok && standard_input && !standard_input->completed;
    standard_input = buffer.add_bopomofo_key(U'u', llavon::ime::BopomofoKeyboardLayout::Standard);
    ok = ok && standard_input && !standard_input->completed;
    ok = ok && !buffer.segment_complete(0);
    ok = ok && buffer.completed_segment_indices().empty();
    const auto pending_standard_predictions = fallback.predict(buffer);
    ok = ok && pending_standard_predictions.size() == 1;
    ok = ok && pending_standard_predictions.front().candidates.empty();
    ok = ok && pending_standard_predictions.front().raw_text == std::u16string(u"ㄧˋ");
    // A delayed response must not make an unfinalized reading visible or stop
    // the final tone from editing the same segment.
    ok = ok && buffer.set_segment_candidates(0, {U'意'}, false);
    standard_input = buffer.add_bopomofo_key(U'4', llavon::ime::BopomofoKeyboardLayout::Standard);
    ok = ok && standard_input && standard_input->completed;
    apply_fallback_to_last_segment(buffer, fallback);
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄧˋ");
    ok = ok && buffer.segments().size() == 1;
    ok = ok && buffer.rendered_composition().size() == 1;

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"su3cl3");
    ok = ok && buffer.move_cursor_left();
    ok = ok && buffer.candidate_target(llavon::ime::CandidateTarget::BeforeCursor) &&
         *buffer.candidate_target(llavon::ime::CandidateTarget::BeforeCursor) == 0;
    ok = ok && buffer.candidate_target(llavon::ime::CandidateTarget::AfterCursor) &&
         *buffer.candidate_target(llavon::ime::CandidateTarget::AfterCursor) == 1;

    ok = ok && buffer.delete_forward();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你");
    ok = ok && buffer.caret() == 1;
    ok = ok && !buffer.delete_forward();

    buffer.clear();
    ok = ok && type_keys(buffer, fallback, U"1m3");
    ok = ok && buffer.empty();
    ok = ok && type_keys(buffer, fallback, U"su3");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你");

    ok = ok && buffer.add_bopomofo(U'ˋ');
    ok = ok && buffer.commit_text() == std::u16string(u"你ˋ");
    ok = ok && buffer.candidate_commit_text() == std::u16string(u"你");

    buffer.clear();
    ok = ok && buffer.add_literal(U'。');
    ok = ok && buffer.rendered_composition() == std::u16string(u"。");
    ok = ok && buffer.commit_text() == std::u16string(u"。");
    ok = ok && buffer.candidate_commit_text() == std::u16string(u"。");
    ok = ok && !buffer.has_unfinished_reading();
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    buffer.clear();
    ok = ok && buffer.add_bopomofo(U'ㄑ');
    ok = ok && buffer.add_bopomofo(U'ㄎ');
    ok = ok && buffer.add_bopomofo(U'ㄇ');
    ok = ok && buffer.add_bopomofo(U'ㄨ');
    ok = ok && buffer.add_bopomofo(U'ㄋ');
    ok = ok && buffer.add_bopomofo(U'ㄑ');
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄑㄨ");
    ok = ok && buffer.segments().size() == 1;

    // Hsu layout through physical keys.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"nefhwf");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄋㄧˇㄏㄠˇ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"你好");
    ok = ok && buffer.commit_text() == std::u16string(u"你好");
    ok = ok && buffer.completed_segment_indices().size() == 2;

    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen kxjen jn dgshnfbkj");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ㄎㄨˋㄧㄣ ㄓㄣ ㄉㄜ˙ㄏㄣˇㄅㄤˋ");

    // Space with no composition is rejected by the buffer so the engine can
    // pass it through to the client.
    buffer.clear();
    ok = ok && !buffer.add_bopomofo_key(U' ', llavon::ime::BopomofoKeyboardLayout::Hsu).has_value();
    ok = ok && buffer.empty();

    // After a completed syllable, d starts a new ㄉ syllable instead of adding
    // a tone to the completed one.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen ");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ");
    ok = ok && type_hsu_keys(buffer, fallback, U"d");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ㄉ");
    ok = ok && !buffer.segment_complete(1);
    ok = ok && buffer.segments().size() == 2;

    // A finalized segment remains finalized when an empty model response
    // removes its candidates.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen ");
    ok = ok && buffer.set_segment_candidates(0, {}, false);
    ok = ok && !buffer.has_unfinished_reading();
    ok = ok && !buffer.has_unfinished_reading_before_caret();
    const auto after_empty_candidates =
        buffer.add_bopomofo_key(U'd', llavon::ime::BopomofoKeyboardLayout::Hsu);
    ok = ok && after_empty_candidates && after_empty_candidates->segment_index == 1;
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ㄉ");
    ok = ok && buffer.segments().size() == 2;

    // Esc with whole-buffer clearing disabled removes only the unfinished
    // reading adjacent to the caret.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen k");
    ok = ok && buffer.has_unfinished_reading_before_caret();
    ok = ok && buffer.move_cursor_left();
    ok = ok && !buffer.has_unfinished_reading_before_caret();
    ok = ok && buffer.clear_unfinished_reading();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ");
    ok = ok && buffer.segments().size() == 1;
    ok = ok && !buffer.has_unfinished_reading_before_caret();
    ok = ok && !buffer.clear_unfinished_reading();

    // A manual choice remains discoverable after after-cursor selection moves
    // the caret past the selected segment.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen ");
    ok = ok && buffer.move_cursor_left();
    const auto after_cursor_target = buffer.candidate_target(llavon::ime::CandidateTarget::AfterCursor);
    ok = ok && after_cursor_target && *after_cursor_target == 0;
    ok = ok && buffer.select_candidate(*after_cursor_target, 0, true);
    ok = ok && buffer.caret() == 1;
    ok = ok && buffer.manually_chosen_segment_at_caret() == after_cursor_target;
    ok = ok && buffer.cancel_candidate_selection(*after_cursor_target);
    ok = ok && !buffer.manually_chosen_segment_at_caret().has_value();

    // Backspace reopens a finalized reading even when it has no visible
    // candidate, and clears alternatives from the old completion.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"a ");
    ok = ok && buffer.set_segment_candidates(0, {}, false);
    ok = ok && buffer.backspace();
    ok = ok && buffer.has_unfinished_reading_before_caret();
    ok = ok && !buffer.segments().front().reading_finalized;
    ok = ok && buffer.segments().front().alternative_readings.empty();
    const auto recompleted = buffer.add_bopomofo_key(U'f', llavon::ime::BopomofoKeyboardLayout::Hsu);
    ok = ok && recompleted && recompleted->completed;
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄘˇ");
    ok = ok && buffer.segments().size() == 1;

    // Semantic edits after an automatic conversion: backspace twice from
    // ㄒㄧㄤ and type k again.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cke");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄤ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒ");
    ok = ok && type_hsu_keys(buffer, fallback, U"k");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄕㄤ");

    // Backspace removes final, medial, then initial structurally.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cke");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄤ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒ");
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    // Backspace over a visible candidate removes the entire segment.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen ");
    ok = ok && buffer.rendered_composition() == std::u16string(u"心");
    ok = ok && buffer.backspace();
    ok = ok && buffer.empty();

    // Moving the caret edits the segment before the caret or inserts at the
    // caret.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"cen ");
    ok = ok && type_hsu_keys(buffer, fallback, U"k");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧㄣ ㄎ");
    ok = ok && buffer.move_cursor_left();
    ok = ok && buffer.move_cursor_left();
    ok = ok && type_hsu_keys(buffer, fallback, U"d");
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄉㄒㄧㄣ ㄎ");
    ok = ok && buffer.caret() == 1;

    // Rejected keys leave the buffer untouched.
    buffer.clear();
    const auto revision_before = buffer.revision();
    ok = ok && type_hsu_keys(buffer, fallback, U"ce");
    ok = ok && !buffer.add_bopomofo_key(U'q', llavon::ime::BopomofoKeyboardLayout::Hsu).has_value();
    ok = ok && buffer.raw_composition() == std::u16string(u"ㄒㄧ");
    ok = ok && buffer.revision() == revision_before + 2;

    // Alternative candidates: primary candidates stay first, alternatives are
    // appended in stored order, and duplicates are removed.
    llavon::ime::TableEngine table(LLAVON_IME_TEST_TABLE_PATH);
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"a ");
    ok = ok && buffer.segment_complete(0);
    {
        const auto predictions = fallback.predict(buffer);
        ok = ok && predictions.size() == 1;
        const auto& candidates = predictions.front().candidates;
        const auto primary = table.lookup(u"ㄘ ");
        ok = ok && !primary.empty();
        ok = ok && candidates.size() >= primary.size() + 1;
        for (size_t i = 0; i < primary.size(); ++i) {
            ok = ok && candidates[i] == primary[i];
        }
        bool found_alternative = false;
        for (const auto candidate : candidates) {
            if (candidate == U'ㄟ') found_alternative = true;
        }
        ok = ok && found_alternative;
    }

    // u"ㄧ " contributes candidates from u"ㄝ ".
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"e ");
    {
        const auto predictions = fallback.predict(buffer);
        ok = ok && predictions.size() == 1;
        bool found_alternative = false;
        for (const auto candidate : predictions.front().candidates) {
            if (candidate == U'ㄝ') found_alternative = true;
        }
        ok = ok && found_alternative;
    }

    // u"ㄦ " contributes candidates from u"ㄌ " and u"ㄥ " in declared order.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"l ");
    {
        const auto predictions = fallback.predict(buffer);
        ok = ok && predictions.size() == 1;
        const auto& candidates = predictions.front().candidates;
        const auto le_candidates = table.lookup(u"ㄌ ");
        const auto eng_candidates = table.lookup(u"ㄥ ");
        ok = ok && !le_candidates.empty() && !eng_candidates.empty();
        size_t le_position = candidates.size();
        size_t eng_position = candidates.size();
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (le_position == candidates.size() && candidates[i] == le_candidates.front()) le_position = i;
            if (eng_position == candidates.size() && candidates[i] == eng_candidates.front()) eng_position = i;
        }
        ok = ok && le_position < eng_position;
    }

    // Tone-only alternatives are looked up without a trailing space.
    buffer.clear();
    ok = ok && type_hsu_keys(buffer, fallback, U"s ");
    {
        const auto predictions = fallback.predict(buffer);
        ok = ok && predictions.size() == 1;
        bool found_alternative = false;
        for (const auto candidate : predictions.front().candidates) {
            if (candidate == U'˙') found_alternative = true;
        }
        ok = ok && found_alternative;
    }

    // Direct helper tests: deduplication and empty-primary fallback.
    {
        llavon::ime::Segment segment;
        segment.alternative_readings = {u"ㄟ ", u"ㄟ "};
        auto merged = fallback.append_alternative_candidates(segment, {U'疵'});
        ok = ok && merged == std::vector<char32_t>({U'疵', U'ㄟ'});
    }
    {
        llavon::ime::Segment segment;
        segment.alternative_readings = {u"ㄧ "};
        auto merged = fallback.append_alternative_candidates(segment, {});
        ok = ok && !merged.empty();
        ok = ok && merged.front() == U'一';
    }
    {
        llavon::ime::Segment segment;
        auto merged = fallback.append_alternative_candidates(segment, {U'你'});
        ok = ok && merged == std::vector<char32_t>({U'你'});
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

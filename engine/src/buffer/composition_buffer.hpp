#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bopomofo/keymap.hpp"
#include "bopomofo/syllable.hpp"

namespace llavon::ime {

enum class CandidateTarget {
    BeforeCursor,
    AfterCursor,
};

struct Segment {
    Syllable syllable;
    char32_t literal = 0;
    std::vector<char32_t> candidates;
    size_t selected_index = 0;
    bool manually_chosen = false;
    bool phrase_override_chosen = false;
    // Parser completion is distinct from a structurally complete syllable.
    bool reading_finalized = false;
    std::vector<std::u16string> alternative_readings;

    bool complete() const noexcept;
    bool empty() const noexcept;
    bool visible_candidate() const noexcept;
    char32_t selected_candidate() const noexcept;
    std::u16string reading() const;
    std::u16string rendered_text() const;
};

struct BopomofoInputResult {
    size_t segment_index = 0;
    bool completed = false;
    bool natural_extension = false;
};

class CompositionBuffer {
public:
    bool add_bopomofo(char32_t symbol);
    std::optional<BopomofoInputResult> add_bopomofo_key(
        char32_t key,
        BopomofoKeyboardLayout layout,
        bool accept_uppercase = true);
    std::optional<BopomofoInputResult> add_bopomofo_keys(
        std::u16string_view keys,
        char32_t tone_key,
        BopomofoKeyboardLayout layout,
        bool strict,
        std::u16string_view expected_reading = {});
    bool add_literal(char32_t symbol);
    bool backspace();
    bool delete_forward();
    bool move_cursor_left();
    bool move_cursor_right();
    // McBopomofo-style marking: the caret moves as usual, but the anchor stays
    // where the marking started, so the range between them can be stored as a
    // phrase override. Any text edit or plain cursor move cancels the mark.
    bool extend_selection(int delta);
    bool clear_selection();
    std::optional<std::pair<size_t, size_t>> marked_range() const;
    std::u16string marked_text() const;
    std::vector<std::u16string> marked_readings() const;
    void clear();

    bool empty() const noexcept;
    bool has_unfinished_reading() const noexcept;
    bool has_unfinished_reading_before_caret() const noexcept;
    bool clear_unfinished_reading();
    std::u16string raw_composition() const;
    std::u16string rendered_composition() const;
    std::u16string rendered_prefix_before_caret() const;
    std::u16string commit_text() const;
    std::u16string candidate_commit_text() const;
    std::optional<size_t> candidate_target(CandidateTarget target) const;
    std::optional<size_t> last_edited_segment() const noexcept;
    size_t caret() const noexcept;
    bool caret_at_end() const noexcept;
    size_t revision() const noexcept;
    const std::vector<Segment>& segments() const noexcept;
    std::vector<size_t> completed_segment_indices() const;

    bool segment_complete(size_t index) const;
    std::u16string segment_reading(size_t index) const;
    const std::vector<char32_t>* segment_candidates(size_t index) const;
    std::optional<size_t> segment_selected_index(size_t index) const;
    std::optional<size_t> manually_chosen_segment_at_caret() const noexcept;
    bool set_segment_candidates(size_t index, std::vector<char32_t> candidates, bool preserve_manual_choice = true);
    // Refreshes fallback/model candidates without dropping a phrase override
    // pin: the pinned character stays selected at the front. Explicit manual
    // choices are preserved like set_segment_candidates(..., true).
    bool refresh_segment_candidates(size_t index, std::vector<char32_t> candidates);
    bool apply_phrase_override(std::span<const char32_t> phrase);
    // Pins [offset, offset + phrase.size()) only, so a stored phrase can apply
    // anywhere inside the composition.
    bool apply_phrase_override(size_t offset, std::span<const char32_t> phrase);
    bool clear_phrase_override_choices();
    bool clear_phrase_override_choices(size_t offset, size_t count);
    bool select_candidate(size_t segment_index, size_t candidate_index, bool move_cursor_after_selection);
    bool cancel_candidate_selection(size_t segment_index);
    bool remove_segment(size_t index);

private:
    void touch();

    std::vector<Segment> segments_;
    size_t caret_ = 0;
    size_t revision_ = 0;
    std::optional<size_t> last_edited_segment_;
    std::optional<size_t> selection_anchor_;
};

}  // namespace llavon::ime

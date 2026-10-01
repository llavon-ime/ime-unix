#include "buffer/composition_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace llavon::ime {

namespace {

void append_utf16(std::u16string& text, char32_t value) {
    if (value == 0) return;
    if (value <= 0xFFFF) {
        text.push_back(static_cast<char16_t>(value));
        return;
    }

    const char32_t codepoint = value - 0x10000;
    text.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
    text.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
}

}  // namespace

bool Segment::complete() const noexcept {
    return reading_finalized && syllable.complete();
}

bool Segment::empty() const noexcept {
    return syllable.empty();
}

bool Segment::visible_candidate() const noexcept {
    if (literal != 0) return true;
    return complete() && selected_index < candidates.size();
}

char32_t Segment::selected_candidate() const noexcept {
    if (literal != 0) return literal;
    return visible_candidate() ? candidates[selected_index] : 0;
}

std::u16string Segment::reading() const {
    if (literal != 0) return {};
    return syllable.text();
}

std::u16string Segment::rendered_text() const {
    if (!visible_candidate()) return reading();

    std::u16string result;
    append_utf16(result, selected_candidate());
    return result;
}

bool CompositionBuffer::add_bopomofo(char32_t symbol) {
    selection_anchor_.reset();
    if (caret_ > 0 && !segments_[caret_ - 1].visible_candidate() &&
        !segments_[caret_ - 1].reading_finalized) {
        auto& segment = segments_[caret_ - 1];
        if (segment.syllable.accept(symbol) || segment.syllable.overwrite(symbol)) {
            segment.candidates.clear();
            segment.selected_index = 0;
            segment.manually_chosen = false;
            segment.phrase_override_chosen = false;
            segment.reading_finalized = is_bopomofo_tone(symbol) && segment.syllable.complete();
            segment.alternative_readings.clear();
            last_edited_segment_ = caret_ - 1;
            touch();
            return true;
        }
    }

    Segment next;
    if (!next.syllable.accept(symbol)) return false;
    next.reading_finalized = is_bopomofo_tone(symbol) && next.syllable.complete();

    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
    last_edited_segment_ = caret_;
    ++caret_;
    touch();
    return true;
}

std::optional<BopomofoInputResult> CompositionBuffer::add_bopomofo_key(char32_t key,
                                                                       BopomofoKeyboardLayout layout,
                                                                       bool accept_uppercase) {
    selection_anchor_.reset();
    size_t active_index = segments_.size();
    if (caret_ > 0 && !segments_[caret_ - 1].visible_candidate() &&
        !segments_[caret_ - 1].reading_finalized && !segments_[caret_ - 1].empty()) {
        active_index = caret_ - 1;
    }

    Syllable active = active_index < segments_.size() ? segments_[active_index].syllable : Syllable();
    auto result = apply_bopomofo_key(active, layout, key, accept_uppercase);
    if (result.status == BopomofoKeyStatus::Rejected) return std::nullopt;

    if (active_index < segments_.size()) {
        auto& segment = segments_[active_index];
        segment.syllable = std::move(active);
        segment.candidates.clear();
        segment.selected_index = 0;
        segment.manually_chosen = false;
        segment.phrase_override_chosen = false;
        segment.reading_finalized = result.status == BopomofoKeyStatus::Completed;
        segment.alternative_readings.clear();
        if (result.status == BopomofoKeyStatus::Completed) {
            segment.alternative_readings = std::move(result.alternative_readings);
        }
    } else {
        Segment next;
        next.syllable = std::move(active);
        next.reading_finalized = result.status == BopomofoKeyStatus::Completed;
        if (result.status == BopomofoKeyStatus::Completed) {
            next.alternative_readings = std::move(result.alternative_readings);
        }
        segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
        active_index = caret_;
        ++caret_;
    }
    last_edited_segment_ = active_index;
    touch();
    return BopomofoInputResult{active_index, result.status == BopomofoKeyStatus::Completed, result.natural_extension};
}

std::optional<BopomofoInputResult> CompositionBuffer::add_bopomofo_keys(
    std::u16string_view keys,
    char32_t tone_key,
    BopomofoKeyboardLayout layout,
    bool strict,
    std::u16string_view expected_reading) {
    if (caret_ != segments_.size()) return std::nullopt;
    if (caret_ > 0) {
        const auto& previous = segments_[caret_ - 1];
        if (!previous.visible_candidate() && !previous.reading_finalized) return std::nullopt;
    }

    const size_t original_size = segments_.size();
    const size_t original_caret = caret_;
    if (!expected_reading.empty()) {
        const auto interpretations = replay_bopomofo_keys(keys, tone_key, layout);
        const auto found = std::ranges::find_if(interpretations, [&](const auto& syllable) {
            return syllable.text() == expected_reading;
        });
        if (found == interpretations.end()) return std::nullopt;
        Segment next;
        next.syllable = *found;
        next.reading_finalized = true;
        segments_.push_back(std::move(next));
        last_edited_segment_ = caret_++;
        touch();
        return BopomofoInputResult{original_size, true, true};
    }
    for (const char32_t key : keys) {
        const auto result = add_bopomofo_key(key, layout, true);
        if (!result || (strict && !result->natural_extension)) {
            while (segments_.size() > original_size) segments_.pop_back();
            caret_ = original_caret;
            last_edited_segment_.reset();
            touch();
            return std::nullopt;
        }
    }

    const auto result = add_bopomofo_key(tone_key, layout, true);
    if (!result || !result->completed) {
        while (segments_.size() > original_size) segments_.pop_back();
        caret_ = original_caret;
        last_edited_segment_.reset();
        touch();
        return std::nullopt;
    }
    return result;
}

bool CompositionBuffer::add_literal(char32_t symbol) {
    selection_anchor_.reset();
    if (symbol == 0) return false;

    Segment next;
    next.literal = symbol;
    segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(caret_), std::move(next));
    last_edited_segment_.reset();
    ++caret_;
    touch();
    return true;
}

bool CompositionBuffer::backspace() {
    selection_anchor_.reset();
    if (segments_.empty() || caret_ == 0) return false;

    auto& segment = segments_[caret_ - 1];
    last_edited_segment_ = caret_ - 1;

    if (segment.visible_candidate()) {
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
        --caret_;
        last_edited_segment_.reset();
        touch();
        return true;
    }

    const bool removed = segment.syllable.pop_back();
    if (segment.empty()) {
        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
        --caret_;
        last_edited_segment_.reset();
    } else {
        segment.candidates.clear();
        segment.selected_index = 0;
        segment.manually_chosen = false;
        segment.phrase_override_chosen = false;
        segment.reading_finalized = false;
        segment.alternative_readings.clear();
    }
    if (removed) touch();
    return removed;
}

bool CompositionBuffer::delete_forward() {
    if (caret_ >= segments_.size()) return false;
    return remove_segment(caret_);
}

bool CompositionBuffer::move_cursor_left() {
    selection_anchor_.reset();
    if (caret_ == 0) return false;
    --caret_;
    last_edited_segment_.reset();
    touch();
    return true;
}

bool CompositionBuffer::move_cursor_right() {
    selection_anchor_.reset();
    if (caret_ >= segments_.size()) return false;
    ++caret_;
    last_edited_segment_.reset();
    touch();
    return true;
}

bool CompositionBuffer::extend_selection(int delta) {
    if (segments_.empty() || delta == 0) return false;
    if (caret_ == 0 && delta < 0) return false;
    if (caret_ >= segments_.size() && delta > 0) return false;
    if (!selection_anchor_) selection_anchor_ = std::min(caret_, segments_.size());

    if (delta < 0) {
        --caret_;
    } else {
        ++caret_;
    }
    last_edited_segment_.reset();
    touch();
    return true;
}

bool CompositionBuffer::clear_selection() {
    if (!selection_anchor_) return false;
    caret_ = *selection_anchor_;
    selection_anchor_.reset();
    touch();
    return true;
}

std::optional<std::pair<size_t, size_t>> CompositionBuffer::marked_range() const {
    if (!selection_anchor_) return std::nullopt;

    const size_t begin = std::min(*selection_anchor_, caret_);
    const size_t end = std::max(*selection_anchor_, caret_);
    if (begin == end || end > segments_.size()) return std::nullopt;
    return std::make_pair(begin, end);
}

std::u16string CompositionBuffer::marked_text() const {
    const auto range = marked_range();
    if (!range) return {};

    std::u16string result;
    for (size_t i = range->first; i < range->second; ++i) result += segments_[i].rendered_text();
    return result;
}

std::vector<std::u16string> CompositionBuffer::marked_readings() const {
    const auto range = marked_range();
    if (!range) return {};

    std::vector<std::u16string> readings;
    readings.reserve(range->second - range->first);
    for (size_t i = range->first; i < range->second; ++i) {
        const auto& segment = segments_[i];
        if (!segment.complete() || !segment.visible_candidate()) return {};
        auto reading = segment.reading();
        if (reading.empty()) return {};
        readings.push_back(std::move(reading));
    }
    return readings;
}

void CompositionBuffer::clear() {
    selection_anchor_.reset();
    if (segments_.empty() && caret_ == 0) return;
    segments_.clear();
    caret_ = 0;
    last_edited_segment_.reset();
    touch();
}

bool CompositionBuffer::empty() const noexcept {
    return segments_.empty();
}

bool CompositionBuffer::has_unfinished_reading() const noexcept {
    return std::any_of(segments_.begin(), segments_.end(), [](const Segment& segment) {
        return !segment.reading_finalized && !segment.empty();
    });
}

bool CompositionBuffer::has_unfinished_reading_before_caret() const noexcept {
    if (caret_ == 0 || caret_ > segments_.size()) return false;
    const auto& segment = segments_[caret_ - 1];
    return !segment.reading_finalized && !segment.empty();
}

bool CompositionBuffer::clear_unfinished_reading() {
    const auto unfinished = [](const Segment& segment) {
        return !segment.reading_finalized && !segment.empty();
    };
    if (caret_ > 0 && unfinished(segments_[caret_ - 1])) return remove_segment(caret_ - 1);
    if (caret_ < segments_.size() && unfinished(segments_[caret_])) return remove_segment(caret_);

    const auto it = std::find_if(segments_.begin(), segments_.end(), unfinished);
    if (it == segments_.end()) return false;
    return remove_segment(static_cast<size_t>(std::distance(segments_.begin(), it)));
}

std::u16string CompositionBuffer::raw_composition() const {
    std::u16string result;
    for (const auto& segment : segments_) result += segment.reading();
    return result;
}

std::u16string CompositionBuffer::rendered_composition() const {
    std::u16string result;
    for (const auto& segment : segments_) result += segment.rendered_text();
    return result;
}

std::u16string CompositionBuffer::rendered_prefix_before_caret() const {
    std::u16string result;
    for (size_t i = 0; i < std::min(caret_, segments_.size()); ++i) result += segments_[i].rendered_text();
    return result;
}

std::u16string CompositionBuffer::commit_text() const {
    return rendered_composition();
}

std::u16string CompositionBuffer::candidate_commit_text() const {
    std::u16string result;
    for (const auto& segment : segments_) {
        if (segment.visible_candidate()) result += segment.rendered_text();
    }
    return result;
}

std::optional<size_t> CompositionBuffer::candidate_target(CandidateTarget target) const {
    if (segments_.empty()) return std::nullopt;

    if (target == CandidateTarget::BeforeCursor) {
        if (caret_ == 0) return std::nullopt;
        const size_t index = caret_ - 1;
        if (segments_[index].complete()) return index;
        return std::nullopt;
    }

    if (caret_ >= segments_.size()) return std::nullopt;
    if (segments_[caret_].complete()) return caret_;
    return std::nullopt;
}

std::optional<size_t> CompositionBuffer::last_edited_segment() const noexcept {
    return last_edited_segment_;
}

size_t CompositionBuffer::caret() const noexcept {
    return caret_;
}

bool CompositionBuffer::caret_at_end() const noexcept {
    return caret_ == segments_.size();
}

size_t CompositionBuffer::revision() const noexcept {
    return revision_;
}

const std::vector<Segment>& CompositionBuffer::segments() const noexcept {
    return segments_;
}

std::vector<size_t> CompositionBuffer::completed_segment_indices() const {
    std::vector<size_t> indices;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].complete()) indices.push_back(i);
    }
    return indices;
}

bool CompositionBuffer::segment_complete(size_t index) const {
    return index < segments_.size() && segments_[index].complete();
}

std::u16string CompositionBuffer::segment_reading(size_t index) const {
    if (index >= segments_.size()) return {};
    return segments_[index].reading();
}

const std::vector<char32_t>* CompositionBuffer::segment_candidates(size_t index) const {
    if (index >= segments_.size()) return nullptr;
    return &segments_[index].candidates;
}

std::optional<size_t> CompositionBuffer::segment_selected_index(size_t index) const {
    if (index >= segments_.size()) return std::nullopt;
    if (!segments_[index].visible_candidate()) return std::nullopt;
    return segments_[index].selected_index;
}

std::optional<size_t> CompositionBuffer::manually_chosen_segment_at_caret() const noexcept {
    const auto explicitly_chosen = [this](size_t index) {
        return segments_[index].manually_chosen && !segments_[index].phrase_override_chosen;
    };
    if (caret_ > 0 && explicitly_chosen(caret_ - 1)) return caret_ - 1;
    if (caret_ < segments_.size() && explicitly_chosen(caret_)) return caret_;
    return std::nullopt;
}

bool CompositionBuffer::set_segment_candidates(size_t index, std::vector<char32_t> candidates,
                                               bool preserve_manual_choice) {
    if (index >= segments_.size()) return false;

    auto& segment = segments_[index];
    if (preserve_manual_choice && segment.manually_chosen) return false;

    segment.candidates = std::move(candidates);
    segment.selected_index = 0;
    segment.manually_chosen = false;
    segment.phrase_override_chosen = false;
    touch();
    return true;
}

bool CompositionBuffer::refresh_segment_candidates(size_t index, std::vector<char32_t> candidates) {
    if (index >= segments_.size()) return false;

    auto& segment = segments_[index];
    if (!segment.phrase_override_chosen) {
        return set_segment_candidates(index, std::move(candidates), true);
    }

    const char32_t pinned = segment.selected_candidate();
    segment.candidates = std::move(candidates);
    if (pinned != 0) {
        segment.candidates.erase(
            std::remove(segment.candidates.begin(), segment.candidates.end(), pinned),
            segment.candidates.end());
        segment.candidates.insert(segment.candidates.begin(), pinned);
    }
    segment.selected_index = 0;
    touch();
    return true;
}

bool CompositionBuffer::apply_phrase_override(std::span<const char32_t> phrase) {
    if (phrase.size() != segments_.size()) return false;
    return apply_phrase_override(0, phrase);
}

bool CompositionBuffer::apply_phrase_override(size_t offset, std::span<const char32_t> phrase) {
    if (phrase.empty() || offset > segments_.size() || phrase.size() > segments_.size() - offset) return false;
    for (size_t i = offset; i < offset + phrase.size(); ++i) {
        const auto& segment = segments_[i];
        if (!segment.complete() || segment.literal != 0 ||
            (segment.manually_chosen && !segment.phrase_override_chosen)) {
            return false;
        }
    }

    bool changed = false;
    for (size_t i = 0; i < phrase.size(); ++i) {
        auto& segment = segments_[offset + i];
        if (segment.candidates.empty() || segment.candidates.front() != phrase[i]) {
            segment.candidates.erase(
                std::remove(segment.candidates.begin(), segment.candidates.end(), phrase[i]),
                segment.candidates.end());
            segment.candidates.insert(segment.candidates.begin(), phrase[i]);
            changed = true;
        }
        if (segment.selected_index != 0 || !segment.manually_chosen || !segment.phrase_override_chosen) changed = true;
        segment.selected_index = 0;
        segment.manually_chosen = true;
        segment.phrase_override_chosen = true;
    }
    if (changed) touch();
    return true;
}

bool CompositionBuffer::clear_phrase_override_choices() {
    return clear_phrase_override_choices(0, segments_.size());
}

bool CompositionBuffer::clear_phrase_override_choices(size_t offset, size_t count) {
    bool changed = false;
    const size_t end = std::min(offset + count, segments_.size());
    for (size_t i = offset; i < end; ++i) {
        auto& segment = segments_[i];
        if (!segment.phrase_override_chosen) continue;
        segment.manually_chosen = false;
        segment.phrase_override_chosen = false;
        changed = true;
    }
    if (changed) touch();
    return changed;
}

bool CompositionBuffer::select_candidate(size_t segment_index, size_t candidate_index,
                                         bool move_cursor_after_selection) {
    if (segment_index >= segments_.size()) return false;

    auto& segment = segments_[segment_index];
    if (candidate_index >= segment.candidates.size()) return false;
    selection_anchor_.reset();

    segment.selected_index = candidate_index;
    segment.manually_chosen = true;
    segment.phrase_override_chosen = false;
    if (move_cursor_after_selection) caret_ = segment_index + 1;
    last_edited_segment_ = segment_index;
    touch();
    return true;
}

bool CompositionBuffer::cancel_candidate_selection(size_t segment_index) {
    if (segment_index >= segments_.size()) return false;
    selection_anchor_.reset();

    auto& segment = segments_[segment_index];
    if (!segment.manually_chosen && segment.selected_index == 0) return false;

    segment.selected_index = 0;
    segment.manually_chosen = false;
    segment.phrase_override_chosen = false;
    last_edited_segment_ = segment_index;
    touch();
    return true;
}

bool CompositionBuffer::remove_segment(size_t index) {
    selection_anchor_.reset();
    if (index >= segments_.size()) return false;

    segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(index));
    if (caret_ > index) --caret_;
    if (caret_ > segments_.size()) caret_ = segments_.size();
    last_edited_segment_.reset();
    touch();
    return true;
}

void CompositionBuffer::touch() {
    ++revision_;
}

}  // namespace llavon::ime

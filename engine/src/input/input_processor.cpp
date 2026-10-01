#include "input/input_processor.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "bopomofo/keymap.hpp"
#include "input/ascii_tokenizer.hpp"
#include "input/keypad.hpp"
#include "input/punctuation.hpp"
#include "text/utf.hpp"

namespace llavon::ime {

std::optional<int> selection_index_for_key(char32_t symbol, std::string_view selection_keys,
                                           int selection_key_count, bool caps_lock_inputs_bopomofo) {
    const auto normalized_key = caps_lock_inputs_bopomofo ? ascii_lower(symbol) : symbol;
    const int count = std::min(selection_key_count, static_cast<int>(selection_keys.size()));
    for (int i = 0; i < count; ++i) {
        const auto selection_key =
            static_cast<char32_t>(static_cast<unsigned char>(selection_keys[static_cast<size_t>(i)]));
        if (normalized_key == selection_key) return i;
    }
    return std::nullopt;
}

CandidateKeyOutcome handle_candidate_key(const InputKey& key, const CandidateKeyConfig& config,
                                         CandidateView& view, std::size_t candidate_count,
                                         bool mixed_decision_active, bool has_chewing_punctuation) {
    if (candidate_count == 0) return {};

    switch (key.sym) {
        case keysym::Up:
            return {view.move_cursor(-1, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                            : CandidateKeyAction::Handled,
                    0};
        case keysym::Down:
            return {view.move_cursor(1, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                           : CandidateKeyAction::Handled,
                    0};
        case keysym::Left:
            if (!view.page_by(-1, true, config.page_size, candidate_count)) {
                return {CandidateKeyAction::PassThrough, 0};
            }
            return {CandidateKeyAction::Redraw, 0};
        case keysym::Right:
            if (!view.page_by(1, true, config.page_size, candidate_count)) {
                return {CandidateKeyAction::PassThrough, 0};
            }
            return {CandidateKeyAction::Redraw, 0};
        case keysym::Home:
            return {view.set_cursor(0, config.page_size, candidate_count) ? CandidateKeyAction::Redraw
                                                                          : CandidateKeyAction::Handled,
                    0};
        case keysym::End:
            return {view.set_cursor(static_cast<int>(candidate_count) - 1, config.page_size, candidate_count)
                        ? CandidateKeyAction::Redraw
                        : CandidateKeyAction::Handled,
                    0};
        default:
            break;
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym))) {
        return {CandidateKeyAction::ActivateCursor, 0};
    }

    if (key.sym == U' ' && config.space_selects_candidate) {
        return {CandidateKeyAction::SelectIndex, view.cursor};
    }

    const int digit_index = ascii_digit_selection_index(static_cast<std::uint32_t>(key.sym));
    if (digit_index >= 0 && !key.has_blocking_modifier()) {
        const int page_offset = view.page_offset(config.page_size, candidate_count);
        return {CandidateKeyAction::SelectIndex, page_offset + digit_index};
    }

    if (const auto index = selection_index_for_key(key.sym, config.selection_keys, config.selection_key_count,
                                                   config.caps_lock_inputs_bopomofo)) {
        const int page_offset = view.page_offset(config.page_size, candidate_count);
        return {CandidateKeyAction::SelectIndex, page_offset + *index};
    }

    if (has_chewing_punctuation && !mixed_decision_active) {
        return {CandidateKeyAction::Handled, 0};
    }

    return {};
}

SymbolMenuKeyOutcome handle_symbol_menu_key(const InputKey& key, const CandidateKeyConfig& config,
                                            SymbolMenuState& menu, CandidateView& view,
                                            std::size_t candidate_count) {
    if (key.sym == keysym::Escape || key.sym == keysym::grave) {
        return {SymbolMenuKeyAction::CloseMenu, 0};
    }

    if (key.sym == keysym::BackSpace) {
        if (menu.in_category()) {
            menu.back();
            view.reset();
            return {SymbolMenuKeyAction::Redraw, 0};
        }
        return {SymbolMenuKeyAction::CloseMenu, 0};
    }

    switch (key.sym) {
        case keysym::Up:
            return {view.move_cursor(-1, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                            : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Down:
            return {view.move_cursor(1, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                           : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Left:
            if (view.page_by(-1, true, config.page_size, candidate_count)) {
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Right:
            if (view.page_by(1, true, config.page_size, candidate_count)) {
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Home:
            return {view.set_cursor(0, config.page_size, candidate_count) ? SymbolMenuKeyAction::Redraw
                                                                          : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::End:
            return {view.set_cursor(static_cast<int>(candidate_count) - 1, config.page_size, candidate_count)
                        ? SymbolMenuKeyAction::Redraw
                        : SymbolMenuKeyAction::Handled,
                    0};
        case keysym::Page_Up:
        case keysym::Page_Down:
            if (view.page_by(key.sym == keysym::Page_Up ? -1 : 1, false, config.page_size, candidate_count)) {
                (void)view.set_cursor(view.page_offset(config.page_size, candidate_count), config.page_size,
                                      candidate_count);
                return {SymbolMenuKeyAction::Redraw, 0};
            }
            return {SymbolMenuKeyAction::Handled, 0};
        case keysym::Tab:
            view.expanded = !view.expanded;
            return {SymbolMenuKeyAction::Redraw, 0};
        default:
            break;
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym)) ||
        (key.sym == U' ' && config.space_selects_candidate)) {
        return {SymbolMenuKeyAction::ActivateCursor, 0};
    }

    if (const auto index = selection_index_for_key(key.sym, config.selection_keys, config.selection_key_count,
                                                   config.caps_lock_inputs_bopomofo)) {
        return {SymbolMenuKeyAction::SelectIndex,
                view.page_offset(config.page_size, candidate_count) + *index};
    }

    return {};
}

InputProcessor::InputProcessor(FallbackEngine& fallback, MixedInputDecoder& decoder,
                               PhraseOverrideStore& phrase_overrides)
    : fallback_(fallback), decoder_(decoder), phrase_overrides_(phrase_overrides) {}

void InputProcessor::set_state_observer(StateObserver observer) {
    state_observer_ = std::move(observer);
}

void InputProcessor::consume() {
    effect_.handled = true;
}

void InputProcessor::redraw() {
    effect_.redraw = true;
}

void InputProcessor::request_prediction() {
    effect_.request_prediction = true;
}

void InputProcessor::commit_text(std::u16string text) {
    effect_.commit = std::move(text);
}

namespace {
// Pending smart input is inserted at the buffer caret, just like settled
// literals and phonetic segments. Display and all commit routes share this
// assembly so a suffix never moves in front of newly typed text.
std::u16string composition_with_pending(const CompositionBuffer& buffer, std::u16string_view pending,
                                       bool candidates_only = false) {
    std::u16string text;
    const auto& segments = buffer.segments();
    for (size_t i = 0; i <= segments.size(); ++i) {
        if (i == buffer.caret()) text += pending;
        if (i < segments.size() && (!candidates_only || segments[i].visible_candidate())) {
            text += segments[i].rendered_text();
        }
    }
    return text;
}

// Everything the composition does not account for was typed literally:
// SmartEnglish pending words and the trailing space or punctuation. Like
// Windows, they stay in the sample as context without a reading.
bool append_literal_tail(InputEffect::CommitSample& sample, std::u16string_view committed) {
    if (!committed.starts_with(sample.answer)) return false;
    const auto tail = utf8_to_u32(u16_to_utf8(committed.substr(sample.answer.size())));
    for (const auto character : tail) {
        if (character == 0 || sample.entries.size() >= 1024) return false;
        sample.entries.push_back({std::u16string{}, character, false, true});
    }
    sample.answer += committed.substr(sample.answer.size());
    return sample.answer == committed && sample.answer.size() <= 1024;
}

std::optional<InputEffect::CommitSample> training_sample(const CompositionBuffer& buffer,
                                                          std::u16string_view committed) {
    if (buffer.segments().empty() || buffer.segments().size() > 1024) return std::nullopt;
    InputEffect::CommitSample sample;
    bool trainable = false;
    for (const auto& segment : buffer.segments()) {
        // A visible candidate is complete by construction; literal positions
        // are rendered as typed and carry no reading.
        if (!segment.visible_candidate() || segment.selected_candidate() == 0) return std::nullopt;
        if (segment.literal != 0) {
            // Windows keeps literal characters in the sample so later
            // Bopomofo positions keep their context; they are not targets.
            sample.entries.push_back({std::u16string{}, segment.literal, false, true});
        } else {
            if (segment.reading().empty()) return std::nullopt;
            sample.entries.push_back({segment.reading(), segment.selected_candidate(),
                                      segment.manually_chosen, false});
            trainable = true;
        }
        sample.answer += segment.rendered_text();
    }
    // A commit without a single composed position carries nothing to learn.
    if (!trainable) return std::nullopt;
    if (!append_literal_tail(sample, committed)) return std::nullopt;
    return sample;
}

// The SmartEnglish decision already holds one entry per committed position:
// Bopomofo segments keep their reading and candidate, everything else is a
// literal position. The committed text must match exactly, so a manually
// paged candidate simply skips recording instead of storing a wrong target.
std::optional<InputEffect::CommitSample> mixed_training_sample(const InputSession& session,
                                                               std::u16string_view committed) {
    if (!session.mixed_decision.active()) return std::nullopt;
    // The preview path can change while typing; the committed text tells which
    // interpretation was really inserted, so the longest matching rendering
    // wins. Matching exactly keeps a paged candidate from storing a wrong
    // target.
    const MixedPath* chosen = nullptr;
    for (const auto& path : session.mixed_decision.result.paths) {
        if (path.rendered.empty() || !committed.starts_with(path.rendered)) continue;
        if (chosen == nullptr || path.rendered.size() > chosen->rendered.size()) chosen = &path;
    }
    if (chosen == nullptr) return std::nullopt;
    const auto& segments = chosen->segments;
    if (segments.empty() || segments.size() > 1024) return std::nullopt;
    InputEffect::CommitSample sample;
    bool trainable = false;
    for (const auto& segment : segments) {
        if (segment.consumed_boundary) continue;
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            if (segment.reading.empty() || segment.candidates.empty() || segment.candidates.front() == 0)
                return std::nullopt;
            sample.entries.push_back({segment.reading, segment.candidates.front(), false, false});
            sample.answer += utf8_to_u16(char32_to_utf8(segment.candidates.front()));
            trainable = true;
            continue;
        }
        for (const auto character : utf8_to_u32(u16_to_utf8(segment.raw))) {
            if (character == 0 || sample.entries.size() >= 1024) return std::nullopt;
            sample.entries.push_back({std::u16string{}, character, false, true});
            sample.answer += utf8_to_u16(char32_to_utf8(character));
        }
    }
    if (!trainable) return std::nullopt;
    if (!append_literal_tail(sample, committed)) return std::nullopt;
    return sample;
}

std::optional<InputEffect::CommitSample> commit_training_sample(const InputSession& session,
                                                               std::u16string_view committed) {
    if (!session.pending_token.empty() && !session.buffer.caret_at_end()) {
        // The old append-only sample assembly would label an inserted reading
        // as literal text, or associate it with a matching suffix character.
        // Materialize a copy at the same caret before capturing the sample.
        auto buffer = session.buffer;
        const auto& decision = session.mixed_decision;
        if (decision.active() && decision.source_revision == session.pending_token.revision &&
            decision.result.raw == session.pending_token.raw && decision.preview_path > 0 &&
            decision.preview_path < decision.result.paths.size()) {
            for (const auto& segment : decision.result.paths[decision.preview_path].segments) {
                if (segment.consumed_boundary) continue;
                if (segment.kind == MixedSegmentKind::Bopomofo) {
                    const auto inserted = buffer.add_bopomofo_keys(segment.body_keys, segment.tone_key,
                        session.pending_token.layout, true, segment.reading);
                    if (!inserted || !inserted->completed || segment.candidates.empty() ||
                        !buffer.set_segment_candidates(inserted->segment_index, segment.candidates)) return std::nullopt;
                } else {
                    for (const char16_t ch : segment.raw) (void)buffer.add_literal(ch);
                }
            }
        } else {
            for (const char16_t ch : session.pending_token.raw) (void)buffer.add_literal(ch);
        }
        return training_sample(buffer, committed);
    }
    auto sample = training_sample(session.buffer, committed);
    if (!sample) sample = mixed_training_sample(session, committed);
    return sample;
}
}

void InputProcessor::mark_prediction_dirty() {
    session_->prediction.mark_dirty();
}

InputEffect InputProcessor::process(const InputKey& key, InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    process_impl(normalize_key(key));
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::select_candidate(InputSession& session, const Config& config, int index) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    if (select_candidate_impl(index)) consume();
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::select_symbol(InputSession& session, const Config& config, int index,
                                           std::uint64_t epoch) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};
    if (select_symbol_impl(index, epoch)) consume();
    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

InputEffect InputProcessor::reset(InputSession& session, const Config& config, InputResetReason reason,
                                  bool clear_context) {
    session_ = &session;
    config_ = &config;
    effect_ = InputEffect{};

    const bool complete_composition = !session.buffer.empty() && !session.buffer.has_unfinished_reading();
    const bool should_commit = reason == InputResetReason::Deactivate ||
                               (reason == InputResetReason::FocusOut &&
                                (!session.pending_token.empty() || complete_composition));
    if (should_commit) {
        auto text = composition_with_pending(session.buffer, pending_rendered_text(session), true);
        effect_.training_sample = commit_training_sample(session, text);
        commit_text(std::move(text));
    }

    session.buffer.clear();
    session.pending_token.clear();
    session.mixed_decision.clear();
    session.symbol_menu.close();
    if (clear_context) session.context_text.clear();
    (void)transition_to(InputStateKind::Empty);
    session.prediction.invalidate();
    redraw();

    session_ = nullptr;
    config_ = nullptr;
    return effect_;
}

void InputProcessor::prepare_for_config_change(InputSession& session) {
    for (const char16_t ch : session.pending_token.raw) {
        (void)session.buffer.add_literal(static_cast<char32_t>(ch));
    }
    session.pending_token.clear();
    session.mixed_decision.clear();
    session.prediction.invalidate();
}

void InputProcessor::process_impl(const InputKey& key) {
    const auto layout = bopomofo_keyboard_layout(config_->keyboard_layout);
    const auto raw_symbol = key.sym;

    // CapsLock state only exists in the raw key. When Chinese input is
    // disabled under CapsLock, everything passes through to the application
    // (mirrors McBopomofo's capsLockAllowChineseInput=False behavior) and the
    // composition is reset.
    if (key.caps_lock) {
        if (!config_->caps_lock_inputs_bopomofo) {
            if (!session_->pending_token.empty()) {
                commit_composition_with(0);
            } else {
                session_->buffer.clear();
                session_->mixed_decision.clear();
                session_->symbol_menu.close();
                (void)transition_to(InputStateKind::Empty);
                session_->prediction.clear_pending();
                redraw();
            }
            return;
        }
    }

    // Shift+Left/Right (McBopomofo also accepts Ctrl+Shift) marks a range
    // inside the composition; Enter then stores the marked text as a phrase
    // override. Plain arrows keep moving the caret and clear the mark.
    const bool arrow_key = key.sym == keysym::Left || key.sym == keysym::Right;
    if (arrow_key && key.has(InputKeyState::Shift) &&
        !(key.has(InputKeyState::Alt) || key.has(InputKeyState::Super) || key.has(InputKeyState::Meta)) &&
        !session_->symbol_menu.active() && !candidate_list_active(*session_) && !composition_empty(*session_)) {
        if (config_->smart_english && !session_->pending_token.empty()) (void)settle_pending_preview();
        if (session_->buffer.extend_selection(key.sym == keysym::Left ? -1 : 1)) {
            redraw();
            consume();
            return;
        }
    }

    // Shift+space commits the composition followed by a space; with an empty
    // buffer the key passes through (mirrors McBopomofo's Shift+space).
    if (key.sym == U' ' && key.raw_has(InputKeyState::Shift)) {
        if (composition_empty(*session_)) {
            return;
        }
        commit_composition_with(U' ');
        consume();
        return;
    }

    const auto chewing_punctuation = chewing_punctuation_for_key(key, layout);
    const bool punctuation_is_bopomofo = !key.has_blocking_modifier() &&
        lookup_bopomofo_key(raw_symbol, layout, false).has_value();
    const auto punctuation = punctuation_is_bopomofo ? std::nullopt : chewing_punctuation;
    if (!punctuation && key.has_shortcut_modifier()) return;

    if (session_->symbol_menu.active()) {
        const auto outcome = handle_symbol_menu_key(key, candidate_key_config(), session_->symbol_menu,
                                                    session_->candidate_view, session_->symbol_menu.menu().size());
        switch (outcome.action) {
            case SymbolMenuKeyAction::CloseMenu:
                close_symbol_menu();
                break;
            case SymbolMenuKeyAction::ActivateCursor:
                (void)select_symbol_impl(session_->candidate_view.cursor, session_->symbol_menu.epoch());
                break;
            case SymbolMenuKeyAction::SelectIndex:
                (void)select_symbol_impl(outcome.index, session_->symbol_menu.epoch());
                break;
            case SymbolMenuKeyAction::Redraw:
                redraw();
                break;
            case SymbolMenuKeyAction::Handled:
                break;
        }
        consume();
        return;
    }

    if (key.sym == keysym::grave && !key.has_blocking_modifier()) {
        if (config_->smart_english && !session_->pending_token.empty()) {
            (void)settle_pending_preview();
            open_symbol_menu();
        } else if (!session_->buffer.has_unfinished_reading()) {
            open_symbol_menu();
        }
        consume();
        return;
    }

    // The numeric keypad keeps its literal meaning, but a composition in
    // progress absorbs the character it types (a digit, or `.`/`+`/`-`/`*`/
    // `/`/`=`) as a literal instead of being committed: the preedit stays
    // editable and the character commits with it. With nothing to compose the
    // key falls through and is typed by the application.
    auto keypad_literal = keypad_digit_keysym(static_cast<std::uint32_t>(key.sym));
    if (!keypad_literal) keypad_literal = keypad_operator_keysym(static_cast<std::uint32_t>(key.sym));
    if (keypad_literal) {
        if (!session_->pending_token.empty()) (void)settle_pending_preview();
        if (!session_->buffer.empty()) {
            (void)session_->buffer.add_literal(*keypad_literal);
            (void)transition_to(InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        }
    }

    if (session_->choosing_candidate() && candidate_count() != 0) {
        const auto outcome =
            handle_candidate_key(key, candidate_key_config(), session_->candidate_view,
                                 candidate_count(), session_->mixed_decision.active(),
                                 chewing_punctuation.has_value());
        switch (outcome.action) {
            case CandidateKeyAction::PassThrough:
                return;
            case CandidateKeyAction::Handled:
                consume();
                return;
            case CandidateKeyAction::Redraw:
                redraw();
                consume();
                return;
            case CandidateKeyAction::ActivateCursor:
                if (session_->mixed_decision.active()) {
                    (void)commit_mixed_candidate_impl(session_->candidate_view.cursor);
                } else {
                    (void)select_candidate_impl(session_->candidate_view.cursor);
                }
                consume();
                return;
            case CandidateKeyAction::SelectIndex:
                (void)select_candidate_impl(outcome.index);
                consume();
                return;
            case CandidateKeyAction::Unhandled:
                break;
        }
    }

    // Smart English keeps the raw input in a pending token; the mixed-input
    // decoder ranks parallel interpretations after every input event. Raw
    // keys (including Space) remain editable until an explicit confirmation.
    if (config_->smart_english) {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        const bool caps_on = key.caps_lock;
        const bool english_intent = (is_upper || is_lower) && (is_upper != caps_on);
        if (english_intent) {
            if (handle_english_letter(raw_symbol, caps_on)) consume();
            return;
        }

        const auto smart_layout = session_->pending_token.empty() ? layout : session_->pending_token.layout;
        if (!session_->pending_token.empty()) {
            if (key.sym == keysym::Escape) {
                (void)session_->buffer.clear_selection();
                if (session_->mixed_decision.active() && session_->choosing_candidate()) {
                    (void)transition_to(InputStateKind::Inputting);
                } else if (session_->mixed_decision.active() && session_->mixed_decision.preview_path != 0) {
                    session_->mixed_decision.preview_path = 0;
                    session_->mixed_decision.preview_character = 0;
                } else {
                    session_->pending_token.clear();
                    session_->mixed_decision.clear();
                }
                (void)transition_to(composition_empty(*session_) ? InputStateKind::Empty : InputStateKind::Inputting);
                redraw();
                consume();
                return;
            }

            if (key.sym == keysym::BackSpace) {
                if (session_->mixed_decision.active() && session_->choosing_candidate()) {
                    (void)transition_to(InputStateKind::Inputting);
                }
                backspace_pending(key.raw_has(InputKeyState::Shift));
                consume();
                return;
            }

            if (is_return_keysym(static_cast<std::uint32_t>(key.sym))) {
                commit_current();
                consume();
                return;
            }

            if (key.sym == keysym::Down && session_->mixed_decision.active()) {
                (void)show_mixed_candidates();
                consume();
                return;
            }

            if (key.sym == U' ') {
                append_pending_char(U' ', smart_layout);
                rerun_pending_decision();
                consume();
                return;
            }

            if (punctuation && key.has(InputKeyState::Ctrl)) {
                (void)settle_pending_preview();
                (void)session_->buffer.add_literal(*punctuation);
                (void)transition_to(InputStateKind::Inputting);
                redraw();
                consume();
                return;
            }

            if (raw_symbol >= 0x21 && raw_symbol <= 0x7e && !key.has_blocking_modifier()) {
                append_pending_char(raw_symbol, smart_layout);
                rerun_pending_decision();
                consume();
                return;
            }

            // Navigation/editing operates on settled ASCII literals inside the
            // composition rather than committing into the client first.
            (void)settle_pending_preview();
        }

        const auto& segments = session_->buffer.segments();
        const bool all_literals = !segments.empty() &&
                                  std::all_of(segments.begin(), segments.end(),
                                              [](const Segment& segment) { return segment.literal != 0; });
        if (session_->pending_token.empty() && all_literals && key.sym == U' ' && !key.has_blocking_modifier()) {
            (void)session_->buffer.add_literal(U' ');
            (void)transition_to(InputStateKind::Inputting);
            redraw();
            consume();
            return;
        }
        if (session_->pending_token.empty() && all_literals && !session_->buffer.caret_at_end() &&
            raw_symbol >= 0x20 && raw_symbol <= 0x7e && !key.has_blocking_modifier()) {
            (void)session_->buffer.add_literal(raw_symbol);
            (void)transition_to(InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        }

        // A tone cannot complete a nonexistent syllable. Keep its exact key
        // reversible instead of leaving an orphan phonetic mark (for example
        // an IBM '.' after explicitly typed uppercase filename letters).
        if (session_->pending_token.empty() && !session_->buffer.has_unfinished_reading() &&
            raw_symbol >= 0x21 && raw_symbol <= 0x7e && !key.has_blocking_modifier() &&
            is_bopomofo_tone_key(raw_symbol, layout)) {
            append_pending_char(raw_symbol, layout);
            rerun_pending_decision();
            consume();
            return;
        }

        if (session_->pending_token.empty() && !key.has_blocking_modifier() && is_smart_start_char(key.sym, layout)) {
            if (candidate_list_active(*session_)) (void)transition_to(InputStateKind::Inputting);
            append_pending_char(key.sym, layout);
            rerun_pending_decision();
            consume();
            return;
        }
    }

    if (punctuation) {
        if (!session_->buffer.has_unfinished_reading()) {
            (void)session_->buffer.add_literal(*punctuation);
            (void)transition_to(InputStateKind::Inputting);
            redraw();
        }
        consume();
        return;
    }

    // Letter keys: the keysym case XOR the CapsLock state decides between
    // English output and bopomofo input, mirroring McBopomofo's case swap.
    // (CapsLock+letter with Chinese disabled already returned above.)
    {
        const bool is_upper = raw_symbol >= U'A' && raw_symbol <= U'Z';
        const bool is_lower = raw_symbol >= U'a' && raw_symbol <= U'z';
        if (is_upper || is_lower) {
            const bool caps_on = key.caps_lock;
            if (is_upper != caps_on) {
                if (handle_english_letter(raw_symbol, caps_on)) consume();
                return;
            }
        }
    }

    // Match Chewing: digits commit directly from an empty state, join completed
    // composition as literals, and bell while a syllable is unfinished, when
    // this layout does not assign the digit to a phonetic symbol or tone.
    if (is_ascii_digit_keysym(static_cast<std::uint32_t>(key.sym)) &&
        !lookup_bopomofo_key(key.sym, layout, false)) {
        if (session_->buffer.has_unfinished_reading()) {
            consume();
            return;
        }
        if (session_->buffer.empty()) {
            commit_text(std::u16string(1, static_cast<char16_t>(key.sym)));
        } else {
            (void)session_->buffer.add_literal(static_cast<char32_t>(key.sym));
            (void)transition_to(InputStateKind::Inputting);
            redraw();
        }
        consume();
        return;
    }

    if (key.sym == U' ' && !session_->buffer.empty() && !session_->buffer.has_unfinished_reading_before_caret()) {
        const bool target_complete = current_candidate_target(*session_, *config_).has_value();
        const bool has_candidates = !available_candidates(*session_, *config_).empty();
        if (target_complete || has_candidates) {
            if (!session_->choosing_candidate()) {
                (void)transition_to(InputStateKind::ChoosingCandidate);
                redraw();
            } else if (config_->space_selects_candidate && candidate_count() != 0) {
                (void)select_candidate_impl(session_->candidate_view.cursor);
            }
            consume();
            return;
        }
    }

    if (is_return_keysym(static_cast<std::uint32_t>(key.sym)) && !composition_empty(*session_)) {
        if (session_->buffer.marked_range()) {
            // Enter in the marking state stores the phrase and keeps composing.
            if (save_marked_phrase_override()) {
                (void)session_->buffer.clear_selection();
                // Pin the freshly stored phrase right away so an immediate
                // commit does not fall back to model output.
                apply_phrase_override(*session_);
                redraw();
            }
            consume();
            return;
        }
        commit_current();
        consume();
        return;
    }

    if (key.sym == keysym::Escape && !composition_empty(*session_)) {
        handle_escape();
        return;
    }

    if (key.sym == keysym::BackSpace && !session_->buffer.empty()) {
        session_->buffer.backspace();
        (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
        mark_prediction_dirty();
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Delete && !session_->buffer.empty()) {
        session_->buffer.delete_forward();
        (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
        mark_prediction_dirty();
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Left && !session_->buffer.empty()) {
        if (!session_->buffer.move_cursor_left()) return;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Right && !session_->buffer.empty()) {
        if (!session_->buffer.move_cursor_right()) return;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        consume();
        return;
    }

    if (key.sym == keysym::Down && !session_->buffer.empty()) {
        const bool target_complete = current_candidate_target(*session_, *config_).has_value();
        const bool has_candidates = !available_candidates(*session_, *config_).empty();
        if (target_complete || has_candidates) {
            (void)transition_to(InputStateKind::ChoosingCandidate);
            redraw();
            consume();
            return;
        }
    }

    if (key.sym == keysym::Tab && !session_->buffer.empty() &&
        !available_candidates(*session_, *config_).empty()) {
        if (!session_->choosing_candidate()) {
            (void)transition_to(InputStateKind::ChoosingCandidate);
        } else {
            session_->candidate_view.expanded = !session_->candidate_view.expanded;
        }
        redraw();
        consume();
        return;
    }

    if ((key.sym == keysym::Page_Up || key.sym == keysym::Page_Down) && !session_->buffer.empty() &&
        candidate_count() != 0) {
        if (page_candidates(key.sym == keysym::Page_Up ? -1 : 1)) {
            (void)set_candidate_cursor(candidate_page_offset(*session_, *config_));
            redraw();
        }
        consume();
        return;
    }

    if (key.sym == U' ' && session_->buffer.empty()) return;

    if (const auto input = session_->buffer.add_bopomofo_key(static_cast<char32_t>(key.sym), layout,
                                                             config_->caps_lock_inputs_bopomofo)) {
        (void)transition_to(InputStateKind::Inputting);
        mark_prediction_dirty();
        if (input->completed) {
            if (const auto segment = session_->buffer.last_edited_segment();
                segment && session_->buffer.segment_complete(*segment)) {
                apply_fallback_candidates(*session_, *segment);
                const auto* candidates = session_->buffer.segment_candidates(*segment);
                if (candidates == nullptr || candidates->empty()) {
                    (void)session_->buffer.remove_segment(*segment);
                    redraw();
                    consume();
                    return;
                }
            }
            request_prediction();
        }
        redraw();
        consume();
        return;
    }
}

bool InputProcessor::transition_to(InputStateKind state) {
    const auto previous = input_state_kind(session_->state);
    if (!transition_input_state(session_->state, state)) return false;
    if (state_observer_) state_observer_(previous, state);
    if (state == InputStateKind::ChoosingCandidate) {
        if (previous != InputStateKind::ChoosingCandidate) {
            reset_candidate_view();
            if (const auto target = current_candidate_target(*session_, *config_)) {
                if (const auto selected = session_->buffer.segment_selected_index(*target)) {
                    session_->candidate_view.cursor = static_cast<int>(*selected);
                }
            }
        }
    } else {
        session_->displayed_candidates.clear();
        reset_candidate_view();
    }
    return true;
}

void InputProcessor::reset_candidate_view() {
    session_->candidate_view.reset();
}

std::size_t InputProcessor::candidate_count() const {
    if (session_->symbol_menu.active()) return session_->symbol_menu.menu().size();
    if (!session_->choosing_candidate()) return 0;
    if (session_->mixed_decision.active()) {
        return decoder_
            .expand_candidates(session_->mixed_decision.result, static_cast<size_t>(config_->candidate_page_size),
                               session_->mixed_decision.preview_path)
            .size();
    }
    return available_candidates(*session_, *config_).size();
}

bool InputProcessor::page_candidates(int delta, bool preserve_cursor_offset) {
    return session_->candidate_view.page_by(delta, preserve_cursor_offset, config_->candidate_page_size,
                                             candidate_count());
}

bool InputProcessor::set_candidate_cursor(int index) {
    return session_->candidate_view.set_cursor(index, config_->candidate_page_size,
                                                candidate_count());
}

CandidateKeyConfig InputProcessor::candidate_key_config() const {
    return CandidateKeyConfig{config_->candidate_page_size,  config_->space_selects_candidate,
                              config_->selection_key_count,  config_->caps_lock_inputs_bopomofo,
                              config_->selection_keys};
}

void InputProcessor::commit_current() {
    auto text = current_preedit(*session_);
    effect_.training_sample = commit_training_sample(*session_, text);
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
}

void InputProcessor::commit_composition_with(char32_t extra) {
    auto text = current_preedit(*session_);
    if (extra != 0) text += utf8_to_u16(char32_to_utf8(extra));
    effect_.training_sample = commit_training_sample(*session_, text);
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
}

bool InputProcessor::select_candidate_impl(int index) {
    if (!session_->choosing_candidate()) return false;
    if (session_->mixed_decision.active()) return select_mixed_candidate_impl(index);
    const auto target = current_candidate_target(*session_, *config_);
    if (!target || index < 0) return false;

    const auto candidates = available_candidates(*session_, *config_);
    if (index >= static_cast<int>(candidates.size())) return false;

    if (!session_->buffer.select_candidate(*target, static_cast<size_t>(index),
                                           config_->move_cursor_after_selection)) {
        return false;
    }
    (void)transition_to(InputStateKind::Inputting);
    request_prediction();
    redraw();
    return true;
}

bool InputProcessor::select_symbol_impl(int index, std::uint64_t epoch) {
    if (!session_->symbol_menu.matches(epoch) || index < 0 ||
        index >= static_cast<int>(session_->symbol_menu.menu().size())) {
        return false;
    }

    char32_t symbol = 0;
    if (!session_->symbol_menu.select(static_cast<size_t>(index), symbol)) {
        // Descended into a category level; keep the menu open.
        reset_candidate_view();
        redraw();
        return true;
    }

    session_->symbol_menu.close();
    session_->displayed_candidates.clear();
    reset_candidate_view();
    if (!session_->buffer.add_literal(symbol)) return false;
    (void)transition_to(InputStateKind::Inputting);
    mark_prediction_dirty();
    redraw();
    return true;
}

bool InputProcessor::commit_mixed_candidate_impl(int index) {
    if (!session_->mixed_decision.active() || index < 0) return false;
    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    static_cast<size_t>(candidate_page_size(*session_, *config_)),
                                                    session_->mixed_decision.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    auto text = composition_with_pending(session_->buffer, entries[static_cast<size_t>(index)].text);
    session_->buffer.clear();
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    session_->symbol_menu.close();
    (void)transition_to(InputStateKind::Empty);
    session_->prediction.clear_pending();
    commit_text(std::move(text));
    redraw();
    return true;
}

bool InputProcessor::select_mixed_candidate_impl(int index) {
    if (!session_->mixed_decision.active() || index < 0) return false;
    if (session_->mixed_decision.source_revision != session_->pending_token.revision) return false;

    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    static_cast<size_t>(candidate_page_size(*session_, *config_)),
                                                    session_->mixed_decision.preview_path);
    if (index >= static_cast<int>(entries.size())) return false;

    const auto& entry = entries[static_cast<size_t>(index)];
    if (entry.path_index == 0) {
        settle_pending_as_literals();
        redraw();
        return true;
    }
    if (entry.path_index >= session_->mixed_decision.result.paths.size()) return false;
    return apply_mixed_path(session_->mixed_decision.result.paths[entry.path_index], entry.char_index);
}

bool InputProcessor::show_mixed_candidates() {
    if (!session_->mixed_decision.active() ||
        session_->mixed_decision.source_revision != session_->pending_token.revision) {
        return false;
    }

    const auto entries = decoder_.expand_candidates(session_->mixed_decision.result,
                                                    static_cast<size_t>(candidate_page_size(*session_, *config_)),
                                                    session_->mixed_decision.preview_path);
    if (entries.size() < 2) return false;
    (void)transition_to(InputStateKind::ChoosingCandidate);
    session_->candidate_view.cursor = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].path_index == session_->mixed_decision.preview_path &&
            entries[i].char_index == session_->mixed_decision.preview_character) {
            session_->candidate_view.cursor = static_cast<int>(i);
            break;
        }
    }
    session_->candidate_view.page = session_->candidate_view.cursor / candidate_page_size(*session_, *config_);
    redraw();
    return true;
}

bool InputProcessor::apply_mixed_path(const MixedPath& path, std::size_t char_index, bool manual) {
    CompositionBuffer next = session_->buffer;
    for (size_t i = 0; i < path.segments.size(); ++i) {
        const auto& segment = path.segments[i];
        if (segment.consumed_boundary) continue;
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            const auto result =
                next.add_bopomofo_keys(segment.body_keys, segment.tone_key, session_->pending_token.layout, true,
                                      segment.reading);
            if (!result || !result->completed) return false;
            (void)next.set_segment_candidates(result->segment_index, segment.candidates);
            const size_t candidate_index = i + 1 == path.segments.size() ? char_index : 0;
            if (candidate_index >= segment.candidates.size()) return false;
            if (manual && !next.select_candidate(result->segment_index, candidate_index,
                                       config_->move_cursor_after_selection)) {
                return false;
            }
        } else {
            for (const char16_t ch : segment.raw) (void)next.add_literal(static_cast<char32_t>(ch));
        }
    }
    session_->buffer = std::move(next);
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    (void)transition_to(InputStateKind::Inputting);
    mark_prediction_dirty();
    request_prediction();
    redraw();
    return true;
}

bool InputProcessor::handle_english_letter(char32_t letter, bool caps_on) {
    if (config_->shift_letter_keys == "directly_put_to_buffer") {
        if (!session_->pending_token.empty()) (void)settle_pending_preview();
        const char32_t lower = ascii_lower(letter);
        const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
        if (!session_->buffer.add_literal(caps_on ? upper : lower)) return false;
        (void)transition_to(InputStateKind::Inputting);
        redraw();
        return true;
    }

    // DirectlyOutputUppercase: an empty composition passes the key through,
    // a non-empty composition commits together with the uppercase letter.
    if (composition_empty(*session_)) return false;
    const char32_t upper = letter >= U'a' && letter <= U'z' ? letter + (U'A' - U'a') : letter;
    commit_composition_with(upper);
    return true;
}

void InputProcessor::handle_escape() {
    // Marking only changes the selection, so Escape drops it first without
    // touching the composition itself.
    if (session_->buffer.clear_selection()) {
        redraw();
        consume();
        return;
    }
    const auto manual_target = session_->buffer.manually_chosen_segment_at_caret();
    const auto action = escape_action(config_->esc_clears_entire_buffer, session_->kind(),
                                      !available_candidates(*session_, *config_).empty(),
                                      session_->buffer.has_unfinished_reading(), manual_target.has_value());
    switch (action) {
        case EscapeAction::ClearBuffer:
            session_->buffer.clear();
            (void)transition_to(InputStateKind::Empty);
            session_->prediction.clear_pending();
            redraw();
            consume();
            return;
        case EscapeAction::CloseCandidateList:
            (void)transition_to(InputStateKind::Inputting);
            redraw();
            consume();
            return;
        case EscapeAction::ClearUnfinishedReading:
            if (!session_->buffer.clear_unfinished_reading()) return;
            (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
            mark_prediction_dirty();
            redraw();
            consume();
            return;
        case EscapeAction::CancelCandidateSelection:
            if (!manual_target || !session_->buffer.cancel_candidate_selection(*manual_target)) return;
            (void)transition_to(InputStateKind::Inputting);
            request_prediction();
            redraw();
            consume();
            return;
        case EscapeAction::KeepBuffer:
            (void)transition_to(InputStateKind::Inputting);
            redraw();
            consume();
            return;
    }
}

void InputProcessor::open_symbol_menu() {
    session_->symbol_menu.open();
    if (session_->choosing_candidate()) {
        (void)transition_to(InputStateKind::Inputting);
    } else {
        session_->displayed_candidates.clear();
        reset_candidate_view();
    }
    redraw();
}

void InputProcessor::close_symbol_menu() {
    session_->symbol_menu.close();
    session_->displayed_candidates.clear();
    reset_candidate_view();
    if (session_->buffer.empty()) {
        if (!session_->empty()) (void)transition_to(InputStateKind::Empty);
    } else {
        (void)transition_to(InputStateKind::Inputting);
    }
    redraw();
}

bool InputProcessor::is_smart_start_char(char32_t key, BopomofoKeyboardLayout layout) const {
    // A leading underscore is an ASCII identifier start, including private
    // names. Keep it reversible instead of turning it into Chinese punctuation.
    if (key == U'_') return true;
    if (key >= U'a' && key <= U'z') return true;
    if (const auto symbol = lookup_bopomofo_key(key, layout)) return !is_bopomofo_tone(*symbol);
    return false;
}

// Re-decode the exact raw pending keys. A language decision only changes the
// preedit preview; it is not written into the composition until confirmation.
void InputProcessor::rerun_pending_decision() {
    if (session_->pending_token.empty()) return;
    auto context = session_->context_text;
    context += session_->buffer.rendered_prefix_before_caret();
    auto result = decoder_.decode(session_->pending_token.raw, session_->pending_token.layout,
                                  false, context);
    const size_t preview = result.best_path;
    set_mixed_preview(std::move(result), preview);
}

void InputProcessor::backspace_pending(bool undo_raw_key) {
    auto& pending = session_->pending_token;
    const auto& decision = session_->mixed_decision;
    std::optional<MixedPath> survivor;
    if (!undo_raw_key && decision.active() && decision.source_revision == pending.revision &&
        decision.preview_path < decision.result.paths.size()) {
        auto path = decision.result.paths[decision.preview_path];
        // A consumed Space has no displayed character. Delete the last
        // displayed unit together with its raw boundary, not an invisible key.
        while (!path.segments.empty() && path.segments.back().consumed_boundary) path.segments.pop_back();
        if (!path.segments.empty()) {
            auto& tail = path.segments.back();
            if (tail.kind == MixedSegmentKind::Bopomofo && !tail.candidates.empty()) {
                pending.raw.resize(tail.begin);
                ++pending.revision;
                const auto character = tail.candidates.front();
                path.rendered.resize(path.rendered.size() - (character > 0xffff ? 2U : 1U));
                path.segments.pop_back();
                survivor = std::move(path);
            } else if (!tail.raw.empty()) {
                // Literal letters/digits/symbols remain one-character edits;
                // retain the visible prefix instead of guessing its language
                // again just because the user deleted a trailing character.
                pending.raw.resize(tail.end - 1);
                ++pending.revision;
                tail.raw.pop_back();
                --tail.end;
                // An unfinished reading is displayed as these same raw keys.
                // Retain that literal prefix for this edit; fresh candidate
                // interpretations remain available in the decoded result.
                if (tail.kind == MixedSegmentKind::BopomofoIncomplete) {
                    tail.kind = MixedSegmentKind::Symbol;
                    tail.reading.clear();
                }
                path.rendered.pop_back();
                if (tail.raw.empty()) path.segments.pop_back();
                survivor = std::move(path);
            }
        }
    }
    if (!survivor) pending.pop(); // unfinished reading or explicit raw-key undo
    session_->mixed_decision.clear();
    (void)transition_to(composition_empty(*session_) ? InputStateKind::Empty : InputStateKind::Inputting);
    if (pending.empty()) { redraw(); return; }
    rerun_pending_decision();
    if (survivor && !survivor->segments.empty()) {
        auto& result = session_->mixed_decision.result;
        // The retained, previously displayed path is valid provenance, even
        // when a fresh beam would prune it. Raw remains path zero and no text
        // was manually selected or silently committed by this operation.
        const auto found = std::ranges::find_if(result.paths, [&](const auto& path) {
            return path.rendered == survivor->rendered && std::ranges::equal(path.segments, survivor->segments,
                [](const auto& a, const auto& b) {
                    return a.kind == b.kind && a.begin == b.begin && a.end == b.end &&
                           a.reading == b.reading && a.raw == b.raw && a.consumed_boundary == b.consumed_boundary;
                });
        });
        const auto preview = found == result.paths.end() ? result.paths.size() :
            static_cast<std::size_t>(std::distance(result.paths.begin(), found));
        if (found == result.paths.end()) result.paths.push_back(std::move(*survivor));
        session_->mixed_decision.preview_path = preview;
        redraw();
    }
}

void InputProcessor::set_mixed_preview(MixedDecodeResult result, std::size_t preview_path) {
    session_->mixed_decision.result = std::move(result);
    session_->mixed_decision.source_revision = session_->pending_token.revision;
    session_->mixed_decision.preview_path = preview_path;
    session_->mixed_decision.preview_character = 0;
    (void)transition_to(InputStateKind::Inputting);
    redraw();
}

void InputProcessor::append_pending_char(char32_t key, BopomofoKeyboardLayout layout) {
    if (session_->pending_token.empty()) (void)session_->buffer.clear_selection(false);
    session_->pending_token.push(key, layout);
    (void)transition_to(InputStateKind::Inputting);
    redraw();
}

void InputProcessor::settle_pending_as_literals() {
    for (const char16_t ch : session_->pending_token.raw) (void)session_->buffer.add_literal(static_cast<char32_t>(ch));
    session_->pending_token.clear();
    session_->mixed_decision.clear();
    (void)transition_to(session_->buffer.empty() ? InputStateKind::Empty : InputStateKind::Inputting);
}

bool InputProcessor::settle_pending_preview() {
    if (session_->mixed_decision.active() &&
        session_->mixed_decision.source_revision == session_->pending_token.revision &&
        session_->mixed_decision.preview_path > 0 &&
        session_->mixed_decision.preview_path < session_->mixed_decision.result.paths.size()) {
        return apply_mixed_path(session_->mixed_decision.result.paths[session_->mixed_decision.preview_path],
                                session_->mixed_decision.preview_character, false);
    }
    settle_pending_as_literals();
    return true;
}

void InputProcessor::apply_fallback_candidates(InputSession& session, std::size_t segment_index, bool preserve_existing) {
    if (!session.buffer.segment_complete(segment_index)) return;
    // A service failure is not a new reading. Settling a mixed preview already
    // supplies valid candidates; retain them instead of replacing its displayed
    // words with a second fallback ranking. Reading edits clear candidates.
    const auto* existing = session.buffer.segment_candidates(segment_index);
    if (preserve_existing && existing != nullptr && !existing->empty()) return;

    const auto predictions = fallback_.predict(session.buffer);
    if (segment_index >= predictions.size()) return;
    (void)session.buffer.refresh_segment_candidates(segment_index, predictions[segment_index].candidates);
}

void InputProcessor::apply_prediction(InputSession& session, const protocol::Prediction& prediction) {
    for (std::size_t i = 0; i < session.prediction.segment_indices.size(); ++i) {
        const auto index = session.prediction.segment_indices[i];
        if (index >= session.buffer.segments().size()) continue;
        const auto& segment = session.buffer.segments()[index];
        // Keep the table's homophones in the candidate list so the user can
        // always pick another character, even after a model response.
        (void)session.buffer.refresh_segment_candidates(
            index, fallback_.merge_model_candidates(segment, prediction.candidates[i]));
    }
}

void InputProcessor::reset_state(InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    (void)transition_to(InputStateKind::Empty);
    session_ = nullptr;
    config_ = nullptr;
}

void InputProcessor::sync_state(InputSession& session, const Config& config) {
    session_ = &session;
    config_ = &config;
    if (composition_empty(session)) {
        if (!session.empty()) (void)transition_to(InputStateKind::Empty);
    } else if (session.empty()) {
        (void)transition_to(InputStateKind::Inputting);
    }
    session_ = nullptr;
    config_ = nullptr;
}

void InputProcessor::apply_phrase_override(InputSession& session) {
    auto& buffer = session.buffer;
    const auto& segments = buffer.segments();

    // A segment can take part in a stored phrase unless it is unfinished, a
    // literal, or was explicitly chosen by the user.
    const auto usable = [](const Segment& segment) {
        return segment.complete() && segment.literal == 0 && segment.visible_candidate() &&
               !(segment.manually_chosen && !segment.phrase_override_chosen);
    };

    // Existing pins are validated on their own readings: typing more of the
    // composition must not revert the forced text, while editing the pinned
    // range or choosing another candidate inside it releases the pin.
    size_t index = 0;
    while (index < segments.size()) {
        if (!segments[index].phrase_override_chosen) {
            ++index;
            continue;
        }
        size_t end = index;
        std::vector<std::u16string> readings;
        bool valid = true;
        while (end < segments.size() && segments[end].phrase_override_chosen) {
            valid = valid && usable(segments[end]);
            readings.push_back(segments[end].reading());
            ++end;
        }
        if (valid) valid = static_cast<bool>(phrase_overrides_.lookup(readings));
        if (!valid) (void)buffer.clear_phrase_override_choices(index, end - index);
        index = end;
    }

    // Pin stored phrases wherever their readings appear. Leftmost-longest
    // wins so overlapping entries cannot fight over the same readings.
    index = 0;
    while (index < segments.size()) {
        if (!usable(segments[index])) {
            ++index;
            continue;
        }
        size_t usable_length = 0;
        while (index + usable_length < segments.size() && usable(segments[index + usable_length])) {
            ++usable_length;
        }
        if (usable_length < PhraseOverrideStore::kMinReadings) {
            ++index;
            continue;
        }

        size_t matched = 0;
        const size_t longest = std::min(usable_length, PhraseOverrideStore::kMaxReadings);
        for (size_t length = longest; length >= PhraseOverrideStore::kMinReadings; --length) {
            std::vector<std::u16string> readings;
            readings.reserve(length);
            for (size_t i = index; i < index + length; ++i) readings.push_back(segments[i].reading());

            const auto phrase = phrase_overrides_.lookup(readings);
            if (!phrase) continue;

            try {
                const auto codepoints = utf8_to_u32(u16_to_utf8(*phrase));
                if (buffer.apply_phrase_override(index, codepoints)) matched = length;
            } catch (...) {
            }
            break;
        }
        index += matched != 0 ? matched : 1;
    }
}

bool InputProcessor::save_marked_phrase_override() {
    const auto readings = session_->buffer.marked_readings();
    if (!PhraseOverrideStore::valid_entry(session_->buffer.marked_text(), readings.size())) return false;
    return phrase_overrides_.add(session_->buffer.marked_text(), readings);
}

bool InputProcessor::composition_empty(const InputSession& session) {
    return session.buffer.empty() && session.pending_token.empty();
}

bool InputProcessor::candidate_list_active(const InputSession& session) {
    return session.choosing_candidate() && !session.displayed_candidates.empty();
}

int InputProcessor::candidate_page_size(const InputSession& session, const Config& config) {
    return session.candidate_view.page_size(config.candidate_page_size, session.displayed_candidates.size());
}

int InputProcessor::candidate_page_offset(const InputSession& session, const Config& config) {
    return session.candidate_view.page_offset(config.candidate_page_size, session.displayed_candidates.size());
}

void InputProcessor::clamp_candidate_cursor(InputSession& session, const Config& config) {
    session.candidate_view.clamp(config.candidate_page_size, session.displayed_candidates.size());
}

std::vector<char32_t> InputProcessor::available_candidates(const InputSession& session, const Config& config) {
    const auto target = current_candidate_target(session, config);
    if (!target) return {};

    const auto* candidates = session.buffer.segment_candidates(*target);
    if (candidates == nullptr) return {};
    return *candidates;
}

std::optional<std::size_t> InputProcessor::current_candidate_target(const InputSession& session,
                                                                    const Config& config) {
    const auto mode = config.select_phrase == "after_cursor" ? CandidateTarget::AfterCursor
                                                             : CandidateTarget::BeforeCursor;
    if (const auto target = session.buffer.candidate_target(mode)) return target;

    // The caret sits at a boundary where the configured side has no segment to
    // select (e.g. position 0 with before-cursor selection). Fall back to the
    // other side so the candidate list can still open and stay visible instead
    // of disappearing (macOS frontend hides the panel when the list is empty).
    const auto fallback = mode == CandidateTarget::BeforeCursor ? CandidateTarget::AfterCursor
                                                               : CandidateTarget::BeforeCursor;
    return session.buffer.candidate_target(fallback);
}

std::u16string InputProcessor::pending_rendered_text(const InputSession& session) {
    if (session.pending_token.empty()) return {};
    if (!session.mixed_decision.active() ||
        session.mixed_decision.source_revision != session.pending_token.revision ||
        session.mixed_decision.result.raw != session.pending_token.raw ||
        session.mixed_decision.preview_path >= session.mixed_decision.result.paths.size()) {
        return session.pending_token.raw;
    }
    return session.mixed_decision.result.paths[session.mixed_decision.preview_path].rendered;
}

std::u16string InputProcessor::current_preedit(const InputSession& session) {
    return composition_with_pending(session.buffer, pending_rendered_text(session));
}

std::u16string InputProcessor::marking_hint_text(const InputSession& session) {
    const auto readings = session.buffer.marked_readings();
    std::u16string hint = u"強制替代詞彙：「" + session.buffer.marked_text() + u"」";
    if (readings.empty()) {
        hint += u"（含未完成的字）— Esc 取消";
    } else if (PhraseOverrideStore::valid_entry(session.buffer.marked_text(), readings.size())) {
        hint += u" — 按 Enter 加入、Esc 取消";
    } else {
        hint += u"（需選取 2 至 8 個字）— Esc 取消";
    }
    return hint;
}

}  // namespace llavon::ime

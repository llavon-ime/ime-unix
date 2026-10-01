#include "host/render_state.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "input/input_processor.hpp"
#include "input/mixed_input_decoder.hpp"
#include "text/utf.hpp"

namespace llavon::ime {
namespace {

std::u16string to_utf16(char32_t value) {
    return utf8_to_u16(char32_to_utf8(value));
}

std::u16string to_utf16(const std::u32string& value) {
    std::u16string text;
    for (const char32_t codepoint : value) text += to_utf16(codepoint);
    return text;
}

}  // namespace

std::u16string preedit_text(const RenderState& state) {
    std::u16string text;
    for (const auto& segment : state.preedit) text += segment.text;
    return text;
}

RenderState build_render_state(InputSession& session, const Config& config, MixedInputDecoder& decoder) {
    RenderState state;
    state.composition_empty = InputProcessor::composition_empty(session);
    state.layout_hint = config.candidate_layout;

    const int selection_key_count =
        std::min(config.selection_key_count, static_cast<int>(config.selection_keys.size()));
    if (selection_key_count > 0) {
        state.selection_keys.assign(config.selection_keys.begin(),
                                    config.selection_keys.begin() + selection_key_count);
    }

    if (!state.composition_empty) {
        // Mirrors the historical panel assembly: the marked range underlines
        // per segment, unmarked compositions render as one segment, and the
        // pending smart-English token always trails the composition.
        auto prefix = session.buffer.rendered_prefix_before_caret();
        const auto pending = InputProcessor::pending_rendered_text(session);
        if (const auto marked = session.buffer.marked_range()) {
            const auto& segments = session.buffer.segments();
            for (std::size_t i = 0; i < segments.size(); ++i) {
                const bool underlined = i >= marked->first && i < marked->second;
                state.preedit.push_back({segments[i].rendered_text(), underlined});
            }
            if (!session.pending_token.empty()) state.preedit.push_back({pending, false});
        } else {
            state.preedit.push_back({InputProcessor::current_preedit(session), false});
        }
        prefix += pending;
        state.caret = prefix.size();
        if (session.buffer.marked_range()) {
            state.aux_up = InputProcessor::marking_hint_text(session);
        }
    }

    if (session.mixed_decision.active() && session.choosing_candidate()) {
        session.displayed_candidates.clear();
        const auto entries = decoder.expand_candidates(session.mixed_decision.result,
                                                       static_cast<std::size_t>(InputProcessor::candidate_page_size(session, config)),
                                                       session.mixed_decision.preview_path);
        for (const auto& entry : entries) session.displayed_candidates.push_back(entry.text);
        state.candidate_target = RenderTarget::Candidates;
    } else if (session.symbol_menu.active()) {
        session.displayed_candidates.clear();
        for (const auto& item : session.symbol_menu.menu()) {
            session.displayed_candidates.push_back(to_utf16(item));
        }
        state.candidate_target = RenderTarget::SymbolMenu;
        state.symbol_epoch = session.symbol_menu.epoch();
    } else if (session.buffer.marked_range() && !session.choosing_candidate()) {
        // The marking hint doubles as a tooltip rendered as a candidate list.
        session.displayed_candidates.assign(1, InputProcessor::marking_hint_text(session));
        state.candidate_target = RenderTarget::MarkingHint;
    } else {
        session.displayed_candidates.clear();
        if (session.choosing_candidate()) {
            for (const char32_t candidate : InputProcessor::available_candidates(session, config)) {
                session.displayed_candidates.push_back(to_utf16(candidate));
            }
            state.candidate_target = RenderTarget::Candidates;
        }
    }

    if (session.displayed_candidates.empty()) {
        session.candidate_view.reset();
        return state;
    }

    InputProcessor::clamp_candidate_cursor(session, config);
    const int page_size = InputProcessor::candidate_page_size(session, config);
    const int page_count = static_cast<int>(
        (session.displayed_candidates.size() + static_cast<std::size_t>(page_size) - 1) /
        static_cast<std::size_t>(page_size));
    if (session.candidate_view.page >= page_count) session.candidate_view.page = page_count - 1;
    if (session.candidate_view.page < 0) session.candidate_view.page = 0;

    state.has_candidates = true;
    state.candidates = session.displayed_candidates;
    state.page = session.candidate_view.page;
    state.page_size = page_size;
    state.page_count = page_count;

    const auto target = session.symbol_menu.active()
                            ? std::optional<std::size_t>()
                            : InputProcessor::current_candidate_target(session, config);
    state.cursor_visible = session.symbol_menu.active() || session.mixed_decision.active() || target.has_value();
    state.cursor = session.candidate_view.cursor - InputProcessor::candidate_page_offset(session, config);
    return state;
}

}  // namespace llavon::ime

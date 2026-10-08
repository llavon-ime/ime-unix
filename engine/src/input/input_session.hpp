#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "buffer/composition_buffer.hpp"
#include "input/candidate_view.hpp"
#include "input/input_key.hpp"
#include "input/input_state.hpp"
#include "input/mixed_input_decoder.hpp"
#include "input/pending_token.hpp"
#include "input/prediction_state.hpp"
#include "protocol/protocol.hpp"
#include "symbol/symbol_menu.hpp"

namespace llavon::ime {

// Reversible decode state for one pending input. The preview is rendered in
// preedit without destroying the exact raw keys kept by PendingInput.
struct MixedDecisionState {
    MixedDecodeResult result;
    size_t preview_path = 0;
    size_t preview_character = 0;
    uint64_t source_revision = 0;

    bool active() const noexcept { return !result.raw.empty(); }
    void clear() {
        result = MixedDecodeResult{};
        preview_path = 0;
        preview_character = 0;
        source_revision = 0;
    }
};

// Where the last resolved context came from. The memory probe is only worth
// its writes when nothing else produced text.
enum class ContextSource : std::uint8_t { None, Client, Accessibility, Memory };

// Per-input-context editing state. The engine swaps the whole object in and
// out of the active input context, so every field always travels together.
struct InputSession {
    // Identifies this input context across commits, not prediction reconnects.
    protocol::SessionId training_source_id{};
    InputState state;
    CandidateView candidate_view;
    std::vector<std::u16string> displayed_candidates;

    CompositionBuffer buffer;
    SymbolMenuState symbol_menu;
    PendingInput pending_token;
    MixedDecisionState mixed_decision;
    // Text before the caret as read from the current prediction source for the
    // pending request. Never accumulated by this IME.
    std::u16string context_text;
    ContextSource context_source = ContextSource::None;
    PredictionState prediction;
    // An explicit submission waits for both buffer and mixed-preview inference.
    // Any later edit or lifecycle boundary cancels this intent.
    std::optional<InputKey> deferred_commit;

    InputStateKind kind() const { return input_state_kind(state); }
    bool empty() const { return kind() == InputStateKind::Empty; }
    bool inputting() const { return kind() == InputStateKind::Inputting; }
    bool choosing_candidate() const { return kind() == InputStateKind::ChoosingCandidate; }
};

}  // namespace llavon::ime

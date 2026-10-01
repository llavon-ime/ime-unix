#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "input/candidate_view.hpp"
#include "input/input_key.hpp"
#include "input/input_session.hpp"
#include "input/mixed_input_decoder.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include "protocol/protocol.hpp"
#include "symbol/symbol_menu.hpp"

namespace llavon::ime {

// Config values the candidate-list key rules depend on.
struct CandidateKeyConfig {
    int page_size = 10;
    bool space_selects_candidate = true;
    int selection_key_count = 10;
    bool caps_lock_inputs_bopomofo = true;
    std::string_view selection_keys;
};

// What the caller must do after a key was routed while the candidate list is
// active.
enum class CandidateKeyAction {
    // Route the key with the rest of the input rules: the candidate list did
    // not claim it.
    Unhandled,
    // Leave the key to the application without consuming it.
    PassThrough,
    // Consume the key without changing the view.
    Handled,
    // Consume the key and refresh the candidate panel.
    Redraw,
    // Consume the key and select/commit the row under the cursor.
    ActivateCursor,
    // Consume the key and select the candidate at `index`.
    SelectIndex,
};

struct CandidateKeyOutcome {
    CandidateKeyAction action = CandidateKeyAction::Unhandled;
    int index = 0;
};

// Routes one key press while the candidate list is showing. The view and the
// session-owned candidate count are the only state touched.
CandidateKeyOutcome handle_candidate_key(const InputKey& key, const CandidateKeyConfig& config,
                                         CandidateView& view, std::size_t candidate_count,
                                         bool mixed_decision_active, bool has_chewing_punctuation);

// What the caller must do after a key was routed while the symbol menu is
// showing. Every key is consumed; the action only describes the effect.
enum class SymbolMenuKeyAction {
    // Consume the key without other effects.
    Handled,
    // Consume the key and refresh the panel.
    Redraw,
    // Consume the key and close the symbol menu.
    CloseMenu,
    // Consume the key and select the item under the cursor.
    ActivateCursor,
    // Consume the key and select the item at `index`.
    SelectIndex,
};

struct SymbolMenuKeyOutcome {
    SymbolMenuKeyAction action = SymbolMenuKeyAction::Handled;
    int index = 0;
};

// Routes one key press while the symbol menu is open. Unlike the candidate
// list, every key is claimed by the menu; paging past an end is a no-op.
SymbolMenuKeyOutcome handle_symbol_menu_key(const InputKey& key, const CandidateKeyConfig& config,
                                            SymbolMenuState& menu, CandidateView& view,
                                            std::size_t candidate_count);

// Index of the selection key in the configured key string, if the key is one.
// Letters normalize by CapsLock handling, matching how they reach the rules.
std::optional<int> selection_index_for_key(char32_t symbol, std::string_view selection_keys,
                                           int selection_key_count, bool caps_lock_inputs_bopomofo);

// What the frontend must do after the input rules handled a key.
struct InputEffect {
    // Consume the key event; false leaves it to the application.
    bool handled = false;
    // Refresh the preedit and candidate panel.
    bool redraw = false;
    // Start or refresh an asynchronous prediction.
    bool request_prediction = false;
    // Text to commit to the application (empty when there is nothing to commit).
    std::u16string commit;
    // Populated only for a committed, one-character-per-reading Bopomofo
    // composition. Captured before the buffer is cleared.
    struct CommitSample {
        std::u16string answer;
        struct Entry {
            std::u16string reading;
            char32_t character = 0;
            bool manually_selected = false;
            // Literal positions carry no reading and are context only; a
            // sample without any composed position is not training data.
            bool literal = false;
        };
        std::vector<Entry> entries;
    };
    std::optional<CommitSample> training_sample;
};

enum class InputResetReason { Explicit, FocusOut, Deactivate };

// Routes keys into an InputSession. Every input rule lives here so it can be
// tested without a frontend; the engine only applies the returned effects.
class InputProcessor {
public:
    using StateObserver = std::function<void(InputStateKind previous, InputStateKind next)>;

    InputProcessor(FallbackEngine& fallback, MixedInputDecoder& decoder, PhraseOverrideStore& phrase_overrides);

    void set_state_observer(StateObserver observer);

    // Routes one key press.
    [[nodiscard]] InputEffect process(const InputKey& key, InputSession& session, const Config& config);

    // Candidate activation from the input panel (mouse or selection key).
    [[nodiscard]] InputEffect select_candidate(InputSession& session, const Config& config, int index);
    [[nodiscard]] InputEffect select_symbol(InputSession& session, const Config& config, int index, std::uint64_t epoch);

    // Lifecycle operations initiated by the frontend adapter.
    [[nodiscard]] InputEffect reset(InputSession& session, const Config& config, InputResetReason reason,
                      bool clear_context);
    void prepare_for_config_change(InputSession& session);

    // Prediction support: fallback candidates, model candidates, and phrase
    // overrides.
    void apply_fallback_candidates(InputSession& session, std::size_t segment_index);
    void apply_prediction(InputSession& session, const protocol::Prediction& prediction);
    void apply_phrase_override(InputSession& session);

    // State synchronization used outside key routing: focus changes and panel
    // rendering.
    void reset_state(InputSession& session, const Config& config);
    void sync_state(InputSession& session, const Config& config);

    // Read-only helpers for the frontend when rendering the input panel.
    static bool composition_empty(const InputSession& session);
    static bool candidate_list_active(const InputSession& session);
    static int candidate_page_size(const InputSession& session, const Config& config);
    static int candidate_page_offset(const InputSession& session, const Config& config);
    static void clamp_candidate_cursor(InputSession& session, const Config& config);
    static std::vector<char32_t> available_candidates(const InputSession& session, const Config& config);
    static std::optional<std::size_t> current_candidate_target(const InputSession& session, const Config& config);
    static std::u16string pending_rendered_text(const InputSession& session);
    static std::u16string current_preedit(const InputSession& session);
    static std::u16string marking_hint_text(const InputSession& session);

private:
    // Entry points set the session and config for the duration of a call.
    InputSession* session_ = nullptr;
    const Config* config_ = nullptr;
    InputEffect effect_;

    void process_impl(const InputKey& key);
    void backspace_pending(bool undo_raw_key);

    void consume();
    void redraw();
    void request_prediction();
    void commit_text(std::u16string text);
    void mark_prediction_dirty();

    bool transition_to(InputStateKind state);
    void reset_candidate_view();
    std::size_t candidate_count() const;
    bool page_candidates(int delta, bool preserve_cursor_offset = false);
    bool set_candidate_cursor(int index);
    CandidateKeyConfig candidate_key_config() const;

    void commit_current();
    void commit_composition_with(char32_t extra);
    bool select_candidate_impl(int index);
    bool select_symbol_impl(int index, std::uint64_t epoch);
    bool commit_mixed_candidate_impl(int index);
    bool select_mixed_candidate_impl(int index);
    bool show_mixed_candidates();
    bool apply_mixed_path(const MixedPath& path, std::size_t char_index, bool manual = true);
    bool handle_english_letter(char32_t letter, bool caps_on);
    void handle_escape();
    void open_symbol_menu();
    void close_symbol_menu();

    bool is_smart_start_char(char32_t key, BopomofoKeyboardLayout layout) const;
    void rerun_pending_decision();
    void set_mixed_preview(MixedDecodeResult result, std::size_t preview_path);
    void append_pending_char(char32_t key, BopomofoKeyboardLayout layout);
    void settle_pending_as_literals();
    bool settle_pending_preview();

    bool save_marked_phrase_override();

    FallbackEngine& fallback_;
    MixedInputDecoder& decoder_;
    PhraseOverrideStore& phrase_overrides_;
    StateObserver state_observer_;
};

}  // namespace llavon::ime

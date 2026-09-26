#include "host/engine.hpp"

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include "context/memory_context.hpp"
#include "context/sample_adoption.hpp"
#include "debug/context_log.hpp"
#include "host/render_state.hpp"
#include "input/input_processor.hpp"
#include "input/mixed_input_decoder.hpp"
#include "text/utf.hpp"
#include "util/env.hpp"

namespace llavon::ime {

namespace {

// True when the client itself gives the caret context, so the memory probe has
// nothing to add and must not write anything.
bool client_reports_text(Host& host, ContextId context) {
    const auto surrounding = host.surrounding_text(context);
    return surrounding.valid && surrounding.cursor > 0;
}

// The memory probe helper is resolved like the installed service: an
// environment override first, then the path compiled in from CMake.
std::filesystem::path memory_helper_path() {
    if (const char* override = env_with_legacy("LLAVON_IME_MEMSCAN_PATH", "IME_FCITX5_MEMSCAN_PATH");
        override != nullptr && override[0] != '\0') {
        return override;
    }
#ifdef LLAVON_IME_INSTALLED_MEMSCAN_PATH
    return LLAVON_IME_INSTALLED_MEMSCAN_PATH;
#else
    return {};
#endif
}

}  // namespace

Engine::Engine(EngineOptions options, Host& host)
    : options_(std::move(options)),
      host_(host),
      fallback_(options_.table_path),
      decoder_([this](std::u16string_view reading) { return fallback_.lookup(reading); },
               [this](std::u16string_view word) { return fallback_.latin_frequency(word); }),
      phrase_overrides_(options_.phrase_overrides_path),
      processor_(fallback_, decoder_, phrase_overrides_),
      transport_(options_.transport),
      config_(options_.config),
      on_training_commit_(options_.on_training_commit),
      on_training_discard_(options_.on_training_discard),
      correction_window_(options_.commit_correction_window) {
    processor_.set_state_observer([this](InputStateKind previous, InputStateKind next) {
        if (previous == InputStateKind::Empty && next == InputStateKind::Inputting) {
            // Samples published from here on may contain this composition's
            // preedit; earlier ones cannot.
            if (accessibility_context_) {
                accessibility_composition_base_ = accessibility_context_->sequence();
            }
            if (memory_context_) {
                memory_composition_base_ = memory_context_->sequence();
            }
        } else if (next == InputStateKind::Empty) {
            accessibility_composition_base_ = 0;
            memory_composition_base_ = 0;
            // A completed/cancelled preedit no longer identifies the same
            // client memory. In-flight scans must not publish it afterwards.
            if (memory_context_) memory_context_->invalidate();
        }
    });
    apply_context_sources();
    // Hosts like the native macOS app have no explicit init hook; load the
    // phrase overrides here so every host sees the same settings file.
    (void)phrase_overrides_.load();
}

Engine::~Engine() {
    alive_.reset();
    // Transport callbacks hold a weak lifetime token; drain them while the
    // engine is still alive.
    transport_.stop();
}

void Engine::attach(ContextId context) {
    (void)find_or_create(context);
}

void Engine::detach(ContextId context) {
    const auto it = sessions_.find(context);
    if (it == sessions_.end()) return;
    if (memory_probe_context_ == context && memory_context_) memory_context_->invalidate();
    close_prediction_session(*it->second);
    sessions_.erase(it);
    if (recent_commit_ && recent_commit_->context == context) recent_commit_.reset();
    if (memory_probe_context_ == context) memory_probe_context_ = 0;
}

bool Engine::has_context(ContextId context) const {
    return sessions_.find(context) != sessions_.end();
}

bool Engine::key_event(ContextId context, const InputKey& key) {
    // Releases reach the engine so key policy lives here instead of in the
    // host; they carry no input meaning today.
    if (key.release) return false;
    // An immediate Backspace withdraws the commit the user is correcting; any
    // other key means the correction window is over.
    if (key.sym == keysym::BackSpace) withdraw_recent_commit(context);
    else recent_commit_.reset();
    auto& session = find_or_create(context);
    const auto effect = processor_.process(key, session, config_);
    apply_effect(context, session, effect);
    if (!effect.handled) {
        // The application receives this key, so the caret may have moved away
        // from the text the memory probe would locate; a paste or undo may
        // also have changed the context, so probe again.
        if (memory_context_) memory_context_->invalidate();
        if (should_probe_memory(context, session)) {
            memory_context_->refresh();
        }
    }
    return effect.handled;
}

void Engine::select_candidate(ContextId context, int index) {
    auto* session = find(context);
    if (session == nullptr) return;
    apply_effect(context, *session, processor_.select_candidate(*session, config_, index));
}

void Engine::select_symbol(ContextId context, int index, std::uint64_t epoch) {
    auto* session = find(context);
    if (session == nullptr) return;
    apply_effect(context, *session, processor_.select_symbol(*session, config_, index, epoch));
}

void Engine::activate(ContextId context) {
    if (memory_context_) memory_context_->invalidate();
    memory_probe_context_ = context;
    if (accessibility_context_) {
        accessibility_context_->set_active(true);
        accessibility_base_sequence_ = accessibility_context_->sequence();
        accessibility_context_->refresh();
    }
    if (memory_context_) {
        memory_context_->set_active(true);
        memory_base_sequence_ = memory_context_->sequence();
    }
    host_.update_ui(context);
}

void Engine::deactivate(ContextId context) {
    if (memory_context_) memory_context_->invalidate();
    if (memory_probe_context_ == context) memory_probe_context_ = 0;
    if (accessibility_context_) {
        accessibility_context_->set_active(false);
        accessibility_base_sequence_ = accessibility_context_->sequence();
    }
    if (memory_context_) {
        memory_context_->set_active(false);
        memory_base_sequence_ = memory_context_->sequence();
    }
    reset(context, InputResetReason::Deactivate, true);
}

void Engine::reset(ContextId context, InputResetReason reason, bool clear_context) {
    auto* session = find(context);
    if (session == nullptr) return;
    if (memory_context_) memory_context_->invalidate();
    if (recent_commit_ && recent_commit_->context == context) recent_commit_.reset();
    if (reason == InputResetReason::FocusOut) {
        if (memory_probe_context_ == context) memory_probe_context_ = 0;
        if (accessibility_context_) {
            accessibility_context_->set_active(false);
            accessibility_base_sequence_ = accessibility_context_->sequence();
        }
        if (memory_context_) {
            memory_context_->set_active(false);
            memory_base_sequence_ = memory_context_->sequence();
        }
    }
    const auto effect = processor_.reset(*session, config_, reason, clear_context);
    apply_effect(context, *session, effect);
}

RenderState Engine::render_state(ContextId context) {
    auto* session = find(context);
    if (session == nullptr) return {};
    processor_.sync_state(*session, config_);
    return build_render_state(*session, config_, decoder_);
}

void Engine::set_config(Config config, bool settle_sessions) {
    config_ = std::move(config);
    apply_context_sources();
    if (!settle_sessions) return;
    for (auto& [context, session] : sessions_) {
        processor_.prepare_for_config_change(*session);
        close_prediction_session(*session);
    }
}

void Engine::set_transport_options(ServiceTransportOptions options) {
    transport_.reconfigure(std::move(options));
    // The previous service process staged its commits; it cannot withdraw them.
    recent_commit_.reset();
    // Prediction sessions belonged to the service process that just went away.
    // Late responses are ignored and each context opens a fresh session on
    // the next prediction.
    for (auto& [context, session] : sessions_) {
        session->prediction.invalidate();
        session->prediction.session_id = {};
    }
}

void Engine::reload_phrase_overrides() {
    (void)phrase_overrides_.load();
}

void Engine::clear_context_text(ContextId context) {
    auto* session = find(context);
    if (session != nullptr) {
        session->context_text.clear();
        session->context_source = ContextSource::None;
    }
    if (memory_probe_context_ == context && memory_context_) memory_context_->invalidate();
}

AccessibilityContextState Engine::accessibility_state() const {
    if (accessibility_context_ == nullptr) return AccessibilityContextState{};
    return accessibility_context_->availability();
}

AccessibilityContextState Engine::memory_context_state() const {
    if (memory_context_ == nullptr) return AccessibilityContextState{};
    return memory_context_->availability();
}

InputSession* Engine::find(ContextId context) {
    const auto it = sessions_.find(context);
    return it == sessions_.end() ? nullptr : it->second.get();
}

InputSession& Engine::find_or_create(ContextId context) {
    auto it = sessions_.find(context);
    if (it == sessions_.end()) {
        it = sessions_.emplace(context, std::make_unique<InputSession>()).first;
        std::random_device random;
        for (auto& byte : it->second->training_source_id) byte = static_cast<std::uint8_t>(random());
    }
    return *it->second;
}

void Engine::apply_effect(ContextId context, InputSession& session, const InputEffect& effect) {
    if (!effect.commit.empty()) {
        // Capture the context before the host inserts the committed text.
        // The prediction path already strips client preedit when sampling it.
        const auto training_context = session.context_text;
        const auto accessibility_sequence = accessibility_context_ ? accessibility_context_->sequence() : 0;
        const bool allow_training = effect.training_sample && config_.collect_training_data &&
                                    !host_.is_sensitive(context);
        host_.commit(context, effect.commit);
        if (allow_training) {
            try {
                protocol::RecordCommitRequest request;
                std::random_device random;
                for (auto& byte : request.event_id) byte = static_cast<std::uint8_t>(random());
                request.source_id = session.training_source_id;
                if (on_training_commit_) {
                    on_training_commit_(*effect.training_sample, training_context);
                } else {
                    request.context = utf16_tail(training_context, 4096);
                    request.answer = effect.training_sample->answer;
                    for (const auto& entry : effect.training_sample->entries) {
                        request.entries.push_back(
                            {entry.reading, entry.character, entry.manually_selected, entry.literal});
                    }
                    transport_.record_commit(request);
                }
                // An immediate Backspace may still withdraw this commit.
                remember_recent_commit(context, request.event_id,
                                       utf16_tail(training_context + effect.commit, 256), accessibility_sequence);
            } catch (...) {
                // Recording must never prevent a successful text commit.
            }
        }
    }
    if (effect.redraw) host_.update_ui(context);
    // Incomplete Bopomofo readings redraw without requesting a prediction.
    // Those intermediate states are precisely what lets the memory probe
    // validate the same location across natural preedit changes.
    if (effect.redraw && !InputProcessor::composition_empty(session) &&
        should_probe_memory(context, session)) {
        memory_context_->refresh();
    }
    // Memory probing observes the client's natural preedit; publish the new
    // preedit before the prediction path schedules its memory scan.
    if (effect.request_prediction) request_prediction(context, session);
}

void Engine::remember_recent_commit(ContextId context, const protocol::SessionId& event_id,
                                    std::u16string_view committed_tail, std::uint64_t accessibility_sequence) {
    recent_commit_ = RecentCommit{context, event_id, std::chrono::steady_clock::now(),
                                  std::u16string(committed_tail), accessibility_sequence};
}

void Engine::withdraw_recent_commit(ContextId context) {
    if (!recent_commit_) return;
    const auto pending = *recent_commit_;
    recent_commit_.reset();
    if (pending.context != context) return;
    if (std::chrono::steady_clock::now() - pending.recorded_at > correction_window_) return;
    // A composition that is not empty takes this Backspace itself; only an
    // empty buffer means the host is deleting the committed text.
    const auto* session = find(context);
    if (session == nullptr || !session->buffer.empty()) return;
    // Only withdraw if the application's text is still exactly what the IME
    // committed. A same-context edit by another path must leave the record.
    const auto surrounding = host_.surrounding_text(context);
    if (surrounding.valid && !surrounding.text.empty()) {
        if (surrounding.cursor != surrounding.anchor || surrounding.cursor > surrounding.text.size() ||
            utf16_tail(std::u16string_view(surrounding.text).substr(0, surrounding.cursor), 256) !=
                pending.committed_tail) return;
    } else {
        if (surrounding.valid && (surrounding.cursor != 0 || surrounding.anchor != 0)) return;
        // Some Linux clients provide no usable surrounding text. Only trust a
        // fresh AT-SPI sample published after the commit, never a stale one.
        const auto sample = accessibility_context_ ? accessibility_context_->latest() : std::nullopt;
        if (!sample || !sample->usable || sample->sequence <= pending.accessibility_sequence ||
            utf16_tail(sample->text, 256) != pending.committed_tail) return;
    }
    if (on_training_discard_) on_training_discard_(pending.event_id);
    else transport_.discard_commit(pending.event_id);
}

bool Engine::should_probe_memory(ContextId context, const InputSession& session) const {
    if (!memory_context_ || client_reports_text(host_, context)) return false;
    if (accessibility_context_) {
        const auto sample = accessibility_context_->latest();
        if (sample) {
            const auto context_text = adopt_context_sample(session, *sample,
                                                           accessibility_base_sequence_,
                                                           accessibility_composition_base_);
            if (context_text && !context_text->empty()) return false;
        }
    }
    return true;
}

void Engine::request_prediction(ContextId context, InputSession& session) {
    processor_.apply_phrase_override(session);
    // A naturally changing composition is the probe signal. The worker keeps
    // only the newest pending state when typing outruns the memory scan.
    if (should_probe_memory(context, session)) {
        memory_context_->refresh();
    }
    resync_context(context, session);
    if (!session.prediction.begin(session.buffer.completed_segment_indices(), session.buffer.raw_composition(),
                                  session.buffer.revision())) {
        return;
    }
    const auto generation = session.prediction.generation;
    if (!session.prediction.session_open()) {
        open_prediction_session(context, generation);
    } else {
        send_prediction(context, session, generation);
    }
}

void Engine::open_prediction_session(ContextId context, std::uint64_t generation) {
    const std::weak_ptr<bool> alive = alive_;
    transport_.open_session([this, alive, context, generation](protocol::Message response) mutable {
        if (alive.expired()) return;
        host_.post([this, alive, context, generation, response = std::move(response)]() mutable {
            if (alive.expired()) return;
            auto* session = find(context);
            if (session == nullptr) return;
            if (session->prediction.generation != generation || !session->prediction.pending ||
                session->prediction.session_open()) {
                return;
            }
            if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&response)) {
                session->prediction.session_id = opened->session_id;
                send_prediction(context, *session, generation);
                return;
            }
            // The backend never opened a session: every requested segment
            // keeps its fallback candidates.
            const auto fallback_indices = session->prediction.segment_indices;
            const bool dirty = session->prediction.finish();
            for (const auto index : fallback_indices) {
                processor_.apply_fallback_candidates(*session, index);
            }
            processor_.apply_phrase_override(*session);
            if (dirty) request_prediction(context, *session);
            host_.update_ui(context);
        });
    });
}

void Engine::send_prediction(ContextId context, InputSession& session, std::uint64_t generation) {
    if (session.prediction.generation != generation || !session.prediction.pending ||
        !session.prediction.session_open()) {
        return;
    }
    resync_context(context, session);
    auto request = build_predict_request(context, session);
    request.request_id = session.prediction.next_request_id++;
    request.buffer_revision = session.prediction.revision;
    session.prediction.inflight_request_id = request.request_id;
    session.prediction.inflight_revision = request.buffer_revision;
    const std::weak_ptr<bool> alive = alive_;
    transport_.predict(
        request.session_id, request.request_id, request.buffer_revision, std::move(request.context),
        std::move(request.padding), [this, alive, context, generation](protocol::Message response) mutable {
            if (alive.expired()) return;
            host_.post([this, alive, context, generation, response = std::move(response)]() mutable {
                if (alive.expired()) return;
                handle_prediction_response(context, generation, std::move(response));
            });
        });
}

void Engine::handle_prediction_response(ContextId context, std::uint64_t generation,
                                        protocol::Message response) {
    auto* session = find(context);
    if (session == nullptr) return;
    if (session->prediction.generation != generation || !session->prediction.pending) return;

    bool accepted = false;
    if (const auto* prediction = std::get_if<protocol::Prediction>(&response)) {
        accepted = session->prediction.correlates(*prediction);
        if (accepted && session->prediction.matches_composition(
                            *prediction, session->buffer.raw_composition(), session->buffer.revision())) {
            processor_.apply_prediction(*session, *prediction);
        } else if (accepted) {
            for (const auto index : session->prediction.segment_indices) {
                processor_.apply_fallback_candidates(*session, index);
            }
        }
    } else if (const auto* error = std::get_if<protocol::Error>(&response)) {
        accepted = session->prediction.correlates(*error);
        if (accepted) {
            if (error->code == protocol::ErrorCode::UnknownSession) {
                session->prediction.session_id = {};
            }
            for (const auto index : session->prediction.segment_indices) {
                processor_.apply_fallback_candidates(*session, index);
            }
        }
    }
    if (!accepted) return;

    processor_.apply_phrase_override(*session);

    const bool dirty = session->prediction.finish();
    if (dirty) request_prediction(context, *session);
    host_.update_ui(context);
}

void Engine::close_prediction_session(InputSession& session) {
    if (!session.prediction.session_open()) return;
    transport_.close_session(session.prediction.session_id, {});
    session.prediction.session_id = {};
}

void Engine::resync_context(ContextId context, InputSession& session) {
    // Context is read fresh from the current source for every prediction; it
    // is never accumulated or reused across requests.
    session.context_text.clear();
    session.context_source = ContextSource::None;
    if (host_.is_sensitive(context)) return;

    const std::size_t limit = config_.context_length > 0 ? static_cast<std::size_t>(config_.context_length) : 0;

    // Some clients report an empty (but valid) document, notably Electron,
    // Chromium and terminals. An empty client prefix must not shadow the
    // accessibility sample, which may still hold the focused widget's text.
    bool client_empty = false;
    const auto surrounding = host_.surrounding_text(context);
    if (surrounding.valid) {
        const std::size_t cursor = std::min(surrounding.cursor, surrounding.anchor);
        const std::size_t bounded = std::min(cursor, surrounding.text.size());
        auto text = utf16_tail(std::u16string_view(surrounding.text).substr(0, bounded), limit);
        if (!text.empty()) {
            session.context_text = std::move(text);
            session.context_source = ContextSource::Client;
            log_context("client-surrounding", session.context_text);
            return;
        }
        if (!surrounding.text.empty()) {
            log_context("client-surrounding-empty-prefix", {});
            return;
        }
        client_empty = true;
    }

    if (accessibility_context_) {
        const auto sample = accessibility_context_->latest();
        const auto text = sample ? adopt_context_sample(session, *sample, accessibility_base_sequence_,
                                                        accessibility_composition_base_)
                                 : std::nullopt;
        if (text) {
            session.context_text = utf16_tail(*text, limit);
            session.context_source = ContextSource::Accessibility;
            log_context("accessibility", session.context_text);
            return;
        }
        if (sample && sample->usable && sample->sequence > accessibility_base_sequence_) {
            log_context("accessibility-preedit-mismatch", sample->text);
        } else {
            log_context("accessibility-unusable", {});
        }
    }

    // Last resort: the probe sample comes from the application's memory and is
    // only used when no other source produced context.
    if (memory_context_) {
        const auto sample = memory_context_->latest();
        const auto text = sample ? adopt_context_sample(session, *sample, memory_base_sequence_,
                                                        memory_composition_base_,
                                                        /*strip_preedit=*/false)
                                 : std::nullopt;
        if (text && !text->empty()) {
            session.context_text = utf16_tail(*text, limit);
            session.context_source = ContextSource::Memory;
            log_context("memory-probe", session.context_text);
            return;
        }
        log_context("memory-probe-unusable", {});
    }

    if (client_empty) {
        log_context("client-surrounding-empty", {});
        return;
    }
    log_context("context-fallback", {});
}

std::optional<std::u16string> Engine::adopt_context_sample(const InputSession& session,
                                                            const AccessibilityContextSample& sample,
                                                            std::uint64_t base_sequence,
                                                            std::uint64_t composition_base,
                                                            bool strip_preedit) const {
    if (!sample.usable || sample.sequence <= base_sequence) return std::nullopt;
    if (!strip_preedit) return sample.text;
    // A sample published before this composition started cannot contain its
    // preedit; anything newer may, so it is only adopted after the composing
    // text is stripped from its tail.
    const bool predates_composition = composition_base != 0 && sample.sequence <= composition_base;
    const bool may_contain_preedit =
        !InputProcessor::composition_empty(session) && !predates_composition;
    if (!may_contain_preedit) return sample.text;
    return strip_accessibility_preedit(session, sample.text);
}

std::optional<std::u16string> Engine::strip_accessibility_preedit(const InputSession& session,
                                                                  const std::u16string& sample) const {
    std::vector<std::pair<std::u16string, std::u16string>> storage;
    for (const auto& segment : session.buffer.segments()) {
        if (segment.empty()) continue;
        storage.emplace_back(segment.rendered_text(), segment.reading());
    }
    if (!session.pending_token.empty()) {
        storage.emplace_back(InputProcessor::pending_rendered_text(session), session.pending_token.raw);
    }

    std::vector<PreeditSegmentState> states;
    states.reserve(storage.size());
    for (const auto& [rendered, reading] : storage) states.push_back({rendered, reading});
    return strip_preedit_suffix(sample, states);
}

protocol::PredictRequest Engine::build_predict_request(ContextId context, const InputSession& session) const {
    protocol::PredictRequest request;
    request.session_id = session.prediction.session_id;
    request.buffer_revision = session.buffer.revision();
    for (const auto& segment : session.buffer.segments()) {
        if (!segment.complete()) continue;

        protocol::PaddingEntry entry;
        entry.bopomofo = segment.reading();
        if (segment.manually_chosen && segment.selected_candidate() != 0) {
            entry.chosen = true;
            entry.chosen_char = segment.selected_candidate();
        }
        request.padding.push_back(std::move(entry));
    }

    if (host_.is_sensitive(context)) return request;

    const std::size_t reserved_tokens = 2 + request.padding.size() * 2;
    const std::size_t context_limit = config_.context_length > static_cast<int>(reserved_tokens)
                                          ? static_cast<std::size_t>(config_.context_length) - reserved_tokens
                                          : 0;
    request.context = utf16_tail(session.context_text, context_limit);
    log_context("model", request.context);
    return request;
}

void Engine::apply_context_sources() {
    const std::size_t limit = static_cast<std::size_t>(std::max(1, config_.context_length));
    if (!options_.enable_accessibility) {
        if (accessibility_context_) {
            accessibility_context_->stop();
            accessibility_context_.reset();
        }
        accessibility_max_code_units_ = 0;
        accessibility_base_sequence_ = 0;
    } else if (!accessibility_context_) {
        accessibility_context_ = create_accessibility_context_provider(limit);
        accessibility_base_sequence_ = 0;
        accessibility_max_code_units_ = limit;
        (void)accessibility_context_->start();
    } else {
        if (accessibility_max_code_units_ != limit) {
            // A configuration change must not rebuild the backend: tearing down
            // and re-initialising libatspi in one process crashes the input
            // method, so only the sampling bound is updated.
            accessibility_context_->set_max_code_units(limit);
            accessibility_max_code_units_ = limit;
        }
        (void)accessibility_context_->start();
    }

    // Memory probe: created only when the host supports it and the setting is
    // on; unlike AT-SPI it can be torn down and recreated freely.
    const bool memory_enabled = options_.enable_memory_context && config_.memory_context;
    if (!memory_enabled) {
        if (memory_context_) {
            memory_context_->stop();
            memory_context_.reset();
        }
        memory_base_sequence_ = 0;
    } else if (!memory_context_) {
        MemoryProbeCallbacks callbacks;
        callbacks.preedit = [this] {
            if (memory_probe_context_ == 0) return std::string{};
            const auto* session = find(memory_probe_context_);
            if (session == nullptr) return std::string{};
            const auto preedit = InputProcessor::current_preedit(*session);
            if (preedit.empty()) return std::string{};
            try {
                return u16_to_utf8(preedit);
            } catch (const std::exception&) {
                return std::string{};
            }
        };
        callbacks.processes = [this] {
            if (memory_probe_context_ == 0) return std::vector<int>{};
            return host_.probe_processes(memory_probe_context_);
        };
        callbacks.sensitive = [this] {
            if (memory_probe_context_ == 0) return true;
            return host_.is_sensitive(memory_probe_context_);
        };
        memory_context_ =
            std::make_unique<MemoryContextProvider>(limit, std::move(callbacks), memory_helper_path());
        memory_base_sequence_ = 0;
        (void)memory_context_->start();
    } else {
        memory_context_->set_max_code_units(limit);
        (void)memory_context_->start();
    }
}

}  // namespace llavon::ime

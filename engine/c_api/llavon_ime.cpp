#include "llavon_ime/llavon_ime.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "config/config_schema.hpp"
#include "host/engine.hpp"
#include "host/host.hpp"
#include "host/render_state.hpp"
#include "input/input_key.hpp"

namespace {

using namespace llavon::ime;

void copy_utf16(const std::u16string& source, uint16_t* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0) return;
    const size_t count = std::min(capacity, source.size());
    std::memcpy(buffer, source.data(), count * sizeof(uint16_t));
    if (count < capacity) buffer[count] = 0;
}

void copy_utf8(const std::string& source, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0) return;
    const size_t count = std::min(capacity, source.size());
    std::memcpy(buffer, source.data(), count);
    if (count < capacity) buffer[count] = '\0';
}

}  // namespace

struct lv_engine;

namespace {

void refresh_render_info(lv_engine* engine);

class HostBridge final : public Host {
public:
    HostBridge(lv_engine* owner, const lv_host& callbacks);
    ~HostBridge() override = default;

    void post(std::function<void()> body) override;
    void commit(ContextId context, std::u16string_view text) override;
    void update_ui(ContextId context) override;
    HostContext surrounding_text(ContextId context) override;
    bool is_sensitive(ContextId context) override;

private:
    lv_engine* owner_;
    lv_host callbacks_;
};

extern "C" {
static void lv_engine_run_posted_body(void* body) {
    const std::unique_ptr<std::function<void()>> function(static_cast<std::function<void()>*>(body));
    (*function)();
}
}

}  // namespace

struct lv_engine {
    explicit lv_engine(const lv_host& host_callbacks) : bridge(this, host_callbacks) {}

    lv_host callbacks = {};
    HostBridge bridge;
    std::unique_ptr<Engine> engine;
    RenderState render;
    lv_render_info info = {};
    std::string config_json;
};

namespace {

HostBridge::HostBridge(lv_engine* owner, const lv_host& callbacks)
    : owner_(owner), callbacks_(callbacks) {}

void HostBridge::post(std::function<void()> body) {
    auto* function = new std::function<void()>(std::move(body));
    if (callbacks_.post != nullptr) {
        callbacks_.post(callbacks_.user, &lv_engine_run_posted_body, function);
    } else {
        lv_engine_run_posted_body(function);
    }
}

void HostBridge::commit(ContextId context, std::u16string_view text) {
    if (callbacks_.commit == nullptr) return;
    callbacks_.commit(callbacks_.user, context, reinterpret_cast<const uint16_t*>(text.data()), text.size());
}

void HostBridge::update_ui(ContextId context) {
    owner_->render = owner_->engine->render_state(context);
    refresh_render_info(owner_);
    if (callbacks_.update_ui != nullptr) callbacks_.update_ui(callbacks_.user, context);
}

HostContext HostBridge::surrounding_text(ContextId context) {
    HostContext result;
    if (callbacks_.surrounding_text == nullptr) return result;

    // The caller copies at most `capacity` units and reports the full length;
    // grow the buffer and retry when the text did not fit.
    std::vector<uint16_t> buffer(4096);
    for (int attempt = 0; attempt < 3; ++attempt) {
        lv_surrounding_text out = {};
        out.capacity = buffer.size();
        out.text = buffer.data();
        if (!callbacks_.surrounding_text(callbacks_.user, context, &out)) return result;
        if (out.length > out.capacity) {
            buffer.resize(out.length);
            continue;
        }
        if (out.valid == 0) return result;
        result.valid = true;
        result.text.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(out.length));
        result.cursor = out.cursor;
        result.anchor = out.anchor;
        return result;
    }
    return result;
}

bool HostBridge::is_sensitive(ContextId context) {
    if (callbacks_.is_sensitive == nullptr) return false;
    return callbacks_.is_sensitive(callbacks_.user, context) != 0;
}

void refresh_render_info(lv_engine* engine) {
    if (engine == nullptr) return;
    const auto& state = engine->render;
    auto& info = engine->info;
    info = lv_render_info{};
    info.composition_empty = state.composition_empty ? 1 : 0;
    info.has_candidates = state.has_candidates ? 1 : 0;
    info.cursor_visible = state.cursor_visible ? 1 : 0;
    info.target = static_cast<int32_t>(state.candidate_target);
    info.symbol_epoch = state.symbol_epoch;
    info.page = state.page;
    info.page_size = state.page_size;
    info.page_count = state.page_count;
    info.cursor = state.cursor;
    info.caret = state.caret;
    size_t preedit_length = 0;
    for (const auto& segment : state.preedit) preedit_length += segment.text.size();
    info.preedit_length = preedit_length;
    info.preedit_segment_count = state.preedit.size();
    info.aux_up_length = state.aux_up.size();
    info.aux_down_length = state.aux_down.size();
    info.candidate_count = state.candidates.size();
    info.selection_key_count = state.selection_keys.size();
}

}  // namespace

extern "C" {

void lv_engine_options_init(lv_engine_options* options) {
    if (options == nullptr) return;
    std::memset(options, 0, sizeof(*options));
    // The numeric fields stay at their "use the effective config" sentinels so
    // a host that does not care about runtime tuning gets exactly the values
    // the fcitx5 addon derives from load_config(); see lv_engine_create.
    options->context_length = 0;
    options->threads = 0;
    options->gpu_layers = std::numeric_limits<std::int32_t>::min();
    options->idle_timeout_seconds = 0;
    options->auto_start_service = 1;
    options->enable_accessibility = 1;
}

int lv_engine_create(const lv_engine_options* options, const lv_host* host, lv_engine** out) {
    if (options == nullptr || host == nullptr || out == nullptr) return -1;
    if (options->table_path == nullptr) return -1;
    try {
        auto engine = std::make_unique<lv_engine>(*host);
        EngineOptions engine_options;
        engine_options.table_path = options->table_path;
        // Hosts that do not pass a path keep the engine's own location, so the
        // native macOS frontend reads the same file as the fcitx5 addon did.
        engine_options.phrase_overrides_path =
            options->phrase_overrides_path != nullptr
                ? std::filesystem::path(options->phrase_overrides_path)
                : phrase_overrides_path();
        engine_options.enable_accessibility = options->enable_accessibility != 0;
        if (options->config_json != nullptr && options->config_json[0] != '\0') {
            engine_options.config = config_from_json(nlohmann::json::parse(options->config_json));
        } else {
            // No inline config: read the on-disk config (fcitx5 conf or the
            // legacy JSON file) so existing user settings survive.
            engine_options.config = load_config();
        }

        ServiceTransportOptions transport;
        if (options->socket_path != nullptr) transport.socket_path = options->socket_path;
        if (options->service_path != nullptr) transport.service_path = options->service_path;
        if (options->model_path != nullptr) {
            transport.model_path = options->model_path;
        } else if (!engine_options.config.model_path.empty()) {
            // No host-provided path: use the configured one, like the fcitx5
            // addon does, so a path saved in the settings takes effect.
            transport.model_path = engine_options.config.model_path;
        }
        if (options->tables_dir != nullptr) transport.tables_dir = options->tables_dir;
        // Unset numeric options come from the effective config, so the service
        // runs with the same values the engine itself uses. The fcitx5 addon
        // derives its transport from load_config() the same way.
        const auto configured = [](int value) {
            return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
        };
        transport.context_length = options->context_length != 0
                                      ? options->context_length
                                      : configured(engine_options.config.context_length);
        transport.threads = options->threads != 0 ? options->threads
                                                  : configured(engine_options.config.thread_count);
        transport.gpu_layers = options->gpu_layers != LV_GPU_LAYERS_FROM_CONFIG
                                   ? options->gpu_layers
                                   : engine_options.config.gpu_layers;
        transport.idle_timeout_seconds = options->idle_timeout_seconds != 0
                                             ? options->idle_timeout_seconds
                                             : configured(engine_options.config.idle_timeout_seconds);
        transport.auto_start = options->auto_start_service != 0;
        engine_options.transport = transport;

        engine->engine = std::make_unique<Engine>(std::move(engine_options), engine->bridge);
        *out = engine.release();
        return 0;
    } catch (...) {
        return -1;
    }
}

void lv_engine_destroy(lv_engine* engine) {
    delete engine;
}

void lv_engine_attach(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->attach(context);
}

void lv_engine_detach(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->detach(context);
}

int lv_engine_key_event(lv_engine* engine, lv_context_id context, const lv_key* key) {
    if (engine == nullptr || engine->engine == nullptr || key == nullptr) return 0;
    InputKey input_key;
    input_key.sym = static_cast<char32_t>(key->sym);
    input_key.states = key->states;
    input_key.frontend_states = key->frontend_states;
    input_key.raw_states = key->raw_states;
    input_key.caps_lock = key->caps_lock != 0;
    input_key.release = key->release != 0;
    return engine->engine->key_event(context, input_key) ? 1 : 0;
}

void lv_engine_select_candidate(lv_engine* engine, lv_context_id context, int32_t index) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->select_candidate(context, index);
}

void lv_engine_select_symbol(lv_engine* engine, lv_context_id context, int32_t index, uint64_t epoch) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->select_symbol(context, index, epoch);
}

void lv_engine_activate(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->activate(context);
}

void lv_engine_deactivate(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->deactivate(context);
}

void lv_engine_reset(lv_engine* engine, lv_context_id context, int32_t reason, int clear_context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    InputResetReason reset_reason = InputResetReason::Explicit;
    switch (reason) {
        case LV_RESET_FOCUS_OUT:
            reset_reason = InputResetReason::FocusOut;
            break;
        case LV_RESET_DEACTIVATE:
            reset_reason = InputResetReason::Deactivate;
            break;
        case LV_RESET_EXPLICIT:
        default:
            reset_reason = InputResetReason::Explicit;
            break;
    }
    engine->engine->reset(context, reset_reason, clear_context != 0);
}

void lv_engine_clear_context_text(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->clear_context_text(context);
}

const lv_render_info* lv_engine_render(lv_engine* engine, lv_context_id context) {
    if (engine == nullptr || engine->engine == nullptr) return nullptr;
    engine->render = engine->engine->render_state(context);
    refresh_render_info(engine);
    return &engine->info;
}

size_t lv_engine_preedit(lv_engine* engine, uint16_t* buffer, size_t capacity) {
    if (engine == nullptr) return 0;
    std::u16string text;
    for (const auto& segment : engine->render.preedit) text += segment.text;
    copy_utf16(text, buffer, capacity);
    return text.size();
}

size_t lv_engine_preedit_segment(lv_engine* engine, size_t index, uint16_t* buffer, size_t capacity,
                                 int* underlined) {
    if (engine == nullptr || index >= engine->render.preedit.size()) return 0;
    const auto& segment = engine->render.preedit[index];
    if (underlined != nullptr) *underlined = segment.underlined ? 1 : 0;
    copy_utf16(segment.text, buffer, capacity);
    return segment.text.size();
}

size_t lv_engine_aux_up(lv_engine* engine, uint16_t* buffer, size_t capacity) {
    if (engine == nullptr) return 0;
    copy_utf16(engine->render.aux_up, buffer, capacity);
    return engine->render.aux_up.size();
}

size_t lv_engine_aux_down(lv_engine* engine, uint16_t* buffer, size_t capacity) {
    if (engine == nullptr) return 0;
    copy_utf16(engine->render.aux_down, buffer, capacity);
    return engine->render.aux_down.size();
}

size_t lv_engine_candidate(lv_engine* engine, size_t index, uint16_t* buffer, size_t capacity) {
    if (engine == nullptr || index >= engine->render.candidates.size()) return 0;
    const auto& candidate = engine->render.candidates[index];
    copy_utf16(candidate, buffer, capacity);
    return candidate.size();
}

size_t lv_engine_selection_keys(lv_engine* engine, uint32_t* buffer, size_t capacity) {
    if (engine == nullptr) return 0;
    const auto& keys = engine->render.selection_keys;
    if (buffer != nullptr && capacity > 0) {
        const size_t count = std::min(capacity, keys.size());
        for (size_t i = 0; i < count; ++i) buffer[i] = static_cast<uint32_t>(keys[i]);
    }
    return keys.size();
}

int32_t lv_engine_layout_hint(lv_engine* engine) {
    if (engine == nullptr) return 0;
    if (engine->render.layout_hint == "vertical") return 1;
    if (engine->render.layout_hint == "horizontal") return 2;
    return 0;
}

int lv_engine_set_config_json(lv_engine* engine, const char* json, size_t length) {
    if (engine == nullptr || engine->engine == nullptr || json == nullptr) return -1;
    try {
        const auto parsed = nlohmann::json::parse(json, json + length);
        engine->engine->set_config(config_from_json(parsed));
        return 0;
    } catch (...) {
        return -1;
    }
}

int lv_engine_reload_config_json(lv_engine* engine, const char* json, size_t length) {
    if (engine == nullptr || engine->engine == nullptr || json == nullptr) return -1;
    try {
        const auto parsed = nlohmann::json::parse(json, json + length);
        // A plain reload from disk: refresh the config without settling the
        // sessions, mirroring the fcitx5 addon's reload_config().
        engine->engine->set_config(config_from_json(parsed), false);
        return 0;
    } catch (...) {
        return -1;
    }
}

size_t lv_engine_config_json(lv_engine* engine, char* buffer, size_t capacity) {
    if (engine == nullptr || engine->engine == nullptr) return 0;
    engine->config_json = to_json(engine->engine->config()).dump();
    copy_utf8(engine->config_json, buffer, capacity);
    return engine->config_json.size();
}

void lv_engine_reload_phrase_overrides(lv_engine* engine) {
    if (engine == nullptr || engine->engine == nullptr) return;
    engine->engine->reload_phrase_overrides();
}

size_t lv_config_schema_json(char* buffer, size_t capacity) {
    static const std::string schema = config_schema_json().dump();
    copy_utf8(schema, buffer, capacity);
    return schema.size();
}

int32_t lv_engine_accessibility_availability(lv_engine* engine) {
    if (engine == nullptr || engine->engine == nullptr) return LV_AVAILABILITY_UNSUPPORTED;
    const auto state = engine->engine->accessibility_state();
    return static_cast<int32_t>(state.availability);
}

size_t lv_engine_accessibility_detail(lv_engine* engine, char* buffer, size_t capacity) {
    if (engine == nullptr || engine->engine == nullptr) return 0;
    const auto state = engine->engine->accessibility_state();
    copy_utf8(state.detail, buffer, capacity);
    return state.detail.size();
}

}  // extern "C"

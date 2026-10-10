#ifndef LLAVON_IME_H
#define LLAVON_IME_H

/*
 * C interface to the host-agnostic llavon-ime engine, for hosts that cannot
 * compile C++ directly (the macOS InputMethodKit frontend in Swift).
 *
 * Threading: every function must be called on the host's main thread except
 * the lv_host.post trampoline, which the engine calls from background threads
 * and which must marshal back to the main thread.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lv_engine lv_engine;

/* Host-assigned handle for one input context. */
typedef uint64_t lv_context_id;

/* One key event; see InputKey in the engine for the field semantics. */
typedef struct {
    uint32_t sym;
    uint32_t states;
    uint32_t frontend_states;
    uint32_t raw_states;
    uint8_t caps_lock;
    uint8_t release;
} lv_key;

typedef enum {
    LV_TARGET_NONE = 0,
    LV_TARGET_CANDIDATES = 1,
    LV_TARGET_SYMBOL_MENU = 2,
    LV_TARGET_MARKING_HINT = 3,
} lv_render_target;

typedef enum {
    LV_RESET_EXPLICIT = 0,
    LV_RESET_FOCUS_OUT = 1,
    LV_RESET_DEACTIVATE = 2,
} lv_reset_reason;

typedef enum {
    LV_AVAILABILITY_UNSUPPORTED = 0,
    LV_AVAILABILITY_DISABLED = 1,
    LV_AVAILABILITY_UNAVAILABLE = 2,
    LV_AVAILABILITY_AVAILABLE = 3,
} lv_accessibility_availability;

/* Client text around the caret. Offsets are UTF-16 code units. */
typedef struct {
    size_t capacity; /* in: size of `text` in UTF-16 code units */
    size_t length;   /* out: full length, even when it exceeds capacity */
    size_t cursor;   /* out */
    size_t anchor;   /* out */
    uint8_t valid;   /* out */
    uint16_t* text;  /* caller-allocated buffer */
} lv_surrounding_text;

typedef struct {
    void* user;

    /* Marshals `body(body_user)` onto the host's main thread. May be called
     * from any thread. */
    void (*post)(void* user, void (*body)(void*), void* body_user);

    /* Commits UTF-16 text to the application. Main thread only. */
    void (*commit)(void* user, lv_context_id context, const uint16_t* text, size_t length);

    /* Asks the host to redraw; read the state with lv_engine_render. */
    void (*update_ui)(void* user, lv_context_id context);

    /* Fills `out` with the surrounding text. Returns non-zero when valid. */
    int (*surrounding_text)(void* user, lv_context_id context, lv_surrounding_text* out);

    /* Returns non-zero for password/sensitive contexts. */
    int (*is_sensitive)(void* user, lv_context_id context);
} lv_host;

typedef struct {
    const char* table_path;            /* required: bopomofo_char.json */
    const char* phrase_overrides_path; /* required */
    const char* socket_path;           /* optional: unix service socket */
    const char* service_path;          /* optional: service executable */
    const char* model_path;            /* optional */
    const char* tables_dir;            /* optional */
    uint32_t context_length;           /* 0 = use the config value */
    uint32_t threads;                  /* 0 = use the config value */
    int32_t gpu_layers;                /* LV_GPU_LAYERS_FROM_CONFIG or -2 (auto) */
    uint32_t idle_timeout_seconds;     /* 0 = use the config value */
    uint8_t auto_start_service;
    uint8_t enable_accessibility;
    const char* config_json; /* optional; see lv_engine_config_json */
} lv_engine_options;

/* `gpu_layers` sentinel: run the service with the configured value. */
#define LV_GPU_LAYERS_FROM_CONFIG INT32_MIN

/* Fills `options` with the library defaults. Call before overriding fields.
 * Unset numeric fields follow the effective config, like the fcitx5 addon. */
void lv_engine_options_init(lv_engine_options* options);

/* Creates an engine. Returns 0 on success. `host` is copied; it must stay
 * valid for the lifetime of the engine (its `user` pointer is passed back). */
int lv_engine_create(const lv_engine_options* options, const lv_host* host, lv_engine** out);
void lv_engine_destroy(lv_engine* engine);

void lv_engine_attach(lv_engine* engine, lv_context_id context);
void lv_engine_detach(lv_engine* engine, lv_context_id context);

/* Returns 1 when the host must consume the key, 0 to leave it to the app. */
int lv_engine_key_event(lv_engine* engine, lv_context_id context, const lv_key* key);
void lv_engine_select_candidate(lv_engine* engine, lv_context_id context, int32_t index);
void lv_engine_select_symbol(lv_engine* engine, lv_context_id context, int32_t index, uint64_t epoch);
void lv_engine_activate(lv_engine* engine, lv_context_id context);
void lv_engine_deactivate(lv_engine* engine, lv_context_id context);
void lv_engine_reset(lv_engine* engine, lv_context_id context, int32_t reason, int clear_context);
void lv_engine_clear_context_text(lv_engine* engine, lv_context_id context);

typedef struct {
    uint8_t composition_empty;
    uint8_t has_candidates;
    uint8_t cursor_visible;
    int32_t target; /* lv_render_target */
    uint64_t symbol_epoch;
    int32_t page;
    int32_t page_size;
    int32_t page_count;
    int32_t cursor;
    size_t caret;
    size_t preedit_length;
    size_t preedit_segment_count;
    size_t aux_up_length;
    size_t aux_down_length;
    size_t candidate_count;
    size_t selection_key_count;
} lv_render_info;

/*
 * Builds the render snapshot for `context` and returns it. The text accessors
 * below read the same snapshot, so call this before reading them. The host's
 * update_ui callback already refreshes the snapshot for its context.
 */
const lv_render_info* lv_engine_render(lv_engine* engine, lv_context_id context);

/* Text accessors. Each returns the full UTF-16 length of the value and copies
 * at most `capacity` units into `buffer` (buffer may be null to only measure).
 * The copy is null-terminated when `capacity` exceeds the returned length.
 * The preedit is the concatenation of its segments; use the segment accessor
 * for per-segment underlines. `underlined` may be null. */
size_t lv_engine_preedit(lv_engine* engine, uint16_t* buffer, size_t capacity);
size_t lv_engine_preedit_segment(lv_engine* engine, size_t index, uint16_t* buffer, size_t capacity,
                                 int* underlined);
size_t lv_engine_aux_up(lv_engine* engine, uint16_t* buffer, size_t capacity);
size_t lv_engine_aux_down(lv_engine* engine, uint16_t* buffer, size_t capacity);
size_t lv_engine_candidate(lv_engine* engine, size_t index, uint16_t* buffer, size_t capacity);
size_t lv_engine_selection_keys(lv_engine* engine, uint32_t* buffer, size_t capacity);
/* 0 = system default, 1 = vertical, 2 = horizontal. */
int32_t lv_engine_layout_hint(lv_engine* engine);

/* Settings. `lv_engine_config_json` returns the full UTF-8 length and copies
 * at most `capacity` bytes (null-terminated when capacity leaves room). `set`
 * returns 0 on success. */
int lv_engine_set_config_json(lv_engine* engine, const char* json, size_t length);
size_t lv_engine_config_json(lv_engine* engine, char* buffer, size_t capacity);

/* Applies config JSON without settling sessions: a plain reload from disk,
 * mirroring the fcitx5 addon's reload_config(). Returns 0 on success. */
int lv_engine_reload_config_json(lv_engine* engine, const char* json, size_t length);

/* Re-reads the phrase overrides file without restarting the engine. */
void lv_engine_reload_phrase_overrides(lv_engine* engine);

/* Restart only prediction transport; composition and frontend contexts survive.
 * Uses current config, preserving a model_path explicitly supplied at creation. */
int lv_engine_restart_prediction_service(lv_engine* engine);

/* Schema of every configurable field (key, label, group, kind, bounds, default
   and choices), for host settings UIs. Returns the JSON length; copies into
   buffer when it is non-NULL, NUL-terminated and truncated to capacity. */
size_t lv_config_schema_json(char* buffer, size_t capacity);

int32_t lv_engine_accessibility_availability(lv_engine* engine);
size_t lv_engine_accessibility_detail(lv_engine* engine, char* buffer, size_t capacity);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LLAVON_IME_H */

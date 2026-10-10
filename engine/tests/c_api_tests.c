#include "test_suites.h"

/* Exercises the C ABI exactly as a C or Swift host would. */
#ifndef _WIN32

#include "llavon_ime/llavon_ime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_posted = 0;
static int g_ui_count = 0;
static int g_commit_count = 0;
static uint16_t g_commit_text[256];
static size_t g_commit_length = 0;

/* Path the host resolves outside the config; the schema reports it as a hint. */
static const char* lv_test_model_path = "/tmp/llavon-ime-test-model.gguf";

/* Posted jobs are marshalled back to this thread by the test. */
static void (*g_jobs[16])(void*);
static void* g_job_users[16];
static int g_job_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_condition = PTHREAD_COND_INITIALIZER;

static void host_post(void* user, void (*body)(void*), void* body_user) {
    (void)user;
    pthread_mutex_lock(&g_mutex);
    if (g_job_count < 16) {
        g_jobs[g_job_count] = body;
        g_job_users[g_job_count] = body_user;
        ++g_job_count;
    }
    ++g_posted;
    pthread_cond_broadcast(&g_condition);
    pthread_mutex_unlock(&g_mutex);
}

/* Runs queued jobs on this thread until the queue stays empty for `idle_ms`. */
static void pump_until_idle(int idle_ms) {
    struct timespec idle_deadline;
    clock_gettime(CLOCK_REALTIME, &idle_deadline);
    idle_deadline.tv_sec += idle_ms / 1000;
    idle_deadline.tv_nsec += (long)(idle_ms % 1000) * 1000000L;
    if (idle_deadline.tv_nsec >= 1000000000L) {
        ++idle_deadline.tv_sec;
        idle_deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        void (*job)(void*) = NULL;
        void* job_user = NULL;
        pthread_mutex_lock(&g_mutex);
        while (g_job_count == 0) {
            if (pthread_cond_timedwait(&g_condition, &g_mutex, &idle_deadline) != 0) break;
        }
        if (g_job_count > 0) {
            job = g_jobs[0];
            job_user = g_job_users[0];
            memmove(g_jobs, g_jobs + 1, sizeof(g_jobs[0]) * (size_t)(g_job_count - 1));
            memmove(g_job_users, g_job_users + 1, sizeof(g_job_users[0]) * (size_t)(g_job_count - 1));
            --g_job_count;
        }
        pthread_mutex_unlock(&g_mutex);
        if (job == NULL) return;
        job(job_user);
    }
}

static void host_commit(void* user, lv_context_id context, const uint16_t* text, size_t length) {
    (void)user;
    (void)context;
    ++g_commit_count;
    g_commit_length = length < 256 ? length : 256;
    memcpy(g_commit_text, text, g_commit_length * sizeof(uint16_t));
}

static void host_update_ui(void* user, lv_context_id context) {
    (void)user;
    (void)context;
    ++g_ui_count;
}

static int host_surrounding(void* user, lv_context_id context, lv_surrounding_text* out) {
    (void)user;
    (void)context;
    static const char text[] = "history";
    const size_t length = sizeof(text) - 1;
    out->length = length;
    out->cursor = length;
    out->anchor = length;
    out->valid = 1;
    if (out->text != NULL && out->capacity > 0) {
        const size_t count = length < out->capacity ? length : out->capacity;
        for (size_t i = 0; i < count; ++i) out->text[i] = (uint16_t)text[i];
    }
    return 1;
}

static int host_is_sensitive(void* user, lv_context_id context) {
    (void)user;
    (void)context;
    return 0;
}

static int type_key(lv_engine* engine, lv_context_id context, uint32_t sym) {
    lv_key key;
    memset(&key, 0, sizeof(key));
    key.sym = sym;
    return lv_engine_key_event(engine, context, &key);
}

static int type_key_with_states(lv_engine* engine, lv_context_id context, uint32_t sym,
                                uint32_t states) {
    lv_key key;
    memset(&key, 0, sizeof(key));
    key.sym = sym;
    key.states = states;
    key.frontend_states = states;
    key.raw_states = states;
    return lv_engine_key_event(engine, context, &key);
}

/* The native macOS host reports the unshifted keysym plus the Shift state
   (AppKit's charactersIgnoringModifiers); Shift+letter must behave exactly
   like the folded fcitx5 keysym. */
static int test_shift_letter_from_state(lv_engine* engine, lv_context_id context) {
    const uint32_t shift = 1; /* InputKeyState::Shift */
    const uint32_t ctrl = 4;  /* InputKeyState::Ctrl */

    /* An empty composition leaves the key to the application. */
    lv_engine_reset(engine, context, LV_RESET_EXPLICIT, 1);
    if (type_key_with_states(engine, context, 'a', shift) != 0) return 0;

    /* A composition commits together with the uppercase letter. */
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    g_commit_count = 0;
    g_commit_length = 0;
    if (type_key_with_states(engine, context, 'a', shift) == 0) return 0;
    if (g_commit_count != 1 || g_commit_length != 2) return 0;
    if (g_commit_text[0] != 0x4f60 || g_commit_text[1] != 0x0041) return 0; /* 你A */

    /* Ctrl+Shift+letter and Shift+Tab stay shortcuts. */
    if (type_key_with_states(engine, context, 'a', shift | ctrl) != 0) return 0;
    if (type_key_with_states(engine, context, 0xff09 /* Tab */, shift) != 0) return 0;

    /* Shift+comma from the state shape commits the fullwidth comma. */
    lv_engine_reset(engine, context, LV_RESET_EXPLICIT, 1);
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    g_commit_count = 0;
    g_commit_length = 0;
    if (type_key_with_states(engine, context, ',', shift) == 0) return 0;
    type_key(engine, context, 0xff0d); /* Return */
    if (g_commit_count != 1 || g_commit_length != 2) return 0;
    return g_commit_text[0] == 0x4f60 && g_commit_text[1] == 0xff0c; /* 你， */
}

/* Frontend shape sweep: for every printable key and the keys the engine acts
   on, the fcitx5 shape (shifted keysym, Shift cleared) and the macOS shapes
   (shifted or base keysym plus the Shift state) must drive identical behavior.
   AppKit reports the base keysym for some punctuation, so a frontend that
   clears Shift without folding the keysym turns Shift+/ into the bopomofo key
   ㄥ; this test keeps that class of bug out. */
typedef struct {
    int consumed;
    size_t commit_length;
    uint16_t commit[8];
    size_t preedit_length;
    uint16_t preedit[8];
} ShapeOutcome;

static ShapeOutcome run_shape(lv_engine* engine, lv_context_id context, uint32_t sym, uint32_t states) {
    ShapeOutcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    lv_engine_reset(engine, context, LV_RESET_EXPLICIT, 1);
    g_commit_count = 0;
    g_commit_length = 0;
    lv_key key;
    memset(&key, 0, sizeof(key));
    key.sym = sym;
    key.states = states;
    key.frontend_states = states;
    key.raw_states = states;
    outcome.consumed = lv_engine_key_event(engine, context, &key) != 0;
    outcome.commit_length = g_commit_length < 8 ? g_commit_length : 8;
    memcpy(outcome.commit, g_commit_text, outcome.commit_length * sizeof(uint16_t));
    uint16_t preedit[8];
    const size_t preedit_length = lv_engine_preedit(engine, preedit, 8);
    outcome.preedit_length = preedit_length < 8 ? preedit_length : 8;
    memcpy(outcome.preedit, preedit, outcome.preedit_length * sizeof(uint16_t));
    return outcome;
}

static int same_shape(const ShapeOutcome* a, const ShapeOutcome* b) {
    if (a->consumed != b->consumed || a->commit_length != b->commit_length) return 0;
    if (memcmp(a->commit, b->commit, a->commit_length * sizeof(uint16_t)) != 0) return 0;
    if (a->preedit_length != b->preedit_length) return 0;
    return memcmp(a->preedit, b->preedit, a->preedit_length * sizeof(uint16_t)) == 0;
}

/* US-layout shift map, mirroring the engine's shifted_ascii_symbol(). */
static char shifted_ascii(char base) {
    switch (base) {
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        case '`': return '~';
        default: return base;
    }
}

static int test_key_shapes(lv_engine* engine, lv_context_id context) {
    const uint32_t shift = 1;
    static const char printable[] = "abcdefghijklmnopqrstuvwxyz1234567890-=[]\\;',./`";
    for (const char* p = printable; *p != '\0'; ++p) {
        const char base = *p;
        const char upper = base >= 'a' && base <= 'z' ? (char)(base - 32) : shifted_ascii(base);
        /* Letters are folded by the macOS adapter, but a host that only passes
           state (C ABI) must still work: shifted sym, or base sym + Shift. */
        const ShapeOutcome fcitx = run_shape(engine, context, (uint32_t)upper, 0);
        const ShapeOutcome mac_base = run_shape(engine, context, (uint32_t)base, shift);
        const ShapeOutcome mac_shifted = run_shape(engine, context, (uint32_t)upper, shift);
        if (!same_shape(&fcitx, &mac_base)) return 0;
        if (!same_shape(&fcitx, &mac_shifted)) return 0;
    }

    static const uint32_t specials[] = {0xff08, 0xff09, 0xff0d, 0xff8d, 0xff1b, 0xffff,
                                        0xff50, 0xff57, 0xff55, 0xff56, 0xff63,
                                        0xff51, 0xff53, 0xff52, 0xff54, 0xffb1, 0xffbd, 0x20};
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); ++i) {
        const ShapeOutcome plain = run_shape(engine, context, specials[i], 0);
        const ShapeOutcome shifted = run_shape(engine, context, specials[i], shift);
        if (!same_shape(&plain, &shifted)) return 0;
    }
    lv_engine_reset(engine, context, LV_RESET_EXPLICIT, 1);
    return 1;
}

static int test_config_schema(void) {
    /* The exported schema drives host settings UIs; it must describe fields. */
    const size_t needed = lv_config_schema_json(NULL, 0);
    if (needed == 0) return 0;
    char* schema = (char*)malloc(needed + 1);
    if (schema == NULL) return 0;
    int ok = lv_config_schema_json(schema, needed + 1) == needed;
    if (ok) ok = strstr(schema, "\"fields\"") != NULL;
    if (ok) ok = strstr(schema, "\"candidate_page_size\"") != NULL;
    if (ok) ok = strstr(schema, "\"choices\"") != NULL;
    free(schema);
    return ok;
}

static int test_commit_and_render(lv_engine* engine, lv_context_id context) {
    /* A composition commits through the host callback. */
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    type_key(engine, context, 0xff0d); /* Return */
    if (g_commit_count != 1 || g_commit_length != 1 || g_commit_text[0] != 0x4f60) return 0;

    /* Candidate list rendering through the accessors. */
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    type_key(engine, context, 0xff54); /* Down */
    const lv_render_info* info = lv_engine_render(engine, context);
    if (info == NULL || !info->has_candidates || info->candidate_count == 0) return 0;
    if (info->target != LV_TARGET_CANDIDATES) return 0;
    if (info->selection_key_count == 0) return 0;
    uint32_t selection_keys[16];
    if (lv_engine_selection_keys(engine, selection_keys, 16) != info->selection_key_count) return 0;
    uint16_t candidate[64];
    if (lv_engine_candidate(engine, 0, candidate, 64) == 0) return 0;
    if (lv_engine_layout_hint(engine) != 0) return 0;
    if (lv_engine_preedit(engine, NULL, 0) == 0) return 0;
    if (info->preedit_segment_count == 0) return 0;
    int underlined = 0;
    if (lv_engine_preedit_segment(engine, 0, NULL, 0, &underlined) == 0) return 0;

    /* Marking state renders the tooltip and the marking-hint target. */
    type_key(engine, context, 0xff1b); /* Escape leaves the candidate list */
    for (const char* ch = "su3cl3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    lv_key shift_left;
    memset(&shift_left, 0, sizeof(shift_left));
    shift_left.sym = 0xff51;
    shift_left.states = 1; /* Shift */
    shift_left.frontend_states = 1;
    shift_left.raw_states = 1;
    lv_engine_key_event(engine, context, &shift_left);
    lv_engine_key_event(engine, context, &shift_left);
    info = lv_engine_render(engine, context);
    if (info == NULL || info->target != LV_TARGET_MARKING_HINT) return 0;
    if (lv_engine_aux_up(engine, NULL, 0) == 0) return 0;

    /* The tooltip text matches the accessor. */
    uint16_t tooltip[128];
    const size_t tooltip_length = lv_engine_aux_up(engine, tooltip, 128);
    if (tooltip_length == 0 || tooltip_length > 128) return 0;

    /* Context text is consumed for predictions and can be cleared. */
    lv_engine_clear_context_text(engine, context);
    return 1;
}

static int test_config(lv_engine* engine) {
    const size_t needed = lv_engine_config_json(engine, NULL, 0);
    if (needed == 0) return 0;
    char* json = (char*)malloc(needed + 1);
    if (json == NULL) return 0;
    const size_t length = lv_engine_config_json(engine, json, needed + 1);
    int ok = length == needed;
    if (ok) ok = lv_engine_set_config_json(engine, json, length) == 0;
    if (ok) ok = lv_engine_set_config_json(engine, "{not json", 9) != 0;
    /* A plain reload from disk accepts valid JSON and rejects malformed input
       without settling the sessions. */
    if (ok) ok = lv_engine_reload_config_json(engine, json, length) == 0;
    if (ok) ok = lv_engine_reload_config_json(engine, "{not json", 9) != 0;
    free(json);
    return ok;
}

int run_c_api_tests(void) {
    /* This is a host-free bridge test, not a desktop accessibility probe. */
    const char* previous_atspi = getenv("LLAVON_IME_ATSPI_LIBRARY");
    char* saved_atspi = previous_atspi != NULL ? strdup(previous_atspi) : NULL;
    setenv("LLAVON_IME_ATSPI_LIBRARY", "/nonexistent/llavon-ime-libatspi.so.0", 1);
    char overrides_path[256];
    snprintf(overrides_path, sizeof(overrides_path), "/tmp/llavon-ime-c-api-%d.txt", (int)getpid());

    /* Isolate the engine from the developer's real config so the default
       settings are exercised regardless of what is installed on the host. */
    char config_home[256];
    snprintf(config_home, sizeof(config_home), "/tmp/llavon-ime-c-api-config-%d", (int)getpid());
    mkdir(config_home, 0700);
    const char* previous_config_home = getenv("XDG_CONFIG_HOME");
    char saved_config_home[256] = {0};
    if (previous_config_home != NULL) {
        snprintf(saved_config_home, sizeof(saved_config_home), "%s", previous_config_home);
    }
    setenv("XDG_CONFIG_HOME", config_home, 1);

    lv_engine_options options;
    lv_engine_options_init(&options);
    /* Unset numeric options follow the effective config, like the fcitx5
       addon's transport defaults; only an explicit value overrides them. */
    if (options.context_length != 0 || options.threads != 0 ||
        options.gpu_layers != LV_GPU_LAYERS_FROM_CONFIG || options.idle_timeout_seconds != 0) {
        fprintf(stderr, "C ABI option defaults are not config-driven\n");
        return EXIT_FAILURE;
    }
    options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    options.model_path = lv_test_model_path;
    options.phrase_overrides_path = overrides_path;
    options.auto_start_service = 0;
    options.enable_accessibility = 0;
    options.socket_path = "/tmp/llavon-ime-c-api-no-service.sock";

    lv_host host;
    memset(&host, 0, sizeof(host));
    host.post = host_post;
    host.commit = host_commit;
    host.update_ui = host_update_ui;
    host.surrounding_text = host_surrounding;
    host.is_sensitive = host_is_sensitive;

    lv_engine* engine = NULL;
    if (lv_engine_create(&options, &host, &engine) != 0 || engine == NULL) return EXIT_FAILURE;

    const lv_context_id context = 77;
    lv_engine_attach(engine, context);
    int ok = test_commit_and_render(engine, context);
    if (ok) {
        lv_engine_reset(engine, context, LV_RESET_EXPLICIT, 1);
        for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
        pump_until_idle(200);
        const lv_render_info* before = lv_engine_render(engine, context);
        const size_t length = before->preedit_length;
        const int commits = g_commit_count;
        ok = lv_engine_restart_prediction_service(NULL) != 0 && lv_engine_restart_prediction_service(engine) == 0;
        pump_until_idle(200);
        const lv_render_info* after = lv_engine_render(engine, context);
        ok = ok && after->preedit_length == length && g_commit_count == commits;
    }
    if (ok) ok = test_shift_letter_from_state(engine, context);
    if (ok) ok = test_key_shapes(engine, context);
    if (ok) ok = test_config(engine);
    if (ok) ok = test_config_schema();
    if (ok && lv_engine_accessibility_availability(engine) == LV_AVAILABILITY_AVAILABLE) ok = 0;
    if (ok) {
        /* A release event enters the engine but is never consumed. */
        lv_key release;
        memset(&release, 0, sizeof(release));
        release.sym = 'a';
        release.release = 1;
        if (lv_engine_key_event(engine, context, &release) != 0) ok = 0;
    }
    if (ok) {
        lv_engine_reset(engine, context, LV_RESET_FOCUS_OUT, 1);
        lv_engine_deactivate(engine, context);
        lv_engine_detach(engine, context);
    }
    lv_engine_destroy(engine);
    if (saved_atspi != NULL) { setenv("LLAVON_IME_ATSPI_LIBRARY", saved_atspi, 1); free(saved_atspi); }
    else unsetenv("LLAVON_IME_ATSPI_LIBRARY");
    unlink(overrides_path);
    if (saved_config_home[0] != '\0') {
        setenv("XDG_CONFIG_HOME", saved_config_home, 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
    rmdir(config_home);
    if (!ok) fprintf(stderr, "C ABI tests failed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else

int run_c_api_tests(void) { return EXIT_SUCCESS; }

#endif

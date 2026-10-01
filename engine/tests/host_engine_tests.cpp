#include "test_suites.h"

#ifndef _WIN32

#include "fake_host.hpp"

#include "host/engine.hpp"
#include "host/render_state.hpp"
#include "input/input_key.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include "text/utf.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace llavon::ime;
using llavon::ime::test::FakeHost;

[[noreturn]] void fail(const char* step) {
    std::fprintf(stderr, "host engine test failed: %s\n", step);
    std::exit(EXIT_FAILURE);
}

std::filesystem::path test_path(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("llavon-ime-host-" + std::string(name) + "-" + std::to_string(getpid()) + ".txt");
}

EngineOptions test_options(Config config, const std::filesystem::path& overrides_path) {
    EngineOptions options;
    options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    options.phrase_overrides_path = overrides_path;
    options.config = std::move(config);
    options.enable_accessibility = false;
    options.transport.socket_path = overrides_path.parent_path() / "llavon-ime-no-service.sock";
    options.transport.auto_start = false;
    return options;
}

InputKey make_key(char32_t sym, std::uint32_t states = 0) {
    InputKey key;
    key.sym = sym;
    key.states = states;
    key.frontend_states = states;
    key.raw_states = states;
    return key;
}

void type(Engine& engine, ContextId context, std::u16string_view text) {
    for (const char16_t ch : text) {
        (void)engine.key_event(context, make_key(ch));
    }
}

bool test_release_is_not_consumed(Engine& engine, ContextId context) {
    (void)engine.key_event(context, make_key(U's'));
    const auto before = engine.render_state(context);
    auto release = make_key(U's');
    release.release = true;
    if (engine.key_event(context, release)) return false;
    const auto after = engine.render_state(context);
    return preedit_text(after) == preedit_text(before);
}

bool test_candidates_and_layout(Engine& engine, ContextId context) {
    type(engine, context, u"su3");
    (void)engine.key_event(context, make_key(keysym::Down));
    const auto state = engine.render_state(context);
    if (!state.has_candidates || state.candidates.empty()) return false;
    if (state.candidate_target != RenderTarget::Candidates) return false;
    if (state.layout_hint != "vertical") return false;
    if (state.page_size != 5) return false;
    const int expected_keys = 10;
    return static_cast<int>(state.selection_keys.size()) == expected_keys;
}

bool test_marking_hint_target(Engine& engine, ContextId context) {
    type(engine, context, u"su3cl3");
    const std::uint32_t shift = static_cast<std::uint32_t>(InputKeyState::Shift);
    for (int i = 0; i < 2; ++i) (void)engine.key_event(context, make_key(keysym::Left, shift));
    const auto state = engine.render_state(context);
    if (state.candidate_target != RenderTarget::MarkingHint) return false;
    if (state.candidates.size() != 1) return false;
    return state.aux_up.find(u"強制替代詞彙") != std::u16string::npos;
}

bool test_symbol_menu_target(Engine& engine, ContextId context) {
    (void)engine.key_event(context, make_key(keysym::grave));
    const auto state = engine.render_state(context);
    if (state.candidate_target != RenderTarget::SymbolMenu) return false;
    if (!state.has_candidates || state.candidates.empty()) return false;
    return state.symbol_epoch != 0;
}

bool test_contexts_are_isolated(Engine& engine) {
    constexpr ContextId first = 41;
    constexpr ContextId second = 42;
    engine.attach(first);
    engine.attach(second);
    type(engine, first, u"su3");
    if (engine.render_state(second).composition_empty != true) return false;
    if (engine.render_state(second).has_candidates) return false;
    engine.detach(second);
    if (engine.has_context(second)) return false;
    if (!engine.has_context(first)) return false;
    return !engine.render_state(first).composition_empty;
}

bool test_sensitive_context(Engine& engine, ContextId context, FakeHost& host) {
    HostContext surrounding;
    surrounding.valid = true;
    surrounding.text = u"history";
    surrounding.cursor = 7;
    surrounding.anchor = 7;
    host.set_surrounding(surrounding);

    type(engine, context, u"su3");
    auto* session = engine.session(context);
    if (session == nullptr || session->context_text != u"history") return false;

    host.set_sensitive(true);
    engine.clear_context_text(context);
    type(engine, context, u"cl3");
    session = engine.session(context);
    if (session == nullptr) return false;
    return session->context_text.empty();
}

bool test_phrase_overrides_loaded_at_start(const std::filesystem::path& overrides_path) {
    {
        PhraseOverrideStore store(overrides_path);
        const std::vector<std::u16string> readings{u"ㄋㄧˇ", u"ㄏㄠˇ"};
        if (!store.add(u"妳好", readings)) return false;
    }

    FakeHost host;
    Config config = default_config();
    config.smart_english = false;
    Engine engine(test_options(config, overrides_path), host);

    // The engine must read the shared settings file on construction; the
    // native macOS host has no explicit reload hook.
    const std::vector<std::u16string> readings{u"ㄋㄧˇ", u"ㄏㄠˇ"};
    const auto phrase = engine.phrase_overrides().lookup(readings);
    return phrase == std::optional<std::u16string>(u"妳好");
}

bool test_config_change_settles_pending(Engine& engine, ContextId context) {
    type(engine, context, u"hello");
    auto* session = engine.session(context);
    if (session == nullptr || session->pending_token.empty()) return false;

    Config changed = engine.config();
    changed.smart_english = false;
    engine.set_config(changed);

    session = engine.session(context);
    if (session == nullptr) return false;
    if (!session->pending_token.empty()) return false;
    if (session->mixed_decision.active()) return false;
    return session->buffer.commit_text() == u"hello";
}

// The native macOS host reports the unshifted keysym plus the Shift state
// (AppKit's charactersIgnoringModifiers), while fcitx5 folds Shift into the
// keysym. Both shapes must drive the same Shift+letter rules.
bool test_shift_letter_reported_as_state(Engine& engine, ContextId context, FakeHost& host) {
    const std::uint32_t shift = static_cast<std::uint32_t>(InputKeyState::Shift);
    const std::uint32_t ctrl = static_cast<std::uint32_t>(InputKeyState::Ctrl);

    // Earlier tests left a symbol menu open; start from an empty composition.
    engine.reset(context, InputResetReason::Explicit, true);

    // An empty composition hands Shift+letter to the application.
    if (engine.key_event(context, make_key(U'a', shift))) return false;
    if (!engine.render_state(context).composition_empty) return false;

    // A composition commits together with the uppercase letter.
    type(engine, context, u"su3");
    const auto commits_before = host.commits().size();
    if (!engine.key_event(context, make_key(U'a', shift))) return false;
    const auto after_letter = host.commits();
    if (after_letter.size() != commits_before + 1 || after_letter.back().second != u"你A") return false;
    if (!engine.render_state(context).composition_empty) return false;

    // Ctrl+Shift+letter and Shift+Tab stay shortcuts: the composition is left
    // alone and the key is not consumed.
    type(engine, context, u"su3");
    if (engine.key_event(context, make_key(U'a', shift | ctrl))) return false;
    if (engine.key_event(context, make_key(keysym::Tab, shift))) return false;
    if (engine.render_state(context).composition_empty) return false;

    // Shift+comma from the state shape commits the fullwidth comma.
    engine.reset(context, InputResetReason::Explicit, true);
    type(engine, context, u"su3");
    if (!engine.key_event(context, make_key(U',', shift))) return false;
    if (!engine.key_event(context, make_key(keysym::Return))) return false;
    const auto after_punctuation = host.commits();
    if (after_punctuation.size() != commits_before + 2) return false;
    return after_punctuation.back().second == u"你，";
}

// Keypad digits keep their literal meaning: with nothing to compose they are
// left to the application, while a composition in progress absorbs them as
// literals so the preedit stays editable and commits with the digit inside.
bool test_keypad_digits_join_composition(Engine& engine, ContextId context, FakeHost& host) {
    constexpr char32_t kKeypad5 = 0xffb5;

    engine.reset(context, InputResetReason::Explicit, true);
    if (engine.key_event(context, make_key(kKeypad5))) return false;
    if (!engine.render_state(context).composition_empty) return false;

    const auto commits_before = host.commits().size();
    type(engine, context, u"su3");
    if (!engine.key_event(context, make_key(kKeypad5))) return false;
    if (host.commits().size() != commits_before) return false;
    const auto state = engine.render_state(context);
    if (state.composition_empty) return false;
    if (preedit_text(state) != u"你5") return false;

    // Enter commits the composition with the digit inside.
    if (!engine.key_event(context, make_key(keysym::Return))) return false;
    const auto commits = host.commits();
    if (commits.size() != commits_before + 1) return false;
    if (commits.back().second != u"你5") return false;
    return engine.render_state(context).composition_empty;
}

// Keypad operators keep their literal meaning too: with nothing to compose the
// key is left to the application, while a composition in progress absorbs the
// character so the preedit stays editable and commits with it.
bool test_keypad_operator_joins_composition(Engine& engine, ContextId context, FakeHost& host) {
    constexpr char32_t kKeypadDecimal = 0xffae;

    engine.reset(context, InputResetReason::Explicit, true);
    if (engine.key_event(context, make_key(kKeypadDecimal))) return false;
    if (!engine.render_state(context).composition_empty) return false;

    const auto commits_before = host.commits().size();
    type(engine, context, u"su3");
    if (!engine.key_event(context, make_key(kKeypadDecimal))) return false;
    if (host.commits().size() != commits_before) return false;
    const auto state = engine.render_state(context);
    if (state.composition_empty) return false;
    if (preedit_text(state) != u"你.") return false;

    // Enter commits the composition with the character inside.
    if (!engine.key_event(context, make_key(keysym::Return))) return false;
    const auto commits = host.commits();
    if (commits.size() != commits_before + 1) return false;
    if (commits.back().second != u"你.") return false;
    return engine.render_state(context).composition_empty;
}

// A pending English token settles into the composition first, so the keypad
// digit joins the settled text instead of committing it.
bool test_keypad_digit_joins_pending_token(Engine& engine, ContextId context, FakeHost& host) {
    constexpr char32_t kKeypad3 = 0xffb3;

    // The preceding test leaves smart English off; this rule needs it on.
    Config config = engine.config();
    config.smart_english = true;
    engine.set_config(config);

    engine.reset(context, InputResetReason::Explicit, true);
    type(engine, context, u"hello");
    auto* session = engine.session(context);
    if (session == nullptr || session->pending_token.empty()) return false;

    const auto commits_before = host.commits().size();
    if (!engine.key_event(context, make_key(kKeypad3))) return false;
    if (host.commits().size() != commits_before) return false;
    const auto preedit = preedit_text(engine.render_state(context));
    if (preedit.empty() || preedit.back() != u'3') return false;

    if (!engine.key_event(context, make_key(keysym::Return))) return false;
    const auto commits = host.commits();
    return commits.size() == commits_before + 1 && commits.back().second == preedit;
}

}  // namespace

int run_host_engine_tests() {
    const auto overrides_path = test_path("engine-overrides");

    {
        FakeHost host;
        Config config = default_config();
        config.smart_english = false;
        config.candidate_layout = "vertical";
        config.candidate_page_size = 5;
        Engine engine(test_options(config, overrides_path), host);
        const ContextId context = 7;
        engine.attach(context);

        if (!test_release_is_not_consumed(engine, context)) fail("release");
        if (!test_candidates_and_layout(engine, context)) fail("candidates");
        if (!test_marking_hint_target(engine, context)) fail("marking hint");
        if (!test_symbol_menu_target(engine, context)) fail("symbol menu");
        if (!test_shift_letter_reported_as_state(engine, context, host)) fail("shift letter state");
        if (!test_keypad_digits_join_composition(engine, context, host)) fail("keypad digits");
        if (!test_keypad_operator_joins_composition(engine, context, host)) fail("keypad operators");
        if (!host.pump_until([&]() { return host.redraw_count() > 0; })) fail("redraw 1");
    }

    {
        FakeHost host;
        Config config = default_config();
        config.smart_english = false;
        Engine engine(test_options(config, overrides_path), host);
        if (!test_contexts_are_isolated(engine)) fail("isolation");
        if (!host.pump_until([&]() { return host.redraw_count() > 0; })) fail("redraw 2");
    }

    {
        FakeHost host;
        Config config = default_config();
        config.smart_english = false;
        Engine engine(test_options(config, overrides_path), host);
        const ContextId context = 9;
        engine.attach(context);
        if (!test_sensitive_context(engine, context, host)) fail("sensitive");
        if (!host.pump_until([&]() { return host.redraw_count() > 0; })) fail("redraw 3");
    }

    {
        FakeHost host;
        Config config = default_config();
        config.smart_english = true;
        Engine engine(test_options(config, overrides_path), host);
        const ContextId context = 11;
        engine.attach(context);
        if (!test_config_change_settles_pending(engine, context)) fail("config settle");
        if (!test_keypad_digit_joins_pending_token(engine, context, host)) fail("keypad pending");
        if (!host.pump_until([&]() { return host.redraw_count() > 0; })) fail("redraw 4");
    }

    if (!test_phrase_overrides_loaded_at_start(overrides_path)) fail("phrase overrides at start");

    std::error_code error;
    std::filesystem::remove(overrides_path, error);
    return EXIT_SUCCESS;
}

#else

int run_host_engine_tests() { return EXIT_SUCCESS; }

#endif

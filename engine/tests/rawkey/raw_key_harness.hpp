#pragma once

// The standard raw-key test harness: keys in, panel state and commits out.
//
// Every behaviour test drives the engine the way a frontend does (raw key
// events through Engine::key_event) and observes what the frontend would
// render (Engine::render_state) and commit (Host::commit). The harness is
// host-free and platform-neutral, so the same suites run on Linux and macOS
// and no frontend keeps a private test architecture of its own.
//
// Scenarios that need the prediction service set HarnessOptions::service_path
// and the transport starts a real service on the harness socket.

#include "config/config.hpp"
#include "fake_host.hpp"
#include "host/engine.hpp"
#include "host/render_state.hpp"
#include "input/input_key.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llavon::ime::rawkey {

// Thrown by RAWKEY_ASSERT; the runner catches it per suite so one failure does
// not stop the remaining suites.
struct Failure {
    std::string message;
};

[[noreturn]] void fail(const char* expression, const char* file, int line);

#define RAWKEY_ASSERT(expression) \
    ((expression) ? (void)0 : ::llavon::ime::rawkey::fail(#expression, __FILE__, __LINE__))

// Parses a key spec: "Return", "KP_5", "Shift+A", "Control+space", "Left",
// "3", "page_down", ... Modifiers are Shift, Control/ Ctrl, Alt, Super/Cmd.
InputKey parse_key(std::string_view spec);

// One key press, built the way a frontend builds it.
class Key {
public:
    explicit Key(std::string_view spec) : key_(parse_key(spec)) {}
    explicit Key(char32_t symbol);
    explicit Key(char symbol) : Key(static_cast<char32_t>(static_cast<unsigned char>(symbol))) {}

    Key with(std::uint32_t states) const {
        Key copy = *this;
        copy.key_.states |= states;
        copy.key_.frontend_states |= states;
        copy.key_.raw_states |= states;
        if (states & input_key_state(InputKeyState::CapsLock)) copy.key_.caps_lock = true;
        return copy;
    }

    const InputKey& input_key() const { return key_; }

private:
    InputKey key_;
};

// Modifier bits for Key::with(), matching the frontends' key states.
inline constexpr std::uint32_t kShift = input_key_state(InputKeyState::Shift);
inline constexpr std::uint32_t kCapsLock = input_key_state(InputKeyState::CapsLock);
inline constexpr std::uint32_t kCtrl = input_key_state(InputKeyState::Ctrl);
inline constexpr std::uint32_t kAlt = input_key_state(InputKeyState::Alt);
inline constexpr std::uint32_t kSuper = input_key_state(InputKeyState::Super);

struct HarnessOptions {
    // Offline suites keep their deterministic baseline. Service-backed live
    // preview suites explicitly enable SmartModelPreview.
    Config config = [] {
        auto value = default_config();
        value.smart_model_preview = false;
        return value;
    }();
    std::function<void(const InputEffect::CommitSample&, std::u16string_view)> on_training_commit;
    std::function<void(const llavon::ime::protocol::SessionId&)> on_training_discard;
    // Shortens the engine's commit correction window for tests.
    std::chrono::milliseconds commit_correction_window{std::chrono::seconds(10)};
    // Set service_path (and optionally socket_path/model_path/tables_dir) to
    // run against a real prediction service; the transport starts it on demand.
    std::string service_path;
    std::string socket_path;
    std::string model_path;
    std::string tables_dir;
    std::string phrase_overrides_path;
    // File backing the accessibility context source; empty keeps it off.
    std::string context_sample_path;
    bool enable_accessibility = false;
    // Optional last-resort memory source used by raw-key integration tests.
    std::string memory_helper_path;
};

class Harness {
public:
    Harness();
    explicit Harness(HarnessOptions options);
    ~Harness();
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // MARK: Raw keys

    void key(std::string_view spec);
    void key(const Key& key);
    bool key_accepted(std::string_view spec);
    bool key_accepted(const Key& key);
    // Types printable characters one key at a time.
    void type(std::string_view text);

    // MARK: Panel state

    std::string preedit() const;
    bool composition_empty() const;
    bool update_ready() const { return !engine_->has_pending_composition(); }
    bool has_candidates() const;
    std::size_t candidate_count() const;
    std::string candidate(std::size_t index) const;
    std::vector<std::string> candidates() const;
    std::vector<std::string> selection_keys() const;
    // Choose a visible candidate by its text using the actual selection key.
    void choose_text(std::string_view text);
    // Cursor row within the candidate list, or -1 when no list is open.
    int cursor_index() const;
    int render_page() const;
    int render_page_size() const;
    std::string layout_hint() const;
    std::string aux_up() const;

    // MARK: Commits

    // Presses Return and requires `text` to be the committed text, or to have
    // been the last commit already (candidate selection commits directly).
    void expect_commit(std::string_view text);
    // Presses a key that commits directly and requires the committed text.
    void expect_direct_commit(std::string_view text, const Key& key);
    // A mixed-input Space stays editable; only Return submits to the client.
    void expect_space_then_commit(std::string_view text);
    void expect_focus_out_commit(std::string_view text);
    std::vector<std::string> commits() const;
    std::string last_commit() const;

    // MARK: Settings (fcitx5 addon paths and values are translated)

    void set_config(std::string_view path, std::string_view value);
    void set_configs(std::initializer_list<std::pair<std::string, std::string>> values);
    void set_config_json(std::string_view json);
    const Config& config() const;

    // MARK: Context

    void set_surrounding(std::string_view text, std::size_t cursor, std::size_t anchor);
    // Path of the phrase override file this harness reads and writes.
    const std::string& phrase_overrides_path() const { return options_.phrase_overrides_path; }
    void reload_phrase_overrides();
    void clear_context_text();
    // Context adopted for the current input context, as UTF-8.
    std::string context_text() const;
    std::size_t memory_probe_count() const;
    // Detaches the context, which closes the prediction service session.
    void detach();
    void activate();
    // Choose another client without settling the previous client's text.
    void use_context(ContextId context);
    void focus_out();
    void reset();
    InputSession* session() const;

    // MARK: Prediction and service

    void settle_prediction();
    bool pending_model_idle() const;
    std::uint64_t pending_model_requests() const;
    // Runs queued host work until `predicate` holds (service responses arrive
    // through it); returns whether the predicate held in time.
    bool pump_until(const std::function<bool()>& predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // Runs the host work already queued.
    void drain();

    test::FakeHost& host() { return host_; }
    Engine& engine() { return *engine_; }

private:
    void apply_environment() const;
    RenderState render() const;

    HarnessOptions options_;
    test::FakeHost host_;
    std::unique_ptr<Engine> engine_;
    ContextId context_ = 1;
    std::string temp_root_;
    // Set by expect_commit when the composition is not committed yet: the next
    // commit must match it, mirroring the frontends' commit expectations.
    std::optional<std::string> pending_commit_;
};

// MARK: Suite registration

// Entry point for the runner's exec-based memory-context test client.
int committed_client_main(std::string_view format, int commands, int responses);

class SuiteRegistrar {
public:
    SuiteRegistrar(const char* name, void (*body)());
};

// Declares one suite: RAWKEY_SUITE("name", suite_function) { ... }
#define RAWKEY_SUITE(name, suite_function)                                                   \
    static void suite_function();                                                            \
    static const ::llavon::ime::rawkey::SuiteRegistrar rawkey_registrar_##suite_function(    \
        name, suite_function);                                                               \
    static void suite_function()

}  // namespace llavon::ime::rawkey

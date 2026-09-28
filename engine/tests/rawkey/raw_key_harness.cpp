#include "raw_key_harness.hpp"

#include "text/utf.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>

namespace llavon::ime::rawkey {

namespace {

namespace fs = std::filesystem;

std::atomic<int> harness_counter{0};

constexpr std::uint32_t shift_state() { return input_key_state(InputKeyState::Shift); }
constexpr std::uint32_t ctrl_state() { return input_key_state(InputKeyState::Ctrl); }
constexpr std::uint32_t alt_state() { return input_key_state(InputKeyState::Alt); }
constexpr std::uint32_t super_state() { return input_key_state(InputKeyState::Super); }

std::optional<char32_t> named_keysym(std::string_view name) {
    // Printable keys by their X11 names, so ported scenarios keep the key
    // names the frontends use.
    if (name == "space") return 0x20;
    if (name == "exclam") return U'!';
    if (name == "quotedbl") return U'"';
    if (name == "numbersign") return U'#';
    if (name == "dollar") return U'$';
    if (name == "percent") return U'%';
    if (name == "ampersand") return U'&';
    if (name == "apostrophe") return U'\'';
    if (name == "parenleft") return U'(';
    if (name == "parenright") return U')';
    if (name == "asterisk") return U'*';
    if (name == "plus") return U'+';
    if (name == "comma") return U',';
    if (name == "minus") return U'-';
    if (name == "period") return U'.';
    if (name == "slash") return U'/';
    if (name == "colon") return U':';
    if (name == "semicolon") return U';';
    if (name == "less") return U'<';
    if (name == "equal") return U'=';
    if (name == "greater") return U'>';
    if (name == "question") return U'?';
    if (name == "at") return U'@';
    if (name == "bracketleft") return U'[';
    if (name == "backslash") return U'\\';
    if (name == "bracketright") return U']';
    if (name == "asciicircum") return U'^';
    if (name == "underscore") return U'_';
    if (name == "grave") return U'`';
    if (name == "braceleft") return U'{';
    if (name == "bar") return U'|';
    if (name == "braceright") return U'}';
    if (name == "asciitilde") return U'~';
    // Navigation, editing and whitespace keys the engine interprets.
    if (name == "Return" || name == "Enter") return 0xff0d;
    if (name == "KP_Enter" || name == "KP_Return") return 0xff8d;
    if (name == "Tab") return 0xff09;
    if (name == "Space") return 0x20;
    if (name == "BackSpace" || name == "Backspace" || name == "backspace") return 0xff08;
    if (name == "Delete" || name == "ForwardDelete") return 0xffff;
    if (name == "Escape" || name == "Esc") return 0xff1b;
    if (name == "Caps_Lock" || name == "CapsLock") return 0xffe5;
    if (name == "Clear") return 0xff0b;
    if (name == "Help") return 0xff6a;
    if (name == "Home") return 0xff50;
    if (name == "End") return 0xff57;
    if (name == "Page_Up" || name == "PageUp" || name == "Prior") return 0xff55;
    if (name == "Page_Down" || name == "PageDown" || name == "Next") return 0xff56;
    if (name == "Left") return 0xff51;
    if (name == "Right") return 0xff53;
    if (name == "Up") return 0xff52;
    if (name == "Down") return 0xff54;
    if (name == "Insert") return 0xff63;
    // Numeric keypad.
    if (name == "KP_Decimal") return 0xffae;
    if (name == "KP_Separator") return 0xffac;
    if (name == "KP_Add") return 0xffab;
    if (name == "KP_Subtract") return 0xffad;
    if (name == "KP_Multiply") return 0xffaa;
    if (name == "KP_Divide") return 0xffaf;
    if (name == "KP_Equal") return 0xffbd;
    if (name.size() == 4 && name.substr(0, 3) == "KP_" && name[3] >= '0' && name[3] <= '9') {
        return static_cast<char32_t>(0xffb0 + (name[3] - '0'));
    }
    return std::nullopt;
}

// The engine hands over the whole list plus the current page; slicing the page
// is the frontend's job, so the harness exposes page-relative indices.
struct PageWindow {
    std::size_t offset = 0;
    std::size_t count = 0;
};

PageWindow page_window(const RenderState& state) {
    if (state.candidates.empty()) return {};
    const std::size_t page_size =
        state.page_size > 0 ? static_cast<std::size_t>(state.page_size) : state.candidates.size();
    const std::size_t offset = static_cast<std::size_t>(state.page) * page_size;
    if (offset >= state.candidates.size()) return {offset, 0};
    return {offset, std::min(page_size, state.candidates.size() - offset)};
}

}  // namespace

void fail(const char* expression, const char* file, int line) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + expression};
}

InputKey parse_key(std::string_view spec) {
    std::uint32_t states = 0;
    std::string_view rest = spec;
    while (true) {
        const auto plus = rest.find('+');
        if (plus == std::string_view::npos) break;
        const auto name = rest.substr(0, plus);
        std::uint32_t bit = 0;
        if (name == "Shift") bit = shift_state();
        else if (name == "Control" || name == "Ctrl") bit = ctrl_state();
        else if (name == "Alt" || name == "Option" || name == "Mod1") bit = alt_state();
        else if (name == "Super" || name == "Cmd" || name == "Command" || name == "Meta") bit = super_state();
        else if (name == "CapsLock" || name == "Caps_Lock") bit = input_key_state(InputKeyState::CapsLock);
        else break;  // The '+' belongs to the key name, not to a modifier.
        states |= bit;
        rest.remove_prefix(plus + 1);
    }

    InputKey key;
    if (rest.empty()) throw Failure{"empty key spec: " + std::string(spec)};
    if (rest.size() == 1) {
        key.sym = static_cast<char32_t>(static_cast<unsigned char>(rest.front()));
    } else if (const auto named = named_keysym(rest)) {
        key.sym = *named;
    } else {
        throw Failure{"unknown key: " + std::string(rest)};
    }
    key.states = states;
    key.frontend_states = states;
    key.raw_states = states;
    key.caps_lock = (states & input_key_state(InputKeyState::CapsLock)) != 0;
    return key;
}

Key::Key(char32_t symbol) {
    key_.sym = symbol;
}

Harness::Harness() : Harness(HarnessOptions{}) {}

Harness::Harness(HarnessOptions options) : options_(std::move(options)) {
    temp_root_ = (fs::temp_directory_path() /
                  ("llavon-ime-rawkey-" + std::to_string(::getpid()) + "-" +
                   std::to_string(harness_counter.fetch_add(1))))
                     .string();
    fs::remove_all(temp_root_);
    fs::create_directories(temp_root_);
    if (options_.phrase_overrides_path.empty()) {
        options_.phrase_overrides_path = temp_root_ + "/phrase_overrides.txt";
    }
    if (options_.socket_path.empty()) options_.socket_path = temp_root_ + "/service.sock";
    apply_environment();

    EngineOptions engine_options;
    engine_options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    engine_options.phrase_overrides_path = options_.phrase_overrides_path;
    engine_options.config = options_.config;
    engine_options.on_training_commit = options_.on_training_commit;
    engine_options.on_training_discard = options_.on_training_discard;
    engine_options.commit_correction_window = options_.commit_correction_window;
    engine_options.enable_accessibility = options_.enable_accessibility;
    engine_options.enable_memory_context = !options_.memory_helper_path.empty();
    if (engine_options.enable_memory_context) engine_options.config.memory_context = true;
    engine_options.transport.socket_path = options_.socket_path;
    engine_options.transport.service_path = options_.service_path;
    engine_options.transport.model_path = options_.model_path;
    engine_options.transport.tables_dir = options_.tables_dir;
    engine_options.transport.auto_start = !options_.service_path.empty();
    engine_ = std::make_unique<Engine>(engine_options, host_);
    engine_->attach(context_);
}

Harness::~Harness() {
    if (engine_) engine_->detach(context_);
    engine_.reset();
    if (!options_.memory_helper_path.empty()) ::unsetenv("LLAVON_IME_MEMSCAN_PATH");
    std::error_code error;
    fs::remove_all(temp_root_, error);
}

void Harness::apply_environment() const {
    if (!options_.memory_helper_path.empty()) {
        ::setenv("LLAVON_IME_MEMSCAN_PATH", options_.memory_helper_path.c_str(), 1);
    }
    if (!options_.context_sample_path.empty()) {
        ::setenv("LLAVON_IME_CONTEXT_SAMPLE_FILE", options_.context_sample_path.c_str(), 1);
    }
    // Without a service the transport must not retry an auto-start on every
    // prediction; with one it must be allowed to spawn it.
    if (options_.service_path.empty()) {
        ::setenv("LLAVON_IME_DISABLE_SERVICE", "1", 1);
    } else {
        ::unsetenv("LLAVON_IME_DISABLE_SERVICE");
    }
}

RenderState Harness::render() const {
    return engine_->render_state(context_);
}

void Harness::key(const Key& key) {
    const auto before = host_.commits().size();
    (void)engine_->key_event(context_, key.input_key());
    drain();
    const auto after = commits();
    if (pending_commit_ && after.size() > before) {
        RAWKEY_ASSERT(after.back() == *pending_commit_);
        pending_commit_.reset();
    }
}

void Harness::key(std::string_view spec) {
    key(Key(spec));
}

bool Harness::key_accepted(const Key& key) {
    const bool accepted = engine_->key_event(context_, key.input_key());
    drain();
    return accepted;
}

bool Harness::key_accepted(std::string_view spec) {
    return key_accepted(Key(spec));
}

void Harness::type(std::string_view text) {
    for (const char ch : text) key(Key(ch));
}

std::string Harness::preedit() const {
    return u16_to_utf8(preedit_text(render()));
}

bool Harness::composition_empty() const {
    return render().composition_empty;
}

bool Harness::has_candidates() const {
    const auto state = render();
    return state.has_candidates && !state.candidates.empty();
}

std::size_t Harness::candidate_count() const {
    return page_window(render()).count;
}

std::string Harness::candidate(std::size_t index) const {
    const auto state = render();
    const auto window = page_window(state);
    if (index >= window.count) return {};
    return u16_to_utf8(state.candidates[window.offset + index]);
}

std::vector<std::string> Harness::candidates() const {
    const auto state = render();
    const auto window = page_window(state);
    std::vector<std::string> result;
    for (std::size_t i = 0; i < window.count; ++i) {
        result.push_back(u16_to_utf8(state.candidates[window.offset + i]));
    }
    return result;
}

std::vector<std::string> Harness::selection_keys() const {
    std::vector<std::string> result;
    for (const char32_t symbol : render().selection_keys) result.push_back(char32_to_utf8(symbol));
    return result;
}

int Harness::cursor_index() const {
    const auto state = render();
    if (!state.has_candidates || state.candidates.empty()) return -1;
    // RenderState::cursor is page-relative; the frontends clamp it into the
    // page they draw, so the harness reports the same row.
    const int count = static_cast<int>(page_window(state).count);
    return std::clamp(state.cursor, 0, std::max(count - 1, 0));
}

int Harness::render_page() const { return render().page; }

int Harness::render_page_size() const { return render().page_size; }

std::string Harness::layout_hint() const {
    return render().layout_hint;
}

std::string Harness::aux_up() const {
    return u16_to_utf8(render().aux_up);
}

std::vector<std::string> Harness::commits() const {
    std::vector<std::string> result;
    for (const auto& entry : host_.commits()) result.push_back(u16_to_utf8(entry.second));
    return result;
}

std::string Harness::last_commit() const {
    const auto all = commits();
    return all.empty() ? std::string() : all.back();
}

void Harness::expect_commit(std::string_view text) {
    const auto before = host_.commits().size();
    pending_commit_ = std::string(text);
    key("Return");
    const auto after = commits();
    if (after.size() > before) {
        // Return committed the composition.
        RAWKEY_ASSERT(after.back() == text);
        pending_commit_.reset();
    } else if (!after.empty() && after.back() == text) {
        // The text was already committed (candidate selection commits).
        pending_commit_.reset();
    }
    // Otherwise the expectation stays pending and the next commit must match.
}

void Harness::expect_direct_commit(std::string_view text, const Key& key) {
    const auto before = host_.commits().size();
    this->key(key);
    const auto after = commits();
    RAWKEY_ASSERT(after.size() > before && after.back() == text);
}

void Harness::expect_focus_out_commit(std::string_view text) {
    const auto before = host_.commits().size();
    focus_out();
    const auto after = commits();
    RAWKEY_ASSERT(after.size() > before && after.back() == text);
}

void Harness::set_config(std::string_view path, std::string_view value) {
    Config updated = engine_->config();
    const bool on = value == "True" || value == "true" || value == "1";
    if (path == "SmartEnglish") {
        updated.smart_english = on;
    } else if (path == "BopomofoKeyboardLayout") {
        updated.keyboard_layout = (value == "許氏" || value == "hsu") ? "hsu" : "standard";
    } else if (path == "ShiftLetterKeys") {
        updated.shift_letter_keys = (value == "直接放入組字區" || value == "directly_put_to_buffer")
                                        ? "directly_put_to_buffer"
                                        : "directly_output_uppercase";
    } else if (path == "CapsLockInputsBopomofo") {
        updated.caps_lock_inputs_bopomofo = on;
    } else if (path == "ChooseCandidateUsingSpace") {
        updated.space_selects_candidate = on;
    } else if (path == "CandidatePageSize") {
        updated.candidate_page_size = std::stoi(std::string(value));
    } else if (path == "SelectPhrase") {
        updated.select_phrase = std::string(value);
    } else if (path == "SelectionKeys") {
        updated.selection_keys = value == "本位列" || value == "home_row" ? "asdfghjkl" : "1234567890";
    } else if (path == "SelectionKeysCount") {
        updated.selection_key_count = std::stoi(std::string(value));
    } else if (path == "EscKeyClearsEntireComposingBuffer") {
        updated.esc_clears_entire_buffer = on;
    } else {
        fail((std::string("unmapped config path: ") + std::string(path)).c_str(), __FILE__, __LINE__);
    }
    engine_->set_config(updated, true);
}

void Harness::set_configs(std::initializer_list<std::pair<std::string, std::string>> values) {
    for (const auto& [path, value] : values) set_config(path, value);
}

void Harness::set_config_json(std::string_view json) {
    engine_->set_config(config_from_json(nlohmann::json::parse(json)), true);
}

const Config& Harness::config() const {
    return engine_->config();
}

void Harness::set_surrounding(std::string_view text, std::size_t cursor, std::size_t anchor) {
    HostContext context;
    context.valid = true;
    context.text = utf8_to_u16(text);
    context.cursor = cursor;
    context.anchor = anchor;
    host_.set_surrounding(std::move(context));
}

void Harness::reload_phrase_overrides() {
    engine_->reload_phrase_overrides();
}

void Harness::detach() {
    engine_->detach(context_);
}

void Harness::clear_context_text() {
    engine_->clear_context_text(context_);
}

std::string Harness::context_text() const {
    return u16_to_utf8(engine_->context_text(context_));
}

std::size_t Harness::memory_probe_count() const {
    return engine_->memory_probe_count();
}

void Harness::activate() {
    engine_->activate(context_);
}

void Harness::focus_out() {
    engine_->deactivate(context_);
}

void Harness::reset() {
    engine_->reset(context_, InputResetReason::Explicit, true);
}

InputSession* Harness::session() const {
    return engine_->session(context_);
}

void Harness::settle_prediction() {
    if (auto* state = session()) state->prediction.invalidate();
}

bool Harness::pump_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    return host_.pump_until(predicate, timeout);
}

void Harness::drain() {
    (void)host_.pump_until([] { return false; }, std::chrono::milliseconds(0));
}

}  // namespace llavon::ime::rawkey

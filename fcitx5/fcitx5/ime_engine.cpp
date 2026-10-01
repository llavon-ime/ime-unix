#include "fcitx5/ime_engine.hpp"
#include "util/parse_number.hpp"
#include <memory>
#ifdef __linux__
#include "fcitx5/update_bridge.hpp"
#include <dbus_public.h>
#endif

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterfacemanager.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <xcb/xcb.h>
#endif

#include "config/config.hpp"
#include "debug/debug_log.hpp"
#include "host/render_state.hpp"
#include "text/utf.hpp"
#include "util/env.hpp"

extern char** environ;

namespace llavon::ime {

namespace {

#if defined(__linux__)
// A program name does not identify the focused window: several instances of
// kitty can each contain stable copies of the same natural composition.
// Resolve the X11 input focus to its owning client window as additional
// evidence; the scanner still considers other same-program processes.
int focused_x11_pid() {
    int screen = 0;
    const std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> owned_connection(
        xcb_connect(nullptr, &screen), &xcb_disconnect);
    auto* connection = owned_connection.get();
    if (!connection || xcb_connection_has_error(connection)) {
        return 0;
    }
    const std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> focus_reply(
        xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr), &std::free);
    if (!focus_reply) {
        return 0;
    }
    xcb_window_t window = focus_reply->focus;
    const char property_name[] = "_NET_WM_PID";
    const std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> atom_reply(
        xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, sizeof(property_name) - 1, property_name), nullptr),
        &std::free);
    if (!atom_reply) {
        return 0;
    }
    const xcb_atom_t atom = atom_reply->atom;
    int pid = 0;
    for (int depth = 0; depth < 32 && window != XCB_WINDOW_NONE &&
                        window != XCB_INPUT_FOCUS_POINTER_ROOT; ++depth) {
        const std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> property(
            xcb_get_property_reply(connection, xcb_get_property(connection, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1), nullptr),
            &std::free);
        if (property && property->type == XCB_ATOM_CARDINAL && property->format == 32 &&
            xcb_get_property_value_length(property.get()) == sizeof(std::uint32_t)) {
            const auto value = *static_cast<const std::uint32_t*>(xcb_get_property_value(property.get()));
            if (value > 1 && value <= static_cast<std::uint32_t>(INT_MAX)) pid = static_cast<int>(value);
        }
        if (pid) break;
        const std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> parent(
            xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr), &std::free);
        if (!parent) break;
        const xcb_window_t next = parent->parent;
        if (next == window) break;
        window = next;
    }
    return pid;
}
#endif

std::string accessibility_status_text(const AccessibilityContextState& state) {
    switch (state.availability) {
        case AccessibilityAvailability::Disabled:
            return "已停用";
        case AccessibilityAvailability::Unsupported:
            return "此平台不支援";
        case AccessibilityAvailability::Available:
            if (state.detail == "sample-file") return "樣本檔案: 可取得";
            if (state.detail == "atspi") return "AT-SPI: 可取得";
            return "無障礙: 可取得";
        case AccessibilityAvailability::Unavailable:
            if (state.detail == "libatspi-missing") return "AT-SPI: 不可用(未安裝 at-spi2-core)";
            if (state.detail == "a11y-bus-unavailable") {
                return "AT-SPI: 不可用(無法連線 a11y bus;請安裝或啟動 at-spi2-core)";
            }
            if (state.detail == "atspi-init-failed") return "AT-SPI: 不可用(初始化失敗)";
            if (state.detail == "atspi-listener-failed") return "AT-SPI: 不可用(無法註冊事件監聽)";
            if (state.detail == "atspi-loop-failed") return "AT-SPI: 不可用(事件迴圈建立失敗)";
            return state.detail.empty() ? "無障礙: 不可用" : "無障礙: 不可用(" + state.detail + ")";
    }
    return "無障礙: 未知";
}

std::string memory_status_text(const AccessibilityContextState& state) {
    switch (state.availability) {
        case AccessibilityAvailability::Available:
            return "可取得";
        case AccessibilityAvailability::Disabled:
            return "已停用";
        case AccessibilityAvailability::Unsupported:
            return "此平台不支援";
        case AccessibilityAvailability::Unavailable:
            if (state.detail == "helper-missing") return "不可用(未安裝 llavon-ime-memscan)";
            if (state.detail == "helper-not-executable") return "不可用(helper 無法執行)";
            if (state.detail == "permission-denied") {
                return "不可用(需要 CAP_SYS_PTRACE 或 ptrace_scope=0)";
            }
            return state.detail.empty() ? "不可用" : "不可用(" + state.detail + ")";
    }
    return "未知";
}

std::filesystem::path default_table_path() {    if (const char* override = env_with_legacy("LLAVON_IME_TABLE_PATH", "IME_FCITX5_TABLE_PATH")) {
        return override;
    }
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto user_path =
            std::filesystem::path(home) / "Library" / "fcitx5" / "share" / "llavon-ime" / "tables" /
            "bopomofo_char.json";
        if (std::filesystem::exists(user_path)) return user_path;
    }
#endif
#ifdef LLAVON_IME_SOURCE_TABLE_PATH
    const auto source_path = std::filesystem::path(LLAVON_IME_SOURCE_TABLE_PATH);
    if (std::filesystem::exists(source_path)) return source_path;
#endif
#ifdef LLAVON_IME_INSTALLED_TABLE_PATH
    const auto installed_path = std::filesystem::path(LLAVON_IME_INSTALLED_TABLE_PATH);
    if (std::filesystem::exists(installed_path)) return installed_path;
    return installed_path;
#endif
    return "/usr/share/llavon-ime/tables/bopomofo_char.json";
}

ServiceTransportOptions default_transport_options() {
    ServiceTransportOptions options;
    const auto config = load_config();
    options.tables_dir = default_table_path().parent_path();
    options.model_path = config.model_path;
    options.context_length = static_cast<std::uint32_t>(config.context_length);
    options.threads = static_cast<std::uint32_t>(config.thread_count);
    options.gpu_layers = config.gpu_layers;
    options.idle_timeout_seconds = static_cast<std::uint32_t>(config.idle_timeout_seconds);
    if (const char* model = env_with_legacy("LLAVON_IME_MODEL_PATH", "IME_FCITX5_MODEL_PATH")) {
        options.model_path = model;
    }
    if (env_with_legacy("LLAVON_IME_DISABLE_SERVICE", "IME_FCITX5_DISABLE_SERVICE") != nullptr) {
        options.auto_start = false;
    }
    return options;
}

fcitx::KeyList selection_key_list(const std::vector<char32_t>& keys) {
    fcitx::KeyList list;
    list.reserve(keys.size());
    for (const char32_t key : keys) {
        list.emplace_back(static_cast<fcitx::KeySym>(static_cast<unsigned char>(key)));
    }
    return list;
}

fcitx::CandidateLayoutHint candidate_layout_hint(const std::string& layout) {
    if (layout == "vertical") return fcitx::CandidateLayoutHint::Vertical;
    if (layout == "horizontal") return fcitx::CandidateLayoutHint::Horizontal;
    return fcitx::CandidateLayoutHint::NotSet;
}

// Candidate entries are selectable; the callback routes the activation back
// into the engine by context id, so it works even when the input context
// object has been recreated meanwhile.
class SelectableCandidateWord final : public fcitx::CandidateWord {
public:
    SelectableCandidateWord(fcitx::Text text, std::function<void()> callback)
        : CandidateWord(std::move(text)), callback_(std::move(callback)) {}

    void select(fcitx::InputContext*) const override { callback_(); }

private:
    std::function<void()> callback_;
};

}  // namespace

ImeEngine::ImeEngine(fcitx::Instance* instance)
    : instance_(instance) {
    if (instance_) {
        event_dispatcher_ = std::make_unique<fcitx::EventDispatcher>();
        event_dispatcher_->attach(&instance_->eventLoop());
    }
    EngineOptions options;
    options.table_path = default_table_path();
    options.phrase_overrides_path = phrase_overrides_path();
    options.config = default_config();
    options.transport = default_transport_options();
    active_service_model_path_ = options.transport.model_path;
#ifdef LLAVON_IME_NATIVE_SURROUNDING
    // The InputMethodKit client supplies surrounding text directly; there is
    // no accessibility provider to run.
    options.enable_accessibility = false;
#else
    // Linux hosts may use the memory probe as the last context source; the
    // `memory_context` setting still gates whether it probes.
    options.enable_memory_context = true;
#endif
    engine_ = std::make_unique<Engine>(std::move(options), *this);

    if (instance_ != nullptr) {
        lora_manager_action_.setShortText("拉風設定與個人化…");
        lora_manager_action_.setLongText("開啟原生輸入法設定、替代詞彙與訓練管理程式");
        lora_manager_action_.registerAction("llavon-ime-lora-manager", &instance_->userInterfaceManager());
        lora_manager_connection_ = lora_manager_action_.connect<fcitx::SimpleAction::Activated>(
            [](fcitx::InputContext*) {
                const char* override = std::getenv("LLAVON_IME_LORA_GUI_PATH");
                const std::string path = override && *override ? override :
                    std::string(LLAVON_IME_INSTALLED_LORA_GUI_PATH);
                char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
                pid_t child = -1;
                if (::posix_spawn(&child, path.c_str(), nullptr, nullptr, argv, ::environ) != 0) return;
                std::thread([child] {
                    int status;
                    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                }).detach();
            });
        (void)instance_->inputContextManager().registerProperty("llavon-ime-input-state", &property_factory_);
        capability_changed_handler_ = instance_->watchEvent(
            fcitx::EventType::InputContextCapabilityAboutToChange, fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event& event) {
                const auto& capability_event = static_cast<const fcitx::CapabilityEvent&>(event);
                if (!capability_event.newFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) return;
                auto* property_state = property(capability_event.inputContext());
                if (property_state == nullptr || property_state->id == 0) return;
                engine_->clear_context_text(property_state->id);
            });
    }
    reload_config();
#ifdef __linux__
    if (instance_) {
        if (auto* dbus = instance_->addonManager().addon("dbus", true)) {
            if (auto* bus = dbus->call<fcitx::IDBusModule::bus>()) {
                update_bridge_ = std::make_unique<UpdateBridge>([this] {
                    if (engine_->has_pending_composition() || !instance_->canRestart()) return false;
                    bool otherPreedit = false;
                    instance_->inputContextManager().foreach([&otherPreedit](fcitx::InputContext* context) {
                        const auto& panel = context->inputPanel();
                        otherPreedit = otherPreedit || !panel.preedit().empty() || !panel.clientPreedit().empty();
                        return !otherPreedit;
                    });
                    if (otherPreedit) return false;
                    instance_->restart();
                    return true;
                }, LLAVON_IME_DISPLAY_VERSION, [this] { return settings_status(); });
                if (!bus->addObjectVTable("/llavon/update", "org.llavon.IME.Update1", *update_bridge_))
                    update_bridge_.reset();
            }
        }
    }
#endif
}

ImeEngine::~ImeEngine() {
#ifdef __linux__
    update_bridge_.reset();
#endif
    // Property destructors may still run after this point during fcitx
    // teardown; the lifetime token makes their detach hooks no-ops.
    alive_.reset();
    engine_.reset();
}

ImeInputContextProperty* ImeEngine::property(fcitx::InputContext* input_context) const {
    if (input_context == nullptr) return nullptr;
    return static_cast<ImeInputContextProperty*>(input_context->property(&property_factory_));
}

ContextId ImeEngine::context_id(fcitx::InputContext* input_context) {
    auto* property_state = property(input_context);
    if (property_state == nullptr) return 0;
    if (property_state->id != 0) return property_state->id;

    const ContextId id = next_context_id_.fetch_add(1);
    property_state->id = id;
    const std::weak_ptr<bool> alive = alive_;
    property_state->on_destroy = [this, id, alive]() {
        if (alive.expired()) return;
        engine_->detach(id);
        contexts_.erase(id);
    };
    contexts_.emplace(id, input_context->watch());
    engine_->attach(id);
    property_state->session = engine_->session(id);
    return id;
}

fcitx::InputContext* ImeEngine::input_context(ContextId context) const {
    const auto it = contexts_.find(context);
    if (it == contexts_.end()) return nullptr;
    return it->second.get();
}

void ImeEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) {
    auto* input_context_ptr = event.inputContext();
    if (input_context_ptr == nullptr) return;

    const ContextId id = context_id(input_context_ptr);
    const auto raw_key = event.rawKey();
    const fcitx::Key effective_key(event.key().sym(),
                                   event.key().states() | (raw_key.states() & fcitx::KeyState::Meta),
                                   event.key().code());
    InputKey input_key;
    input_key.sym = static_cast<char32_t>(effective_key.sym());
    input_key.states = static_cast<std::uint32_t>(effective_key.states());
    input_key.frontend_states = static_cast<std::uint32_t>(event.key().states());
    input_key.raw_states = static_cast<std::uint32_t>(raw_key.states());
    input_key.caps_lock = static_cast<bool>(raw_key.states() & fcitx::KeyState::CapsLock);
    input_key.release = event.isRelease();

    if (engine_->key_event(id, input_key)) event.filterAndAccept();
}

void ImeEngine::activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    auto* input_context_ptr = event.inputContext();
    if (input_context_ptr == nullptr) return;
    const ContextId id = context_id(input_context_ptr);
    reload_config();
    input_context_ptr->statusArea().addAction(fcitx::StatusGroup::InputMethod, &lora_manager_action_);
    engine_->activate(id);
}

void ImeEngine::deactivate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    auto* input_context_ptr = event.inputContext();
    if (input_context_ptr == nullptr) return;
    engine_->deactivate(context_id(input_context_ptr));
}

void ImeEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    auto* input_context_ptr = event.inputContext();
    if (input_context_ptr == nullptr) return;
    const bool focus_out = event.type() == fcitx::EventType::InputContextFocusOut;
    const auto reason = focus_out ? InputResetReason::FocusOut
                                 : event.type() == fcitx::EventType::InputContextReset
                                       ? InputResetReason::Explicit
                                       : InputResetReason::Deactivate;
    engine_->reset(context_id(input_context_ptr), reason, true);
}

void ImeEngine::reloadConfig() {
    reload_config();
}

void ImeEngine::reload_config() {
    const auto config = load_config();
    const auto transport = default_transport_options();
    if (transport.model_path != active_service_model_path_) {
        // Restart the prediction service with the newly selected model. The
        // engine is kept: rebuilding it would tear down the accessibility
        // backend, and libatspi cannot be re-initialized in the same process.
        engine_->set_transport_options(transport);
        active_service_model_path_ = transport.model_path;
    }
    engine_->set_config(config, false);
    engine_->reload_phrase_overrides();
}

void ImeEngine::save() {
    // Fcitx may save this launcher dialog during shutdown. It must never
    // replace the app-owned INI with an empty/obsolete platform form.
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &settings_launcher_;
}

const fcitx::Configuration* ImeEngine::getSubConfig(const std::string& path) const {
    return path == "phraseoverrides" ? &settings_launcher_ : nullptr;
}

void ImeEngine::setSubConfig(const std::string&, const fcitx::RawConfig&) {
    // Old frontend submissions cannot overwrite the app-owned phrase file.
}

void ImeEngine::setConfig(const fcitx::RawConfig&) {
    reload_config();
}

std::string ImeEngine::settings_status() const {
#ifdef LLAVON_IME_NATIVE_SURROUNDING
    const std::string status = "InputMethodKit: 可取得（不需輔助使用權限）";
#else
    std::string status = accessibility_status_text(engine_->accessibility_state());
    const auto memory = engine_->memory_context_state();
    if (memory.availability != AccessibilityAvailability::Unsupported) {
        status += "；記憶體取樣: " + memory_status_text(memory);
    }
#endif
    return nlohmann::json{{"version", LLAVON_IME_DISPLAY_VERSION}, {"context", status},
        {"status", "設定由拉風原生管理器維護"}, {"config", to_json(engine_->config())}}.dump();
}

void ImeEngine::post(std::function<void()> body) {
    if (event_dispatcher_ == nullptr) return;
    event_dispatcher_->schedule(std::move(body));
}

void ImeEngine::commit(ContextId context, std::u16string_view text) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return;
    input_context_ptr->commitString(u16_to_utf8(text));
}

void ImeEngine::update_ui(ContextId context) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return;

    const RenderState state = engine_->render_state(context);
    auto& panel = input_context_ptr->inputPanel();
    panel.reset();

    if (!state.composition_empty) {
        fcitx::Text preedit;
        for (const auto& segment : state.preedit) {
            preedit.append(u16_to_utf8(segment.text),
                           segment.underlined ? fcitx::TextFormatFlag::Underline : fcitx::TextFormatFlag::NoFlag);
        }
        const auto full = preedit_text(state);
        const auto caret_units = std::min(state.caret, full.size());
        preedit.setCursor(static_cast<int>(
            u16_to_utf8(std::u16string_view(full).substr(0, caret_units)).size()));
        const bool use_client_preedit =
            input_context_ptr->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
        panel.setClientPreedit(use_client_preedit ? preedit : fcitx::Text());
        panel.setPreedit(use_client_preedit ? fcitx::Text() : preedit);
        if (!state.aux_up.empty()) panel.setAuxUp(fcitx::Text(u16_to_utf8(state.aux_up)));
    }
    panel.setAuxDown(fcitx::Text());
    input_context_ptr->updatePreedit();

    if (!state.has_candidates) {
        panel.setCandidateList(nullptr);
        input_context_ptr->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    candidates->setPageSize(state.page_size);
    candidates->setSelectionKey(selection_key_list(state.selection_keys));
    candidates->setLayoutHint(candidate_layout_hint(state.layout_hint));

    const ContextId id = context;
    const auto target = state.candidate_target;
    const auto epoch = state.symbol_epoch;
    int index = 0;
    for (const auto& candidate : state.candidates) {
        std::function<void()> activate;
        switch (target) {
            case RenderTarget::SymbolMenu:
                activate = [this, id, index, epoch]() { engine_->select_symbol(id, index, epoch); };
                break;
            case RenderTarget::MarkingHint:
                // The marking hint is informational: clicking it must not pick
                // a candidate behind the user's back.
                activate = []() {};
                break;
            case RenderTarget::Candidates:
            case RenderTarget::None:
                activate = [this, id, index]() { engine_->select_candidate(id, index); };
                break;
        }
        candidates->append<SelectableCandidateWord>(fcitx::Text(u16_to_utf8(candidate)), std::move(activate));
        ++index;
    }
    candidates->setPage(state.page);
    if (state.cursor_visible) candidates->setGlobalCursorIndex(state.cursor);
    panel.setCandidateList(std::move(candidates));
    input_context_ptr->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

HostContext ImeEngine::surrounding_text(ContextId context) {
    auto* input_context_ptr = input_context(context);
    HostContext result;
    if (input_context_ptr == nullptr) return result;

    const auto& surrounding = input_context_ptr->surroundingText();
    if (!surrounding.isValid()) return result;

    try {
        // fcitx5 reports cursor/anchor as scalar (code point) offsets; the
        // engine expects UTF-16 units.
        const std::string raw = surrounding.text();
        const auto scalars = utf8_to_u32(raw);
        const auto scalar_to_units = [&scalars](unsigned int offset) {
            const std::size_t bounded = std::min(static_cast<std::size_t>(offset), scalars.size());
            std::size_t units = 0;
            for (std::size_t i = 0; i < bounded; ++i) {
                units += scalars[i] > 0xFFFF ? 2 : 1;
            }
            return units;
        };
        result.text = utf8_to_u16(raw);
        result.cursor = scalar_to_units(surrounding.cursor());
        result.anchor = scalar_to_units(surrounding.anchor());
        result.valid = true;
    } catch (const std::runtime_error&) {
        // Ignore malformed surrounding text supplied by a client.
        return HostContext{};
    }
    return result;
}

bool ImeEngine::is_sensitive(ContextId context) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return true;
    return input_context_ptr->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive);
}

std::string ImeEngine::program(ContextId context) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return {};
    return input_context_ptr->program();
}

int ImeEngine::focused_probe_process(ContextId context) {
#if defined(__linux__)
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr && input_context_ptr->display().starts_with("x11:"))
        return focused_x11_pid();
#else
    (void)context;
#endif
    return 0;
}

std::vector<int> ImeEngine::probe_processes(ContextId context) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return {};
    const std::string program = input_context_ptr->program();
    if (program.empty()) return {};
#if defined(__linux__)
    // Focus is additional evidence, not a prerequisite. Wayland and some
    // clients do not expose a reliable window PID; keep scanning their
    // same-program processes and let the evidence for each address decay.
    const int focused_pid = input_context_ptr->display().starts_with("x11:")
                                ? focused_x11_pid() : 0;
#else
    return {};
#endif
#if defined(__linux__)

    const auto matches = [](std::string_view candidate, std::string_view needle) {
        if (candidate.empty() || needle.empty()) return false;
        std::string lowered;
        lowered.reserve(candidate.size());
        for (const char value : candidate) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
        }
        // Linux truncates comm to 15 characters. A short or unreadable comm
        // must never match an unrelated application by a loose prefix.
        if (lowered == needle) return true;
        if (lowered.size() == 15 && needle.starts_with(lowered)) return true;
        // Desktop entries often prefix the binary name: "google-chrome-stable"
        // runs as "chrome", "onlyoffice-desktopeditors" as "desktopeditors".
        // Compare whole dash-separated components, never loose substrings.
        std::size_t start = 0;
        while (start <= needle.size()) {
            const auto dash = needle.find('-', start);
            const auto piece = needle.substr(
                start, dash == std::string_view::npos ? std::string_view::npos : dash - start);
            if (piece == lowered) return true;
            if (dash == std::string_view::npos) break;
            start = dash + 1;
        }
        return false;
    };

    std::string needle;
    needle.reserve(program.size());
    for (const char value : program) {
        needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }

    std::vector<int> processes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) == 0) continue;
        const int pid = parse_decimal<int>(name, false).value_or(0);
        if (pid <= 1 || pid == static_cast<int>(::getpid())) continue;
        struct stat info {};
        if (::stat(entry.path().c_str(), &info) != 0 || info.st_uid != ::getuid()) continue;
        bool matched = false;
        {
            std::ifstream comm(entry.path() / "comm");
            std::string line;
            std::getline(comm, line);
            matched = matches(line, needle);
        }
        if (!matched) {
            std::error_code link_error;
            const auto executable = std::filesystem::read_symlink(entry.path() / "exe", link_error);
            if (!link_error) matched = matches(executable.filename().string(), needle);
        }
        if (!matched) continue;
        if (pid == focused_pid)
            processes.insert(processes.begin(), pid);
        else
            processes.push_back(pid);
        if (processes.size() >= 32 && focused_pid == 0) break;
    }
    if (focused_pid && !processes.empty() && processes.front() == focused_pid)
        LLAVON_DEBUG_LOG("MEMCTX-PID", "focused program=%s pid=%d", program.c_str(), focused_pid);
    if (processes.size() > 16) processes.resize(16);
    return processes;
#endif
}

fcitx::AddonInstance* ImeEngineFactory::create(fcitx::AddonManager* manager) {
    return new ImeEngine(manager ? manager->instance() : nullptr);
}

}  // namespace llavon::ime

FCITX_ADDON_FACTORY(llavon::ime::ImeEngineFactory)

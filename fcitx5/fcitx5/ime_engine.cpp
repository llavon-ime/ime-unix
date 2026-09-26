#include "fcitx5/ime_engine.hpp"

#include <fcitx-config/iniparser.h>
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

#include "config/config.hpp"
#include "host/render_state.hpp"
#include "text/utf.hpp"
#include "util/env.hpp"

extern char** environ;

namespace llavon::ime {

namespace {

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
    : instance_(instance), event_dispatcher_(instance ? &instance->eventDispatcher() : nullptr) {
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
        lora_manager_action_.setShortText("管理個人化訓練…");
        lora_manager_action_.setLongText("在瀏覽器開啟本機個人化訓練管理介面");
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
}

ImeEngine::~ImeEngine() {
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
    fcitx_config_ = ImeFcitxConfig();
    try {
        fcitx::readAsIni(fcitx_config_, kFcitxConfigFile);
    } catch (...) {
        fcitx_config_ = ImeFcitxConfig();
    }

    std::error_code ec;
    const bool has_fcitx_config = std::filesystem::exists(config_path(), ec) && !ec;
    apply_shared_config(fcitx_config_, load_config());
    if (!has_fcitx_config) save();
    const auto transport = default_transport_options();
    if (transport.model_path != active_service_model_path_) {
        // Restart the prediction service with the newly selected model. The
        // engine is kept: rebuilding it would tear down the accessibility
        // backend, and libatspi cannot be re-initialized in the same process.
        engine_->set_transport_options(transport);
        active_service_model_path_ = transport.model_path;
    }
    engine_->set_config(to_shared_config(fcitx_config_), false);
    engine_->reload_phrase_overrides();
    update_accessibility_status();
}

void ImeEngine::save() {
    // The default INI location is PkgConfig, matching config_path().
    // The accessibility status is informational and must not be persisted.
    const std::string status = *fcitx_config_.accessibilityStatus;
    (void)fcitx_config_.accessibilityStatus.setValue(std::string());
    fcitx::safeSaveAsIni(fcitx_config_, kFcitxConfigFile);
    (void)fcitx_config_.accessibilityStatus.setValue(status);
}

const fcitx::Configuration* ImeEngine::getConfig() const {
    return &fcitx_config_;
}

void ImeEngine::refresh_phrase_override_editor() const {
    auto* entries = phrase_override_editor_.entries.mutableValue();
    entries->clear();
    for (const auto& record : engine_->phrase_overrides().entries()) {
        PunctuationMapEntryConfig entry;
        (void)entry.phrase.setValue(u16_to_utf8(record.phrase));
        (void)entry.readings.setValue(PhraseOverrideStore::format_readings(record.readings));
        entries->emplace_back(std::move(entry));
    }
}

const fcitx::Configuration* ImeEngine::getSubConfig(const std::string& path) const {
    if (path != "phraseoverrides") return nullptr;

    refresh_phrase_override_editor();
    return &phrase_override_editor_;
}

void ImeEngine::setSubConfig(const std::string& path, const fcitx::RawConfig& config) {
    if (path != "phraseoverrides") return;
    // An absent Entries list is not authoritative: frontends also send empty
    // configs as action triggers, and silently wiping every saved phrase would
    // be unrecoverable. Removing all entries is done by editing the file.
    if (!config.get("Entries")) return;

    PhraseOverrideEditorConfig editor;
    editor.load(config, true);
    std::vector<PhraseOverrideRecord> records;
    records.reserve(editor.entries->size());
    for (const auto& entry : *editor.entries) {
        // The dialog has no error channel, so one malformed row must not
        // discard the edits the user made to every other row. Reusing the file
        // parser keeps the accepted readings identical to the on-disk format.
        const auto record = PhraseOverrideStore::parse_line(*entry.phrase + " " + *entry.readings);
        if (!record || !PhraseOverrideStore::valid_entry(record->phrase, record->readings.size())) continue;
        records.push_back(*record);
    }
    (void)engine_->phrase_overrides().replace(records);
}

void ImeEngine::setConfig(const fcitx::RawConfig& config) {
    fcitx_config_.load(config, true);
    (void)fcitx_config_.version.setValue(DisplayVersion::Current);
    engine_->set_config(to_shared_config(fcitx_config_));
    update_accessibility_status();
    save();
}

void ImeEngine::update_accessibility_status() {
#ifdef LLAVON_IME_NATIVE_SURROUNDING
    const std::string status = "InputMethodKit: 可取得（不需輔助使用權限）";
#else
    std::string status = accessibility_status_text(engine_->accessibility_state());
    const auto memory = engine_->memory_context_state();
    if (memory.availability != AccessibilityAvailability::Unsupported) {
        status += "；記憶體取樣: " + memory_status_text(memory);
    }
#endif
    if (*fcitx_config_.accessibilityStatus == status) return;
    (void)fcitx_config_.accessibilityStatus.setValue(status);
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
    if (state.cursor_visible) candidates->setCursorIndex(state.cursor);
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

std::vector<int> ImeEngine::probe_processes(ContextId context) {
    auto* input_context_ptr = input_context(context);
    if (input_context_ptr == nullptr) return {};
    const std::string program = input_context_ptr->program();
    if (program.empty()) return {};

    const auto matches = [](std::string_view candidate, std::string_view needle) {
        std::string lowered;
        lowered.reserve(candidate.size());
        for (const char value : candidate) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
        }
        // Linux truncates comm to 15 characters, so accept either direction of
        // the prefix relationship.
        return lowered == needle || lowered.starts_with(needle) || needle.starts_with(lowered);
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
        const int pid = std::atoi(name.c_str());
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
        processes.push_back(pid);
        if (processes.size() >= 32) break;
    }
    return processes;
}

fcitx::AddonInstance* ImeEngineFactory::create(fcitx::AddonManager* manager) {
    return new ImeEngine(manager ? manager->instance() : nullptr);
}

}  // namespace llavon::ime

FCITX_ADDON_FACTORY(llavon::ime::ImeEngineFactory)

#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/action.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "fcitx5/ime_config.hpp"
#include "fcitx5/input_context_property.hpp"
#include "host/engine.hpp"
#include "host/host.hpp"

namespace fcitx {
class EventDispatcher;
class InputContext;
}  // namespace fcitx

namespace llavon::ime {

// Fcitx5 host adapter: it converts fcitx key events into engine keys, exposes
// the engine's render states through the fcitx input panel, and implements the
// Host callbacks (commit, surrounding text, sensitivity). It holds no input
// method logic of its own.
class ImeEngine final : public fcitx::InputMethodEngineV2, public Host {
public:
    explicit ImeEngine(fcitx::Instance* instance);
    ~ImeEngine() override;

    void keyEvent(const fcitx::InputMethodEntry& entry, fcitx::KeyEvent& event) override;
    void activate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void deactivate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reset(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override;
    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration* getConfig() const override;
    void setConfig(const fcitx::RawConfig& config) override;
    const fcitx::Configuration* getSubConfig(const std::string& path) const override;
    void setSubConfig(const std::string& path, const fcitx::RawConfig& config) override;

    // Host interface.
    void post(std::function<void()> body) override;
    void commit(ContextId context, std::u16string_view text) override;
    void update_ui(ContextId context) override;
    HostContext surrounding_text(ContextId context) override;
    bool is_sensitive(ContextId context) override;
    std::vector<int> probe_processes(ContextId context) override;
    std::string program(ContextId context) override;

private:
    ImeInputContextProperty* property(fcitx::InputContext* input_context) const;
    ContextId context_id(fcitx::InputContext* input_context);
    fcitx::InputContext* input_context(ContextId context) const;
    void reload_config();
    void update_accessibility_status();
    void refresh_phrase_override_editor() const;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<Engine> engine_;
    std::filesystem::path active_service_model_path_;
    ImeFcitxConfig fcitx_config_;
    // Refreshed in place so a pointer handed to a config frontend stays valid.
    mutable PhraseOverrideEditorConfig phrase_override_editor_;
    fcitx::Instance* instance_ = nullptr;
    fcitx::EventDispatcher* event_dispatcher_ = nullptr;
    std::atomic<std::uint64_t> next_context_id_{1};
    std::unordered_map<ContextId, fcitx::TrackableObjectReference<fcitx::InputContext>> contexts_;
    ImeInputContextPropertyFactory property_factory_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> capability_changed_handler_;
    fcitx::SimpleAction lora_manager_action_;
    fcitx::ScopedConnection lora_manager_connection_;
};

class ImeEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override;
};

}  // namespace llavon::ime

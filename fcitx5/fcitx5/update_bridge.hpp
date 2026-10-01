#pragma once

#include <fcitx-utils/dbus/objectvtable.h>
#include <functional>
#include <string>

namespace llavon::ime {
// Runs on Fcitx's event loop. Checking and requesting restart are one method,
// so a key cannot arrive between a remote readiness query and the restart.
class UpdateBridge final : public fcitx::dbus::ObjectVTable<UpdateBridge> {
public:
    UpdateBridge(std::function<bool()> restart, std::string version, std::function<std::string()> status = {})
        : restart_(std::move(restart)), version_(std::move(version)), status_(std::move(status)) {}
    bool restartIfIdle() { return restart_(); }
    std::string version() const { return version_; }
    std::string status() const { return status_ ? status_() : "{}"; }
    FCITX_OBJECT_VTABLE_METHOD(restartIfIdle, "RestartIfIdle", "", "b");
    FCITX_OBJECT_VTABLE_METHOD(version, "Version", "", "s");
    FCITX_OBJECT_VTABLE_METHOD(status, "Status", "", "s");
private:
    std::function<bool()> restart_;
    std::string version_;
    std::function<std::string()> status_;
};
} // namespace llavon::ime

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace ime::unix_service {

// Owns one child and its process group. Model/database policy stays in the
// manager; argv never contains a training password. Destruction always reaps.
class JobProcess final {
public:
    JobProcess() = default;
    ~JobProcess();
    JobProcess(const JobProcess&) = delete;
    JobProcess& operator=(const JobProcess&) = delete;

    void launch(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                const std::filesystem::path& log, const std::string& password = {},
                const std::optional<std::filesystem::path>& trainer = std::nullopt);
    bool running() const noexcept { return pid_ > 0; }
    bool cancelling() const noexcept { return cancelling_; }
    void cancel();
    // nullopt while running, otherwise successful/failed completion.
    std::optional<bool> poll();
    void terminate() noexcept;

private:
    void signal_group(int signal) const noexcept;
    pid_t pid_ = -1;
    bool cancelling_ = false;
    std::chrono::steady_clock::time_point cancel_started_{};
};

}  // namespace ime::unix_service

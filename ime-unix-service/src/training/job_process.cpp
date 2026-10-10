#include "job_process.hpp"
#include "../../../engine/src/util/unique_fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sodium.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ime::unix_service {
using llavon::ime::UniqueFd;

JobProcess::~JobProcess() { terminate(); }

void JobProcess::launch(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                        const std::filesystem::path& log, const std::string& password,
                        const std::optional<std::filesystem::path>& trainer) {
    if (running()) throw std::runtime_error("已有工作進行中");
    if (password.size() > 4096) throw std::runtime_error("training password is too long");
    // Prepare all allocations before fork. The child only performs libc calls.
    std::vector<std::string> strings{executable.string()};
    strings.insert(strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& item : strings) argv.push_back(item.data());
    argv.push_back(nullptr);
    const auto trainer_name = trainer ? trainer->string() : std::string{};
    const UniqueFd output(::open(log.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!output.valid()) throw std::runtime_error("cannot open job log");
    int pair[2] = {-1, -1};
    if (!password.empty() && ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        throw std::runtime_error("cannot pass the training password");
    const UniqueFd reader(pair[0]), writer(pair[1]);
#ifdef SO_NOSIGPIPE
    if (writer.valid()) {
        const int enabled = 1;
        if (::setsockopt(writer.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
            throw std::runtime_error("cannot protect password channel");
    }
#endif
    pid_ = ::fork();
    if (pid_ < 0) throw std::runtime_error("cannot start CLI");
    if (pid_ == 0) {
        if (::setsid() < 0) _exit(127);
        if (reader.valid()) {
            // Redirect output first: its descriptor might itself be 3.
            if (::dup2(output.get(), STDOUT_FILENO) < 0 || ::dup2(output.get(), STDERR_FILENO) < 0) _exit(127);
            if (output.get() != 3) ::close(output.get());
            if (::dup2(reader.get(), 3) < 0) _exit(127);
            if (reader.get() != 3) ::close(reader.get());
            if (writer.get() != 3) ::close(writer.get());
            if (::fcntl(3, F_SETFD, 0) < 0) _exit(127);
        } else {
            if (::dup2(output.get(), STDOUT_FILENO) < 0 || ::dup2(output.get(), STDERR_FILENO) < 0) _exit(127);
            ::close(output.get());
        }
        if (trainer && ::setenv("LLAVON_IME_LORA_CLI_PATH", trainer_name.c_str(), 1) != 0) _exit(127);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    cancelling_ = false;
    if (writer.valid()) {
        std::string line = password + "\n";
        std::size_t offset = 0;
        while (offset < line.size()) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            const auto count = ::send(writer.get(), line.data() + offset, line.size() - offset, flags);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        sodium_memzero(line.data(), line.size());
        if (offset != line.size()) { terminate(); throw std::runtime_error("cannot pass the training password"); }
    }
}

void JobProcess::signal_group(int signal) const noexcept {
    if (!running()) return;
    // The owned child can be cancelled before setsid has run. Never signal
    // the parent's process group; fall back to the positive child PID.
    if (::kill(-pid_, signal) != 0 && errno == ESRCH) (void)::kill(pid_, signal);
}

void JobProcess::cancel() {
    if (!running()) throw std::runtime_error("沒有執行中的工作");
    if (!cancelling_) {
        cancelling_ = true;
        cancel_started_ = std::chrono::steady_clock::now();
        signal_group(SIGTERM);
    }
}

std::optional<bool> JobProcess::poll() {
    if (!running()) return std::nullopt;
    if (cancelling_ && std::chrono::steady_clock::now() - cancel_started_ >= std::chrono::seconds(5))
        signal_group(SIGKILL);
    int status = 0;
    const auto ended = ::waitpid(pid_, &status, WNOHANG);
    if (ended == 0 || (ended < 0 && errno == EINTR)) return std::nullopt;
    pid_ = -1;
    return !cancelling_ && ended > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void JobProcess::terminate() noexcept {
    if (!running()) return;
    signal_group(SIGTERM);
    for (int i = 0; i < 25 && running(); ++i) {
        (void)poll();
        if (running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!running()) return;
    signal_group(SIGKILL);
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
}

}  // namespace ime::unix_service

#include "context/memory_context.hpp"

#include "debug/debug_log.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#endif

namespace llavon::ime {
namespace {

constexpr std::size_t kAnchorUnits = 48;
constexpr auto kFailurePause = std::chrono::seconds(2);
constexpr std::size_t kMaxConsecutiveFailures = 3;

#if defined(__linux__)
// One child per provider, owned by the IME. Only explicit same-UID target PIDs
// are passed to it; it has no listener or shared/global service endpoint.
class HelperProcess {
public:
    explicit HelperProcess(std::filesystem::path path) : path_(std::move(path)) {}
    ~HelperProcess() { close(); }

    std::optional<nlohmann::json> request(const nlohmann::json& input) {
        if (pid_ <= 0 && !launch()) return std::nullopt;
        const std::string line = input.dump() + '\n';
        std::size_t offset = 0;
        while (offset < line.size()) {
            pollfd descriptor{to_child_, POLLOUT, 0};
            if (::poll(&descriptor, 1, 300) <= 0 || !(descriptor.revents & POLLOUT)) {
                close();
                return std::nullopt;
            }
            const ssize_t count = ::write(to_child_, line.data() + offset, line.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                close();
                return std::nullopt;
            }
            offset += static_cast<std::size_t>(count);
        }
        std::string output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1750);
        while (output.size() < 65536 && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd descriptor{from_child_, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || !(descriptor.revents & POLLIN)) break;
            char buffer[4096];
            const ssize_t count = ::read(from_child_, buffer, sizeof(buffer));
            if (count <= 0) break;
            output.append(buffer, static_cast<std::size_t>(count));
            if (const auto newline = output.find('\n'); newline != std::string::npos) {
                auto parsed = nlohmann::json::parse(output.substr(0, newline), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object()) return parsed;
                break;
            }
        }
        close();
        return std::nullopt;
    }

    void close() {
        if (to_child_ >= 0) ::close(to_child_);
        if (from_child_ >= 0) ::close(from_child_);
        to_child_ = from_child_ = -1;
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
    }

private:
    bool launch() {
        int input[2], output[2];
        if (::pipe(input) != 0) return false;
        if (::pipe(output) != 0) {
            ::close(input[0]);
            ::close(input[1]);
            return false;
        }
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(input[1]);
            ::close(output[0]);
            if (::dup2(input[0], STDIN_FILENO) < 0 ||
                ::dup2(output[1], STDOUT_FILENO) < 0) ::_exit(127);
            ::close(input[0]);
            ::close(output[1]);
            ::execl(path_.c_str(), path_.c_str(), "--serve", nullptr);
            ::_exit(127);
        }
        ::close(input[0]);
        ::close(output[1]);
        if (child < 0) {
            ::close(input[1]);
            ::close(output[0]);
            return false;
        }
        pid_ = child;
        to_child_ = input[1];
        from_child_ = output[0];
        return true;
    }

    std::filesystem::path path_;
    pid_t pid_ = -1;
    int to_child_ = -1;
    int from_child_ = -1;
};
#endif

}  // namespace

class MemoryContextProvider::Impl {
public:
    struct Hooks {
        std::function<void(std::u16string, bool)> publish;
        std::function<void(AccessibilityAvailability, std::string)> availability;
        std::function<bool()> active;
        std::function<std::size_t()> max_code_units;
        std::function<std::uint64_t()> generation;
    };

    Impl(Hooks hooks, MemoryProbeCallbacks callbacks, std::filesystem::path path)
        : hooks_(std::move(hooks)), callbacks_(std::move(callbacks)), helper_path_(std::move(path)) {}
    ~Impl() { stop(); }

    bool start() {
#if defined(__linux__)
        std::error_code error;
        if (helper_path_.empty() || !std::filesystem::is_regular_file(helper_path_, error)) {
            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-missing");
            return false;
        }
        const auto permissions = std::filesystem::status(helper_path_, error).permissions();
        if (error || (permissions & std::filesystem::perms::owner_exec) ==
                         std::filesystem::perms::none) {
            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-not-executable");
            return false;
        }
        if (!worker_.joinable()) {
            stopping_ = false;
            worker_ = std::thread([this] { worker_loop(); });
        }
        running_ = true;
        hooks_.availability(AccessibilityAvailability::Available, "memscan");
        return true;
#else
        hooks_.availability(AccessibilityAvailability::Unsupported, "no-backend");
        return false;
#endif
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_.reset();
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        running_ = false;
    }

    bool running() const noexcept { return running_; }

    void invalidate() {
        std::lock_guard lock(mutex_);
        ++revision_;
        pending_.reset();
        candidates_.clear();
        last_anchor_.clear();
        observed_anchor_.clear();
        failures_ = 0;
        hint_pid_ = 0;
        hint_address_ = 0;
        hint_before_.clear();
        last_probe_ = {};
        published_usable_ = false;
        unverified_ = 0;
        hooks_.publish({}, false);
    }

    void refresh() {
        if (!running_ || !hooks_.active()) return;
        if (callbacks_.sensitive && callbacks_.sensitive()) {
            invalidate();
            return;
        }
        const std::string preedit = callbacks_.preedit ? callbacks_.preedit() : std::string{};
        if (preedit.empty()) return;
        std::vector<int> pids = callbacks_.processes ? callbacks_.processes() : std::vector<int>{};
        if (pids.empty()) {
            LLAVON_DEBUG_LOG("MEMCTX", "skip: focused client has no named process");
            return;
        }
        if (pids.size() > 16) pids.resize(16);

        std::string anchor;
        try {
            anchor = u16_to_utf8(utf16_tail(utf8_to_u16(preedit), kAnchorUnits));
        } catch (const std::exception&) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        if (stopping_ || anchor == last_anchor_) return;
        if (failures_ >= kMaxConsecutiveFailures && now - last_probe_ < kFailurePause) return;
        last_probe_ = now;
        last_anchor_ = anchor;
        pending_ = Job{std::move(anchor), std::move(pids), hooks_.generation(), revision_};
        condition_.notify_one();
    }

    std::size_t probe_count() const noexcept { return probes_.load(); }

private:
    struct Candidate {
        int pid = 0;
        std::uintptr_t address = 0;
        std::string encoding;
        std::string before;
        unsigned observations = 1;
        bool operator==(const Candidate& other) const {
            return pid == other.pid && address == other.address && encoding == other.encoding;
        }
    };
    struct Job {
        std::string anchor;
        std::vector<int> pids;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
    };

    void worker_loop() {
#if defined(__linux__)
        // A crashed helper must report an error, not terminate fcitx5 via
        // SIGPIPE when its input pipe is written on the next request.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        HelperProcess helper(helper_path_);
#endif
        for (;;) {
            Job job;
            int hint_pid = 0;
            std::uintptr_t hint_address = 0;
            std::string hint_before;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_) return;
                job = std::move(*pending_);
                pending_.reset();
                hint_pid = hint_pid_;
                hint_address = hint_address_;
                hint_before = hint_before_;
            }
            ++probes_;
#if defined(__linux__)
            const auto result = helper.request({{"anchor", job.anchor}, {"pids", job.pids},
                                                {"epoch", job.revision},
                                                {"hint_pid", hint_pid},
                                                {"hint_address", hint_address},
                                                {"hint_before", hint_before}});
            const std::string error_code = result && result->contains("error") &&
                                                   (*result)["error"].is_string()
                                               ? (*result)["error"].get<std::string>()
                                               : "helper-failed";
            const auto matches = result && result->contains("matches") && (*result)["matches"].is_array()
                                     ? (*result)["matches"] : nlohmann::json::array();
            std::vector<Candidate> current;
            try {
                for (const auto& item : matches) {
                    Candidate candidate{item.at("pid").get<int>(),
                                        item.at("address").get<std::uintptr_t>(),
                                        item.at("encoding").get<std::string>(),
                                        item.at("before").get<std::string>()};
                    // A few printable bytes in heap metadata are not useful
                    // document context. Very short real documents safely
                    // fall back to the other context sources.
                    if (std::ranges::find(job.pids, candidate.pid) != job.pids.end() &&
                        candidate.address > 0 && utf8_to_u16(candidate.before).size() >= 8 &&
                        current.size() < 16) current.push_back(std::move(candidate));
                }
            } catch (const std::exception&) {
                current.clear();
            }
            std::optional<Candidate> verified;
            {
                std::lock_guard lock(mutex_);
                if (stopping_ || job.revision != revision_ ||
                    job.generation != hooks_.generation() || !hooks_.active()) continue;
                // Typing may outrun a cold scan. Keep its candidate evidence
                // for the next natural state, but never publish a result for
                // an already superseded preedit.
                const bool current_job = !pending_.has_value() && job.anchor == last_anchor_;
                const bool changed = job.anchor != observed_anchor_;
                observed_anchor_ = job.anchor;
                for (auto& candidate : current) {
                    // The preedit *changed* between requests, but the same
                    // location and its preceding text did not. An arbitrary
                    // copy of a common word is never enough to publish.
                    const auto previous = std::ranges::find(candidates_, candidate);
                    if (changed && previous != candidates_.end() &&
                        previous->before == candidate.before && !candidate.before.empty()) {
                        // Three different natural composition states (two
                        // transitions) are required to establish a location.
                        candidate.observations = std::min(3u, previous->observations + 1);
                    }
                    if (candidate.observations >= 3) {
                        verified = candidate;
                        break;
                    }
                }
                candidates_ = std::move(current);
                if (verified && current_job) {
                    hint_pid_ = verified->pid;
                    hint_address_ = verified->address;
                    hint_before_ = verified->before;
                    failures_ = 0;
                } else if (!candidates_.empty()) {
                    // A copy of the composition in a protocol/layout buffer
                    // is not a document location. Until the transitions
                    // confirm one address, do not let the first arbitrary
                    // match constrain the next search to its neighbourhood.
                    hint_pid_ = 0;
                    hint_address_ = 0;
                    hint_before_.clear();
                    failures_ = 0;
                } else if (current_job) {
                    hint_pid_ = 0;
                    hint_address_ = 0;
                    hint_before_.clear();
                    ++failures_;
                }
                if (verified && current_job) {
                    LLAVON_DEBUG_LOG("MEMCTX", "preedit=\"%s\" confirmed pid=%d address=0x%lx candidates=%zu",
                                     job.anchor.c_str(), verified->pid,
                                     static_cast<unsigned long>(verified->address), candidates_.size());
                    try {
                        hooks_.publish(utf16_tail(utf8_to_u16(verified->before), hooks_.max_code_units()), true);
                        published_usable_ = true;
                        unverified_ = 0;
                        hooks_.availability(AccessibilityAvailability::Available, "memscan");
                    } catch (const std::exception&) {
                        published_usable_ = false;
                        hooks_.publish({}, false);
                    }
                } else if (current_job) {
                    const auto progress = result && result->contains("progress")
                                              ? (*result)["progress"].dump() : std::string{};
                    LLAVON_DEBUG_LOG("MEMCTX", "preedit=\"%s\" unverified matches=%zu accepted=%zu error=%s progress=%s",
                                     job.anchor.c_str(), matches.size(), candidates_.size(),
                                     error_code.c_str(), progress.c_str());
                    // A miss on one preedit state (client redraw lag, scan
                    // budget) must not discard a location that natural preedit
                    // changes already confirmed. Document changes invalidate
                    // it explicitly; several unverified states in a row drop
                    // it as well, so a vanished location cannot linger.
                    if (published_usable_ && ++unverified_ >= kMaxConsecutiveFailures)
                        published_usable_ = false;
                    if (!published_usable_) {
                        hooks_.publish({}, false);
                        if (!result)
                            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-failed");
                        else if (error_code == "denied")
                            hooks_.availability(AccessibilityAvailability::Unavailable, "permission-denied");
                    }
                }
            }
#else
            (void)job;
#endif
        }
    }

    Hooks hooks_;
    MemoryProbeCallbacks callbacks_;
    std::filesystem::path helper_path_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Job> pending_;
    std::vector<Candidate> candidates_;
    std::string last_anchor_;
    std::string observed_anchor_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> probes_{0};
    bool stopping_ = false;
    std::size_t failures_ = 0;
    int hint_pid_ = 0;
    std::uintptr_t hint_address_ = 0;
    std::string hint_before_;
    bool published_usable_ = false;
    std::size_t unverified_ = 0;
    std::uint64_t revision_ = 0;
    std::chrono::steady_clock::time_point last_probe_{};
};

MemoryContextProvider::MemoryContextProvider(size_t max_code_units, MemoryProbeCallbacks callbacks,
                                             std::filesystem::path helper_path)
    : AccessibilityContextProvider(max_code_units) {
    Impl::Hooks hooks;
    hooks.publish = [this](std::u16string text, bool usable) { publish(std::move(text), usable); };
    hooks.availability = [this](AccessibilityAvailability value, std::string detail) {
        set_availability(value, std::move(detail));
    };
    hooks.active = [this] { return active(); };
    hooks.max_code_units = [this] { return AccessibilityContextProvider::max_code_units(); };
    hooks.generation = [this] { return activation_generation(); };
    impl_ = std::make_unique<Impl>(std::move(hooks), std::move(callbacks), std::move(helper_path));
}

MemoryContextProvider::~MemoryContextProvider() = default;
bool MemoryContextProvider::start() { return impl_->start(); }
void MemoryContextProvider::stop() { impl_->stop(); }
bool MemoryContextProvider::running() const noexcept { return impl_->running(); }
void MemoryContextProvider::refresh() { impl_->refresh(); }
void MemoryContextProvider::invalidate() { impl_->invalidate(); }
std::size_t MemoryContextProvider::probe_count() const noexcept { return impl_->probe_count(); }

bool memory_context_supported() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

}  // namespace llavon::ime

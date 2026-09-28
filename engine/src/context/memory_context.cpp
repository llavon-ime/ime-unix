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
// A missed scan is retried while the same preedit state is still current:
// once for a slow client redraw, plus further attempts for a scan that ran
// out of its byte or time budget and must resume.
constexpr int kScanAttempts = 4;

// Preference order for layouts that match at the same address: plain byte
// encodings before cell grids, denser cells before wider ones. A wider cell
// layout reading a denser grid sees every other character and can decode
// plausible text from it, so its score alone is not comparable.
int encoding_rank(std::string_view encoding) {
    if (encoding == "utf8") return 0;
    if (encoding == "utf16le") return 1;
    if (encoding == "utf32le") return 2;
    if (encoding == "utf32cell8le") return 3;
    if (encoding == "utf32cell12le") return 4;
    if (encoding == "utf32cell16le") return 5;
    if (encoding == "utf32cell20le") return 6;
    if (encoding == "utf32cell24le") return 7;
    return 8;
}

#if defined(__linux__)
// One child per provider, owned by the IME. Only explicit same-UID target PIDs
// are passed to it; it has no listener or shared/global service endpoint.
class HelperProcess {
public:
    explicit HelperProcess(std::filesystem::path path) : path_(std::move(path)) {}
    ~HelperProcess() { close(); }

    std::optional<nlohmann::json> request(const nlohmann::json& input) {
        // Reinstalling the helper must take effect without restarting the
        // input method: replace the child when its binary changed on disk.
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(path_, mtime_error);
        if (pid_ > 0 && !mtime_error && mtime != launched_mtime_) close();
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
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
        std::error_code mtime_error;
        launched_mtime_ = std::filesystem::last_write_time(path_, mtime_error);
        return true;
    }

    std::filesystem::path path_;
    std::filesystem::file_time_type launched_mtime_{};
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

    void invalidate(bool context_continues = false) {
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
        unverified_ = 0;
        misses_ = 0;
        focus_misses_ = 0;
        // A commit moves the caret. Keep the old *tail* only as verification
        // evidence; the previous sample must not be adopted for the new caret
        // while the client has not been scanned again.
        published_usable_ = false;
        hooks_.publish({}, false);
        if (!context_continues) {
            // A forwarded edit or focus change can replace the document.
            last_context_.clear();
            last_published_commit_.clear();
            last_commit_history_.clear();
        }
    }

    void prime() {
        if (!running_ || !hooks_.active()) return;
        std::vector<int> pids = callbacks_.processes ? callbacks_.processes() : std::vector<int>{};
        if (pids.empty()) return;
        if (pids.size() > 16) pids.resize(16);
        const std::string program = callbacks_.program ? callbacks_.program() : std::string{};
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        pending_ = Job{{}, {}, std::move(pids), hooks_.generation(), revision_, true, false, program, {}, 0};
        condition_.notify_one();
    }

    void refresh() {
        if (!running_ || !hooks_.active()) return;
        if (callbacks_.sensitive && callbacks_.sensitive()) {
            invalidate();
            return;
        }
        const std::string preedit = callbacks_.preedit ? callbacks_.preedit() : std::string{};
        std::vector<int> pids = callbacks_.processes ? callbacks_.processes() : std::vector<int>{};
        if (pids.empty()) {
            LLAVON_DEBUG_LOG("MEMCTX", "skip: focused client has no named process");
            return;
        }
        if (pids.size() > 16) pids.resize(16);
        const std::string program = callbacks_.program ? callbacks_.program() : std::string{};

        // The composition and the text committed just before it are searched
        // together: clients that never draw the composition into their
        // document (Konsole, VTE) still write the committed text there, and a
        // location that both agree on is the caret.
        std::vector<std::string> anchors;
        std::string committed_anchor;
        const auto add_anchor = [&](const std::string& text, bool committed) {
            if (text.empty()) return;
            try {
                const auto tail = u16_to_utf8(utf16_tail(utf8_to_u16(text), kAnchorUnits));
                if (tail.empty()) return;
                if (committed) committed_anchor = tail;
                if (std::ranges::find(anchors, tail) == anchors.end()) anchors.push_back(tail);
            } catch (const std::exception&) {
            }
        };
        add_anchor(preedit, false);
        std::string history;
        if (callbacks_.commit_history) history = callbacks_.commit_history();
        add_anchor(history, true);
        if (anchors.empty()) return;
        const std::string preedit_prefix = callbacks_.preedit_prefix ? callbacks_.preedit_prefix() : std::string{};
        const int focus = callbacks_.focused_process ? callbacks_.focused_process() : 0;
        const int preferred_pid = std::ranges::find(pids, focus) == pids.end() ? 0 : focus;
        const std::string key = Job::make_key(anchors) + '\x1e' + preedit_prefix + '\x1e' +
                                std::to_string(preferred_pid);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        if (stopping_ || key == last_anchor_) return;
        if (failures_ >= kMaxConsecutiveFailures && now - last_probe_ < kFailurePause) return;
        last_probe_ = now;
        last_anchor_ = key;
        // A new commit means the client just wrote text the probe has never
        // seen; the dirty baseline may already cover it, so read everything
        // once and fall back to the dirty set afterwards.
        const bool committed = !history.empty() && history != last_commit_history_;
        last_commit_history_ = history;
        // A pending job that has not run yet must not lose the full scan a
        // commit asked for: typing can replace it before the worker picks it up.
        const bool carry_full = pending_.has_value() && pending_->full_scan;
        pending_ = Job{std::move(anchors), std::move(committed_anchor), std::move(pids),
                       hooks_.generation(), revision_, false, committed || carry_full, program,
                        preedit_prefix, preferred_pid};
        condition_.notify_one();
    }

    std::size_t probe_count() const noexcept { return probes_.load(); }

    // True while a location is being verified or already confirmed, so a
    // rendered-only composition may still be tracked.
    bool tracking() const {
        std::lock_guard lock(mutex_);
        return !candidates_.empty() || hint_pid_ != 0;
    }

private:
    struct Candidate {
        int pid = 0;
        std::uintptr_t address = 0;
        std::string encoding;
        std::string before;
        // The text after the composition. A hit whose following text starts
        // as the same word may be a static string prefix; it is only trusted
        // when this text stays the same across composition states.
        std::string after;
        bool continuation = false;
        int confidence = 0;
        // Index of the anchor that produced this match. Anchor 0 is the
        // composition; later anchors are the recently committed text, which
        // sits between the match and the caret.
        std::size_t anchor_index = 0;
        // True when the match came from the committed-text anchor (or from the
        // single deduplicated anchor whose text is the committed one).
        bool committed = false;
        unsigned observations = 1;
        // States in which the location was not seen again. Clients that keep
        // two copies of the composition (double buffering) alternate between
        // them, so a candidate has to survive a few states to be verified.
        unsigned carried = 0;
        bool operator==(const Candidate& other) const {
            return pid == other.pid && address == other.address && encoding == other.encoding;
        }
    };
    struct Job {
        // Anchors to search for, most important first: the composition on
        // screen, then the text committed just before it. Both are scanned in
        // one request over the same pages.
        std::vector<std::string> anchors;
        // The committed anchor among them, empty when there is none. A match
        // in front of the caret has this text between it and the caret.
        std::string committed_anchor;
        std::vector<int> pids;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        bool prime = false;
        // The first scan after a commit reads the whole address space: the
        // committed text may have been written before the dirty baseline was
        // reset, so a changed-pages scan can miss it. Later states reuse the
        // dirty set and the verified hint.
        bool full_scan = false;
        std::string program;
        std::string preedit_prefix;
        int preferred_pid = 0;
        // Stable identity of the anchor set, for state deduplication.
        static std::string make_key(const std::vector<std::string>& list) {
            std::string joined;
            for (const auto& anchor : list) {
                joined += anchor;
                joined.push_back('\x1f');
            }
            return joined;
        }
        std::string key() const {
            return make_key(anchors) + '\x1e' + preedit_prefix + '\x1e' +
                   std::to_string(preferred_pid);
        }
        const std::string& primary() const {
            static const std::string empty;
            return anchors.empty() ? empty : anchors.front();
        }
    };

    // The best committed-text candidate among everything seen so far. Its
    // text never changes between composition states, so a hint pointing at it
    // stays meaningful while its pages are no longer written.
    const Candidate* best_committed_candidate() const {
        const Candidate* best = nullptr;
        for (const auto& candidate : candidates_) {
            if (!candidate.committed) continue;
            if (best == nullptr || candidate.confidence > best->confidence) best = &candidate;
        }
        return best;
    }

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
#if defined(__linux__)
            if (job.prime) {
                // Establish the soft-dirty baseline; no anchor is scanned yet.
                helper.request({{"prime", true}, {"anchor", ""}, {"pids", job.pids}});
                continue;
            }
#endif
#if defined(__linux__)
            std::optional<nlohmann::json> result;
            std::string error_code;
            for (int attempt = 0; attempt < kScanAttempts; ++attempt) {
                result = helper.request({{"anchor", job.primary()}, {"anchors", job.anchors},
                                         {"full", job.full_scan}, {"pids", job.pids},
                                         {"program", job.program},
                                         {"epoch", job.revision},
                                         {"hint_pid", hint_pid},
                                         {"hint_address", hint_address},
                                         {"hint_before", hint_before}});
                error_code = result && result->contains("error") && (*result)["error"].is_string()
                                 ? (*result)["error"].get<std::string>()
                                 : "helper-failed";
                const bool transient = error_code == "not-found" || error_code == "timeout" ||
                                       error_code == "budget";
                if (!transient || attempt + 1 >= kScanAttempts) break;
                // The client may not have painted this preedit state yet, so
                // keep retrying while the state is still current instead of
                // giving up after one miss or depending on a fixed delay. A
                // timeout/budget retry also resumes where the last scan
                // stopped, because the helper keeps its cursor per PID.
                std::unique_lock lock(mutex_);
                if (stopping_ || job.revision != revision_ || job.key() != last_anchor_ ||
                    pending_.has_value()) {
                    break;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            const auto matches = result && result->contains("matches") && (*result)["matches"].is_array()
                                     ? (*result)["matches"] : nlohmann::json::array();
            std::vector<Candidate> current;
            try {
                for (const auto& item : matches) {
                    Candidate candidate{item.at("pid").get<int>(),
                                        item.at("address").get<std::uintptr_t>(),
                                        item.at("encoding").get<std::string>(),
                                        item.at("before").get<std::string>(),
                                        item.value("after", std::string{}),
                                        item.value("continuation", false),
                                        item.value("confidence", 0),
                                        item.value("anchor_index", std::size_t{0})};
                    candidate.committed =
                        candidate.anchor_index < job.anchors.size() &&
                        job.anchors[candidate.anchor_index] == job.committed_anchor;
                    // A few printable bytes in heap metadata are not useful
                    // document context. Very short real documents safely
                    // fall back to the other context sources.
                    if (std::ranges::find(job.pids, candidate.pid) != job.pids.end() &&
                        candidate.address > 0 && utf8_to_u16(candidate.before).size() >= 8 &&
                        current.size() < 32) current.push_back(std::move(candidate));
                }
            } catch (const std::exception&) {
                current.clear();
            }
            std::optional<Candidate> verified;
            std::u16string verified_context;
            bool notify_published = false;
            {
                std::lock_guard lock(mutex_);
                if (stopping_ || job.revision != revision_ ||
                    job.generation != hooks_.generation() || !hooks_.active()) continue;
                // Typing may outrun a cold scan. Keep its candidate evidence
                // for the next natural state, but never publish a result for
                // an already superseded preedit.
                const bool current_job = !pending_.has_value() && job.key() == last_anchor_;
                const bool changed = job.key() != observed_anchor_;
                observed_anchor_ = job.key();
                const bool focused_hit = job.preferred_pid &&
                    std::ranges::any_of(current, [&](const Candidate& candidate) {
                        return candidate.pid == job.preferred_pid;
                    });
                if (focused_hit) focus_misses_ = 0;
                else if (job.preferred_pid) ++focus_misses_;
                int verified_score = -1;
                for (auto& candidate : current) {
                    // The preedit *changed* between requests, but the same
                    // location and its preceding text did not. An arbitrary
                    // copy of a common word is never enough to publish.
                    const auto previous = std::ranges::find(candidates_, candidate);
                    // A location whose hit continues as a longer word may be a
                    // static string prefix. A static string's tail shrinks as
                    // the composition grows, while a real caret's following
                    // text stays the same, so require that text to match too.
                    bool after_stable = true;
                    if (previous != candidates_.end() &&
                        (candidate.continuation || previous->continuation)) {
                        after_stable = previous->after == candidate.after;
                    }
                    if (changed && previous != candidates_.end() &&
                        previous->before == candidate.before && after_stable &&
                        !candidate.before.empty()) {
                        // Every further natural composition state at the same
                        // location raises the observation count; three (two
                        // transitions) are required to establish it.
                        candidate.observations = std::min(8u, previous->observations + 1);
                    }
                    if (candidate.observations >= 3) {
                        // A stable copy in a different window must not win
                        // while the actual focused process is still producing
                        // candidates. If it never yields a caret, three misses
                        // let the search fall through to other processes.
                        if (job.preferred_pid && candidate.pid != job.preferred_pid &&
                            (focused_hit || focus_misses_ < 3)) continue;
                        // Evaluate every confirmed location before ranking
                        // them. The top-scoring old row may be stale while a
                        // different location in this scan is current.
                        std::string context = candidate.before;
                        // An editor may draw the entire uncommitted preedit
                        // into the document buffer. The anchor is only the
                        // last segment; earlier segments can therefore sit
                        // immediately before it but must remain in padding,
                        // not be mistaken for committed document context.
                        if (!candidate.committed && !job.preedit_prefix.empty() &&
                            context.ends_with(job.preedit_prefix)) {
                            context.resize(context.size() - job.preedit_prefix.size());
                        }
                        if (candidate.committed && !job.committed_anchor.empty() &&
                            !context.ends_with(job.committed_anchor)) {
                            context += job.committed_anchor;
                        }
                        std::u16string published;
                        try {
                            published = utf16_tail(utf8_to_u16(context), hooks_.max_code_units());
                        } catch (const std::exception&) {
                            continue;
                        }
                        if (published.empty()) continue;
                        // A fixed-size tail shifts forward after a commit;
                        // comparing it as a prefix of the old tail is wrong.
                        bool stale = false;
                        if (!last_context_.empty()) {
                            if (job.committed_anchor == last_published_commit_) {
                                stale = !published.ends_with(last_context_);
                            } else if (job.committed_anchor.starts_with(last_published_commit_)) {
                                const std::string delta =
                                    job.committed_anchor.substr(last_published_commit_.size());
                                try {
                                    const auto expected = utf16_tail(last_context_ + utf8_to_u16(delta),
                                                                     hooks_.max_code_units());
                                    stale = !published.ends_with(expected);
                                } catch (const std::exception&) {
                                    stale = true;
                                }
                            }
                        }
                        if (stale) continue;
                        int score = candidate.confidence * 4 +
                                     static_cast<int>(std::min(4u, candidate.observations)) * 8;
                        if (job.preferred_pid && candidate.pid == job.preferred_pid) score += 100;
                        // A terminal displaying our diagnostics can contain
                        // an exact, naturally changing copy of every preedit.
                        // It is weaker evidence of the *document caret* than
                        // text from the application's actual edit buffer.
                        const bool diagnostic_copy =
                            candidate.before.find("[MEMCTX] preedit=") != std::string::npos ||
                            candidate.before.find("[MEMCTX-CAND] preedit=") != std::string::npos ||
                            candidate.before.find("[CTX] source=") != std::string::npos;
                        if (diagnostic_copy) score -= 500;
                        // A strong hit sits at a word boundary like a caret
                        // does; a continuation hit may be a static prefix, so
                        // it needs a clear lead to win over a strong one.
                        if (candidate.continuation) score -= 32;
                        if (score > verified_score) {
                            verified_score = score;
                            verified = candidate;
                            verified_context = std::move(published);
                        }
                    }
                }
                if (current.empty()) {
                    // A missed state is negative evidence, not another vote
                    // for a once-confirmed address. Keep it briefly for redraw
                    // lag, but reduce its confidence before it can win again.
                    if (++misses_ >= kMaxConsecutiveFailures) {
                        candidates_.clear();
                    } else {
                        for (auto& previous : candidates_) {
                            ++previous.carried;
                            previous.observations = previous.observations > 2
                                                        ? previous.observations - 2 : 0;
                            previous.confidence = std::max(0, previous.confidence - 20);
                        }
                    }
                } else {
                    misses_ = 0;
                    // Keep locations seen in earlier states: a client can
                    // alternate between two copies of the composition, and
                    // each copy is verified on its own. A location that stops
                    // appearing is dropped after a few states.
                    std::vector<Candidate> merged;
                    merged.reserve(current.size() + candidates_.size());
                    for (auto& candidate : current) {
                        candidate.carried = 0;
                        merged.push_back(std::move(candidate));
                    }
                    for (auto& previous : candidates_) {
                        if (std::ranges::any_of(merged, [&](const Candidate& candidate) {
                                return candidate == previous;
                            })) {
                            continue;
                        }
                        if (++previous.carried >= kMaxConsecutiveFailures) continue;
                        previous.observations = previous.observations > 2
                                                    ? previous.observations - 2 : 0;
                        previous.confidence = std::max(0, previous.confidence - 20);
                        if (merged.size() < 32) merged.push_back(std::move(previous));
                    }
                    // The same address can satisfy several cell layouts at
                    // once (an 8-byte grid read as 16-byte cells sees every
                    // other character, which still decodes as plausible
                    // text). A wider layout is only trusted when its text
                    // quality is clearly better; on a near-tie the denser,
                    // plainer layout is the real one.
                    std::vector<Candidate> unique;
                    unique.reserve(merged.size());
                    for (auto& candidate : merged) {
                        const auto existing = std::ranges::find_if(unique, [&](const Candidate& other) {
                            return other.pid == candidate.pid && other.address == candidate.address;
                        });
                        if (existing == unique.end()) {
                            unique.push_back(std::move(candidate));
                            continue;
                        }
                        const bool clearly_better =
                            candidate.confidence > existing->confidence + 8;
                        const bool near_tie_better =
                            candidate.confidence + 8 >= existing->confidence &&
                            encoding_rank(candidate.encoding) < encoding_rank(existing->encoding);
                        if (clearly_better || near_tie_better) *existing = std::move(candidate);
                    }
                    candidates_ = std::move(unique);
                }
                if (verified && current_job) {
                    hint_pid_ = verified->pid;
                    hint_address_ = verified->address;
                    hint_before_ = verified->before;
                    failures_ = 0;
                } else if (const auto* best = best_committed_candidate(); best != nullptr) {
                    // Keep the next scan pointed at the best committed-text
                    // candidate even before it is verified: that text never
                    // changes between states and its pages may already be
                    // clean, so the following states have to find it again to
                    // accumulate observations. Candidates of the composition
                    // itself are left to the changed-pages scan, which finds
                    // the rewritten text on its own.
                    hint_pid_ = best->pid;
                    hint_address_ = best->address;
                    hint_before_ = best->before;
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
#ifdef LLAVON_IME_DEBUG
                if (current_job) {
                    for (const auto& candidate : candidates_) {
                        const char* tail = candidate.before.c_str() +
                                           (candidate.before.size() > 60
                                                ? candidate.before.size() - 60
                                                : 0);
                        LLAVON_DEBUG_LOG("MEMCTX-CAND",
                                         "preedit=\"%s\" pid=%d encoding=%s address=0x%lx observations=%u confidence=%d before_units=%zu before_tail=\"%s\"",
                                          job.primary().c_str(), candidate.pid,
                                         candidate.encoding.c_str(),
                                         static_cast<unsigned long>(candidate.address),
                                         candidate.observations, candidate.confidence,
                                         candidate.before.size(), tail);
                    }
                }
#endif
                // A verified location is the text in front of the caret,
                // which does not change while the composition grows. Publish
                // it even when typing already moved on; the published callback
                // re-requests the prediction for the composition on screen.
                if (verified) {
                    LLAVON_DEBUG_LOG("MEMCTX", "preedit=\"%s\" confirmed pid=%d address=0x%lx observations=%u confidence=%d candidates=%zu",
                                      job.primary().c_str(), verified->pid,
                                     static_cast<unsigned long>(verified->address),
                                     verified->observations, verified->confidence,
                                     candidates_.size());
                    try {
                        hooks_.publish(verified_context, true);
                        last_context_ = std::move(verified_context);
                        last_published_commit_ = job.committed_anchor;
                        published_usable_ = true;
                        unverified_ = 0;
                        notify_published = true;
                        hooks_.availability(AccessibilityAvailability::Available, "memscan");
                    } catch (const std::exception&) {
                        published_usable_ = false;
                        hooks_.publish({}, false);
                    }
                } else if (current_job) {
                    const auto progress = result && result->contains("progress")
                                              ? (*result)["progress"].dump() : std::string{};
                    LLAVON_DEBUG_LOG("MEMCTX", "preedit=\"%s\" unverified matches=%zu accepted=%zu error=%s progress=%s",
                                      job.primary().c_str(), matches.size(), candidates_.size(),
                                     error_code.c_str(), progress.c_str());
                    // A miss on one preedit state (client redraw lag, scan
                    // budget) must not discard a location that natural preedit
                    // changes already confirmed. Document changes invalidate
                    // it explicitly; several unverified states in a row drop
                    // it as well, so a vanished location cannot linger.
                    if (published_usable_ && ++unverified_ >= 2) {
                        published_usable_ = false;
                        // The old caret may have vanished. Its tail must not
                        // veto a newly found document in another location.
                        last_context_.clear();
                        last_published_commit_.clear();
                        hint_pid_ = 0;
                        hint_address_ = 0;
                        hint_before_.clear();
                    }
                    if (!published_usable_) {
                        hooks_.publish({}, false);
                        if (!result)
                            hooks_.availability(AccessibilityAvailability::Unavailable, "helper-failed");
                        else if (error_code == "denied")
                            hooks_.availability(AccessibilityAvailability::Unavailable, "permission-denied");
                    }
                }
            }
            // Notify outside the lock: the engine re-requests the prediction
            // for the composition on screen so the confirmed context is used.
            ++probes_;
            if (notify_published && callbacks_.published) callbacks_.published();
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
    std::string last_commit_history_;
    // The last published context. A following publish has to extend it while
    // the caret keeps moving forward, so stale copies cannot shrink it.
    std::u16string last_context_;
    std::string last_published_commit_;
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
    std::size_t misses_ = 0;
    std::size_t focus_misses_ = 0;
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
void MemoryContextProvider::invalidate(bool context_continues) {
    impl_->invalidate(context_continues);
}
void MemoryContextProvider::prime() { impl_->prime(); }
std::size_t MemoryContextProvider::probe_count() const noexcept { return impl_->probe_count(); }
bool MemoryContextProvider::tracking() const { return impl_->tracking(); }

bool memory_context_supported() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

}  // namespace llavon::ime

#include "context/memory_context.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace llavon::ime {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class FakeHelper {
public:
    explicit FakeHelper(std::string mode = "stable") {
        static std::atomic<int> serial{0};
        path_ = std::filesystem::temp_directory_path() /
                ("memscan-serve-test-" + std::to_string(getpid()) + "-" +
                 std::to_string(serial++) + ".py");
        log_ = path_.string() + ".log";
        std::ofstream script(path_);
        script << "#!/usr/bin/env python3\n"
               << "import json, sys, time\n"
               << "with open(" << nlohmann_quote(log_) << ", 'a') as log: log.write('start\\n')\n"
               << "for line in sys.stdin:\n"
               << " request = json.loads(line)\n"
               << " with open(" << nlohmann_quote(log_) << ", 'a') as log: log.write(request['anchor'] + '\\n')\n"
               << " mode = " << nlohmann_quote(mode) << "\n"
               << " if mode == 'slow': time.sleep(0.15)\n"
               << " if mode == 'denied': reply = {'matches': [], 'error': 'denied'}\n"
               << " elif mode == 'miss': reply = {'matches': [], 'error': 'not-found'}\n"
               << " elif mode == 'bad-error': reply = {'matches': [], 'error': 42}\n"
               << " else:\n"
               << "  address = 4096 if mode != 'moving' else 4096 + len(request['anchor'])\n"
               << "  before = 'document prefix ' if mode != 'changing' else 'document ' + request['anchor']\n"
               << "  reply = {'matches': [{'pid': 4242, 'encoding': 'utf8', 'address': address, 'before': before}], 'error': 'not-found'}\n"
               << " print(json.dumps(reply), flush=True)\n"
               << " if mode == 'exit': sys.exit(0)\n";
        script.close();
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    ~FakeHelper() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(log_, error);
    }
    const auto& path() const { return path_; }
    std::string log() const {
        std::ifstream file(log_);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

private:
    static std::string nlohmann_quote(const std::string& text) {
        // The generated script uses paths and mode names from these tests only.
        return "'" + text + "'";
    }
    std::filesystem::path path_;
    std::string log_;
};

struct ProbeState {
    std::mutex mutex;
    std::string preedit = "ㄋ";
    std::vector<int> pids{4242};
    bool sensitive = false;

    MemoryProbeCallbacks callbacks() {
        MemoryProbeCallbacks result;
        result.preedit = [this] {
            std::lock_guard lock(mutex);
            return preedit;
        };
        result.processes = [this] {
            std::lock_guard lock(mutex);
            return pids;
        };
        result.sensitive = [this] {
            std::lock_guard lock(mutex);
            return sensitive;
        };
        return result;
    }
    void set(std::string text) {
        std::lock_guard lock(mutex);
        preedit = std::move(text);
    }
};

bool step(MemoryContextProvider& provider, ProbeState& state, std::string text, std::size_t count) {
    const auto previous = provider.sequence();
    state.set(std::move(text));
    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    provider.refresh();
    return wait_for([&] { return provider.probe_count() >= count && provider.sequence() > previous; });
}

bool test_natural_changes_confirm_and_reuse_helper() {
    FakeHelper helper;
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "resident helper starts");
    provider.set_active(true);
    ok &= check(step(provider, state, "ㄋ", 1), "first phonetic state scanned");
    ok &= check(!provider.latest()->usable, "a single matching string is never trusted");
    ok &= check(step(provider, state, "ㄋㄧ", 2), "second phonetic state scanned");
    ok &= check(!provider.latest()->usable, "one transition is not enough");
    ok &= check(step(provider, state, "你", 3), "converted character scanned");
    ok &= check(wait_for([&] {
                    const auto sample = provider.latest();
                    return sample && sample->usable && sample->text == u"document prefix ";
                }), "two natural changes at one address validate context");
    const auto log = helper.log();
    ok &= check(log.find("ㄋ\nㄋㄧ\n你\n") != std::string::npos,
                "helper receives every natural preedit without added characters");
    ok &= check(log.find("start\n") == log.rfind("start\n"),
                "all scans reuse one child process");
    provider.stop();
    return ok;
}

bool test_wrong_location_never_publishes() {
    bool ok = true;
    for (const auto* mode : {"moving", "changing", "miss"}) {
        FakeHelper helper(mode);
        ProbeState state;
        MemoryContextProvider provider(64, state.callbacks(), helper.path());
        ok &= check(provider.start(), "helper starts");
        provider.set_active(true);
        ok &= step(provider, state, "ㄋ", 1);
        ok &= step(provider, state, "ㄋㄧ", 2);
        ok &= step(provider, state, "你", 3);
        ok &= check(!provider.latest()->usable, "moving or unrelated copies are rejected");
        provider.stop();
    }
    return ok;
}

bool test_superseded_probe_never_publishes() {
    FakeHelper helper("slow");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "slow provider starts");
    provider.set_active(true);
    state.set("ㄋ");
    provider.refresh();
    ok &= check(wait_for([&] { return provider.probe_count() >= 1; }),
                "first slow probe begins");
    state.set("ㄋㄧ");
    provider.refresh();
    state.set("你");
    provider.refresh();
    ok &= check(wait_for([&] { return provider.probe_count() >= 2; }),
                "newest preedit is probed");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ok &= check(provider.latest() && !provider.latest()->usable,
                "superseded result cannot publish a verified context");
    provider.stop();
    return ok;
}

bool test_focus_discards_cache() {
    FakeHelper helper;
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    ok &= step(provider, state, "ㄋ", 1);
    ok &= step(provider, state, "ㄋㄧ", 2);
    ok &= step(provider, state, "你", 3);
    ok &= check(wait_for([&] { return provider.latest() && provider.latest()->usable; }),
                "location confirmed before focus switch");
    provider.invalidate();
    ok &= check(provider.latest() && !provider.latest()->usable, "focus invalidates published context");
    ok &= step(provider, state, "他", 4);
    ok &= check(!provider.latest()->usable, "one new-state observation cannot reuse old location");
    provider.stop();
    return ok;
}

bool test_dead_helper_does_not_crash_ime() {
    FakeHelper helper("exit");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    ok &= step(provider, state, "ㄋ", 1);
    // The helper exited after its reply. The next write sees a closed pipe;
    // only that probe fails, and the following request restarts the child.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ok &= step(provider, state, "ㄋㄧ", 2);
    ok &= step(provider, state, "你", 3);
    ok &= check(!provider.latest()->usable, "a restarted helper cannot certify stale candidates");
    ok &= check(wait_for([&] { return helper.log().find("start\n", 1) != std::string::npos; }),
                "dead helper restarted on a subsequent request");
    provider.stop();
    return ok;
}

bool test_skip_and_denied() {
    FakeHelper helper("denied");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(false);
    provider.refresh();
    provider.set_active(true);
    state.sensitive = true;
    provider.refresh();
    state.sensitive = false;
    state.pids.clear();
    provider.refresh();
    ok &= check(provider.probe_count() == 0, "inactive, sensitive and unnamed clients are skipped");
    state.pids = {4242};
    ok &= step(provider, state, "ㄋ", 1);
    ok &= check(wait_for([&] {
                    return provider.availability().detail == "permission-denied";
                }), "permission failure reported");
    provider.stop();
    return ok;
}

bool test_sensitive_transition_forgets_context() {
    FakeHelper helper;
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    ok &= step(provider, state, "ㄋ", 1);
    ok &= step(provider, state, "ㄋㄧ", 2);
    ok &= step(provider, state, "你", 3);
    ok &= check(provider.latest() && provider.latest()->usable, "sample established before sensitive field");
    {
        std::lock_guard lock(state.mutex);
        state.sensitive = true;
    }
    provider.refresh();
    ok &= check(provider.latest() && !provider.latest()->usable,
                "entering a sensitive field discards the cached document context");
    {
        std::lock_guard lock(state.mutex);
        state.sensitive = false;
    }
    ok &= step(provider, state, "他", 4);
    ok &= check(!provider.latest()->usable, "old context is not reused after sensitive input");
    provider.stop();
    return ok;
}

bool test_bad_helper_response_does_not_crash_worker() {
    FakeHelper helper("bad-error");
    ProbeState state;
    MemoryContextProvider provider(64, state.callbacks(), helper.path());
    bool ok = check(provider.start(), "provider starts");
    provider.set_active(true);
    ok &= step(provider, state, "ㄋ", 1);
    ok &= step(provider, state, "ㄋㄧ", 2);
    ok &= check(provider.latest() && !provider.latest()->usable,
                "malformed helper error is handled without publishing context");
    provider.stop();
    return ok;
}

#if defined(__linux__)
// A client that stores its screen as 12-byte cells: a UTF-32 code point plus
// attributes, with wide characters repeated in a continuation cell. The
// composition starts at a fixed address so the scanner can verify it across
// preedit changes.
struct GridChild {
    pid_t pid = -1;
    int commands = -1;
    int responses = -1;
};

GridChild spawn_grid_child(std::string_view prefix) {
    int commands[2], responses[2];
    if (::pipe(commands) != 0) return {};
    if (::pipe(responses) != 0) {
        ::close(commands[0]);
        ::close(commands[1]);
        return {};
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(commands[1]);
        ::close(responses[0]);
        auto append_cell = [](std::string& out, char32_t codepoint, bool continuation) {
            for (int shift = 0; shift < 32; shift += 8) {
                out.push_back(static_cast<char>((codepoint >> shift) & 0xff));
            }
            const unsigned char attributes[8] = {
                0x00, 0x00, 0x0e, 0x00,
                static_cast<unsigned char>(continuation ? 0x01 : 0x00), 0x04, 0x00, 0x00};
            out.append(reinterpret_cast<const char*>(attributes), 8);
        };
        std::string buffer;
        buffer.reserve(16384);
        for (const char value : prefix) append_cell(buffer, static_cast<unsigned char>(value), false);
        const std::size_t composition_start = buffer.size();
        constexpr std::size_t kCompositionCells = 256;
        for (std::size_t cell = 0; cell < kCompositionCells; ++cell) append_cell(buffer, 0, false);
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data() + composition_start);
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
            std::vector<char32_t> text;
            if (step == '1') text = {0x310B};
            if (step == '2') text = {0x310B, 0x3127};
            if (step == '3') text = {0x4F60};
            std::string cells;
            for (const char32_t codepoint : text) {
                append_cell(cells, codepoint, false);
                append_cell(cells, codepoint, true);
            }
            while (cells.size() < kCompositionCells * 12) cells.push_back('\0');
            buffer.replace(composition_start, cells.size(), cells);
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

bool update_grid(const GridChild& child, char step) {
    char ack = 0;
    return ::write(child.commands, &step, 1) == 1 &&
           ::read(child.responses, &ack, 1) == 1 && ack == step;
}

// End-to-end: the real scanner must locate the client's grid composition and
// the provider must confirm the same address and prefix across natural
// preedit changes. Skipped unless the built helper is named in the
// environment, so the ordinary test run stays self-contained.
bool test_real_scanner_terminal_grid() {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') return true;
    const GridChild child = spawn_grid_child("document prefix ");
    if (!check(child.pid > 0, "grid child spawned")) return false;
    ProbeState state;
    {
        std::lock_guard lock(state.mutex);
        state.pids = {static_cast<int>(child.pid)};
    }
    MemoryContextProvider provider(64, state.callbacks(), helper);
    bool ok = check(provider.start(), "real helper starts");
    provider.set_active(true);
    auto advance = [&](char step_key, std::string preedit, std::size_t count) {
        ok &= check(update_grid(child, step_key), "grid composition updated");
        return step(provider, state, std::move(preedit), count);
    };
    ok &= advance('1', "ㄋ", 1);
    ok &= check(!provider.latest()->usable, "one grid observation is not trusted");
    ok &= advance('2', "ㄋㄧ", 2);
    ok &= advance('3', "你", 3);
    ok &= check(wait_for([&] {
                    const auto sample = provider.latest();
                    return sample && sample->usable && sample->text.ends_with(u"document prefix ");
                }), "the real scanner confirms the terminal grid across preedit changes");
    provider.stop();
    ::kill(child.pid, SIGKILL);
    int status = 0;
    ::waitpid(child.pid, &status, 0);
    ::close(child.commands);
    ::close(child.responses);
    return ok;
}
#endif

}  // namespace
}  // namespace llavon::ime

int run_memory_context_tests() {
    using namespace llavon::ime;
    if (!memory_context_supported()) return EXIT_SUCCESS;
    bool ok = true;
    ok &= test_natural_changes_confirm_and_reuse_helper();
    ok &= test_wrong_location_never_publishes();
    ok &= test_superseded_probe_never_publishes();
    ok &= test_focus_discards_cache();
    ok &= test_dead_helper_does_not_crash_ime();
    ok &= test_skip_and_denied();
    ok &= test_sensitive_transition_forgets_context();
    ok &= test_bad_helper_response_does_not_crash_worker();
#if defined(__linux__)
    ok &= test_real_scanner_terminal_grid();
#endif
    if (ok) std::printf("memory context tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

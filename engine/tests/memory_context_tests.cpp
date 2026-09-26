#include "context/memory_context.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
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
               << "import json, sys\n"
               << "with open(" << nlohmann_quote(log_) << ", 'a') as log: log.write('start\\n')\n"
               << "for line in sys.stdin:\n"
               << " request = json.loads(line)\n"
               << " with open(" << nlohmann_quote(log_) << ", 'a') as log: log.write(request['anchor'] + '\\n')\n"
               << " mode = " << nlohmann_quote(mode) << "\n"
               << " if mode == 'denied': reply = {'matches': [], 'error': 'denied'}\n"
               << " elif mode == 'miss': reply = {'matches': [], 'error': 'not-found'}\n"
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

}  // namespace
}  // namespace llavon::ime

int run_memory_context_tests() {
    using namespace llavon::ime;
    if (!memory_context_supported()) return EXIT_SUCCESS;
    bool ok = true;
    ok &= test_natural_changes_confirm_and_reuse_helper();
    ok &= test_wrong_location_never_publishes();
    ok &= test_focus_discards_cache();
    ok &= test_dead_helper_does_not_crash_ime();
    ok &= test_skip_and_denied();
    if (ok) std::printf("memory context tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

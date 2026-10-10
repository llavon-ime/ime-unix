#include "raw_key_harness.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(__linux__)
namespace llavon::ime::rawkey {
namespace {

// Controlled helper replies, not a memory reader or an application fixture.
// The files synchronize transport ordering only; they never supply context
// to the engine. All candidate text is explicitly mocked in the helper.
class OrderedHelper {
public:
    OrderedHelper() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "llavon-memscan-order-XXXXXX").string();
        const char* directory = ::mkdtemp(pattern.data());
        RAWKEY_ASSERT(directory != nullptr);
        root_ = directory;
        path_ = root_ / "helper.py";
        std::ofstream script(path_);
        script << R"PY(#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import time

root = Path(__file__).parent
count = 0
def record(kind, value):
    with (root / 'events.jsonl').open('a') as stream:
        stream.write(json.dumps({'kind': kind, 'value': value}) + '\n')
for line in sys.stdin:
    request = json.loads(line)
    record('request', request)
    if request.get('prime'):
        reply = {'matches': []}
    else:
        count += 1
        if count in (3, 4):
            name = 'old' if count == 3 else 'new'
            (root / (name + '-ready')).touch()
            deadline = time.monotonic() + 2.5
            while not (root / (name + '-release')).exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError('mock transport gate timed out')
                time.sleep(0.005)
        reply = {'matches': [{'pid': 4242, 'address': 4096,
                             'encoding': 'utf8', 'before': 'test scan prefix '}]}
    record('reply', reply)
    print(json.dumps(reply), flush=True)
)PY";
        script.close();
        RAWKEY_ASSERT(script.good());
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    OrderedHelper(const OrderedHelper&) = delete;
    OrderedHelper& operator=(const OrderedHelper&) = delete;

    ~OrderedHelper() {
        std::ifstream events(root_ / "events.jsonl");
        const std::string body{std::istreambuf_iterator<char>(events),
                               std::istreambuf_iterator<char>()};
        std::printf("[mock memory-order events]\n%s", body.c_str());
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::string path() const { return path_.string(); }
    bool ready(std::string_view name) const {
        return std::filesystem::exists(root_ / (std::string(name) + "-ready"));
    }
    void release(std::string_view name) const {
        std::ofstream signal(root_ / (std::string(name) + "-release"));
        signal << "release\n";
        RAWKEY_ASSERT(signal.good());
    }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

template <typename Predicate>
bool wait_for_gate(Harness& harness, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        // File/probe changes do not necessarily post a frontend callback.
        // Pump in bounded slices instead of sleeping through the mock gate.
        if (harness.pump_until(predicate, std::chrono::milliseconds(1))) return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

}  // namespace

RAWKEY_SUITE("superseded memory scan cannot publish before the current reply", memory_order) {
    OrderedHelper helper;
    HarnessOptions options;
    options.memory_helper_path = helper.path();
    Harness harness(options);
    harness.host().set_probe_pids({4242});
    harness.activate();

    const auto first = harness.memory_probe_count();
    harness.key("s");
    RAWKEY_ASSERT(harness.preedit() == "ㄋ");
    RAWKEY_ASSERT(wait_for_gate(harness, [&] { return harness.memory_probe_count() > first; }));
    RAWKEY_ASSERT(harness.context_text().empty());

    const auto second = harness.memory_probe_count();
    harness.key("u");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    RAWKEY_ASSERT(wait_for_gate(harness, [&] { return harness.memory_probe_count() > second; }));
    RAWKEY_ASSERT(harness.context_text().empty());

    harness.key("p");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧㄣ");
    RAWKEY_ASSERT(wait_for_gate(harness, [&] { return helper.ready("old"); }));
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    helper.release("old");
    RAWKEY_ASSERT(wait_for_gate(harness, [&] { return helper.ready("new"); }));
    harness.drain();
    std::printf("[memory-order] current_reply_held=1 context=%s preedit=%s commits=%zu\n",
                harness.context_text().c_str(), harness.preedit().c_str(), harness.commits().size());
    RAWKEY_ASSERT(harness.context_text().empty());
    RAWKEY_ASSERT(harness.commits().empty());

    helper.release("new");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.context_text() == "test scan prefix "; }));
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    harness.key("3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.expect_commit("你");
    RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{"你"});
}

}  // namespace llavon::ime::rawkey
#endif

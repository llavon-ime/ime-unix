#include "training/job_process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace {
bool wait(ime::unix_service::JobProcess& process, bool expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto result = process.poll()) return *result == expected;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    process.terminate();
    return false;
}
}

int main() {
    auto pattern = (std::filesystem::temp_directory_path() / "llavon-job-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) return EXIT_FAILURE;
    const std::filesystem::path root(pattern);
    bool ok = true;
    {
        ime::unix_service::JobProcess process;
        // Password has no representation in argv or environment; fd 3 carries it.
        process.launch("/bin/sh", {"-c", "IFS= read -r secret <&3; test \"$secret\" = test-only-secret; printf '%s' success"},
                       root / "job.log", "test-only-secret");
        ok = ok && wait(process, true);
        std::ifstream log(root / "job.log"); std::string text; log >> text;
        ok = ok && text == "success";
        process.launch("/bin/sh", {"-c", "exit 7"}, root / "job.log");
        ok = ok && wait(process, false);
        process.launch("/nonexistent/llavon-cli", {}, root / "job.log");
        ok = ok && wait(process, false);
        process.launch("/bin/sh", {"-c", "sleep 60"}, root / "job.log");
        process.cancel();
        process.cancel();
        ok = ok && wait(process, false) && !process.running() && process.cancelling();
        process.launch("/bin/sh", {"-c", "exit 0"}, root / "job.log");
        ok = ok && wait(process, true) && !process.cancelling();
        process.launch("/bin/sh", {"-c", "sleep 60"}, root / "job.log");
        process.terminate();
        ok = ok && !process.running();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#pragma once

#include "anchor.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace llavon::memscan {

// A previously successful region for a PID; tried before the full scan so a
// repeat probe is fast.
struct Hint {
    Hint() = default;
    Hint(pid_t process, std::uintptr_t begin, std::uintptr_t finish,
         std::uintptr_t expected_address = 0, std::string expected_before = {})
        : pid(process), start(begin), end(finish), match_address(expected_address),
          before(std::move(expected_before)) {}

    pid_t pid = -1;
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    // A nearby copy is not enough: only an exact, previously verified match
    // with the same prefix may bypass a full scan.
    std::uintptr_t match_address = 0;
    std::string before;
};

struct ScanLimits {
    std::size_t max_bytes_per_pid = 512ull * 1024 * 1024;
    std::chrono::milliseconds timeout{3000};
    std::size_t window_limit = 64 * 1024;
};

struct Match {
    pid_t pid = -1;
    Encoding encoding = Encoding::Utf8;
    std::uintptr_t address = 0;
    std::string before;
    std::string after;
    std::size_t before_bytes = 0;
    std::size_t after_bytes = 0;
    std::size_t scanned_bytes = 0;
    bool truncated = false;
};

struct ScanError {
    std::string code;
    std::string detail;
};

struct ScanStats {
    std::size_t hits = 0;       // Aligned byte-pattern matches, before prefix filtering.
    std::size_t qualified = 0;  // Matches with a readable prefix; may exceed the returned cap.
};

// True when /proc/<pid> is owned by the calling user.
bool same_uid(pid_t pid);

// One readable mapping of the target.
struct Region {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    bool writable = false;
};

// Finds the anchor in one process and returns the decoded window around it.
// Refuses foreign PIDs, validates the caller-provided budget, and never
// returns more than ScanLimits::window_limit bytes per side. Hints are tried
// first so repeat probes stay fast.
std::optional<Match> scan_pid(pid_t pid, const Anchor& anchor, std::size_t before_bytes,
                              std::size_t after_bytes, const ScanLimits& limits,
                              const std::vector<Hint>& hints, ScanError& error,
                              std::vector<Match>* candidates = nullptr,
                              std::uintptr_t* next_address = nullptr,
                              ScanStats* stats = nullptr);

}  // namespace llavon::memscan

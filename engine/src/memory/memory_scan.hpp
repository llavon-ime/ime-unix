#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace context_scan {

struct Region {
    std::uintptr_t begin{};
    std::size_t size{};
};

struct Limits {
    std::size_t max_bytes = 256 * 1024;
    // Checked between reads. A syscall can overrun this soft deadline.
    std::chrono::microseconds time_budget{2000};
    std::size_t max_hits = 16;
};

enum class Stop { complete, byte_budget, time_budget, hit_budget, unavailable };

struct Result {
    std::vector<std::uintptr_t> hits;
    std::size_t bytes_attempted{};
    std::size_t bytes_read{};
    std::size_t read_calls{};
    std::size_t unreadable_pages{};
    int error{};
    Stop stop = Stop::complete;
    std::chrono::microseconds elapsed{};

    // This only describes coverage of the supplied region. Even a unique,
    // stable hit does not establish document identity or the current caret.
    [[nodiscard]] bool fully_read() const noexcept {
        return stop == Stop::complete && unreadable_pages == 0;
    }
};

[[nodiscard]] Result scan(pid_t pid, Region region, std::string_view needle,
                          Limits limits = {});
[[nodiscard]] std::string_view name(Stop stop) noexcept;

} // namespace context_scan

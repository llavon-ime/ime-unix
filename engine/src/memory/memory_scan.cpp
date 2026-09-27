#include "memory_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>

#include <sys/uio.h>
#include <unistd.h>

namespace context_scan {

Result scan(pid_t pid, Region region, std::string_view needle, Limits limits) {
    if (pid <= 0 || needle.empty() || needle.size() > 1024 || limits.max_hits == 0 ||
        region.size > std::numeric_limits<std::uintptr_t>::max() - region.begin) {
        throw std::invalid_argument("invalid scan parameters");
    }
    const auto raw_page_size = ::sysconf(_SC_PAGESIZE);
    if (raw_page_size <= 0) throw std::runtime_error("cannot determine page size");
    const auto page_size = static_cast<std::size_t>(raw_page_size);
    const auto start = std::chrono::steady_clock::now();
    Result result;
    std::string carry;
    std::string block(page_size, '\0');
    std::string window;
    window.reserve(page_size + needle.size());
    std::size_t offset = 0;
    while (offset < region.size) {
        if (std::chrono::steady_clock::now() - start >= limits.time_budget) {
            result.stop = Stop::time_budget;
            break;
        }
        if (result.bytes_attempted >= limits.max_bytes) {
            result.stop = Stop::byte_budget;
            break;
        }
        const auto address = region.begin + offset;
        // Each syscall stays within one remote page. Gaps cannot accidentally
        // join two unrelated strings, and an unreadable page does not end a scan.
        const auto count = std::min({page_size - address % page_size,
                                     region.size - offset,
                                     limits.max_bytes - result.bytes_attempted});
        iovec local{block.data(), count};
        iovec remote{reinterpret_cast<void*>(address), count};
        const auto read = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        const int read_error = read < 0 ? errno : 0;
        ++result.read_calls;
        result.bytes_attempted += count;
        if (read <= 0) {
            carry.clear();
            if (read_error == EPERM || read_error == EACCES || read_error == ESRCH) {
                result.error = read_error;
                result.stop = Stop::unavailable;
                break;
            }
            ++result.unreadable_pages;
        } else {
            const auto received = static_cast<std::size_t>(read);
            result.bytes_read += received;
            window.assign(carry);
            window.append(block.data(), received);
            const auto window_begin = address - carry.size();
            for (auto pos = window.find(needle); pos != std::string::npos;
                 pos = window.find(needle, pos + 1)) {
                result.hits.push_back(window_begin + pos);
                if (result.hits.size() == limits.max_hits) {
                    result.stop = Stop::hit_budget;
                    break;
                }
            }
            if (result.stop == Stop::hit_budget) break;
            if (received == count) {
                carry.assign(window, window.size() - std::min(needle.size() - 1, window.size()),
                             std::string::npos);
            } else {
                ++result.unreadable_pages;
                carry.clear();
            }
        }
        offset += count;
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

std::string_view name(Stop stop) noexcept {
    switch (stop) {
        case Stop::complete: return "complete";
        case Stop::byte_budget: return "byte_budget";
        case Stop::time_budget: return "time_budget";
        case Stop::hit_budget: return "hit_budget";
        case Stop::unavailable: return "unavailable";
    }
    return "unknown";
}

} // namespace context_scan

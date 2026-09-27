// Diagnostic helper for chromium_undo_probe.mjs. Read-only, resident pages
// only, bounded work, no privilege or sandbox changes, no captured-text output.
#include "memory_scan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

std::uint64_t number(std::string_view text, int base = 10) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid number");
    return value;
}

struct Pattern { std::string label; std::string bytes; };

std::vector<Pattern> patterns(const char* file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("cannot open pattern file");
    std::vector<Pattern> result;
    std::string label, hex;
    while (input >> label >> hex) {
        if (label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
            hex.empty() || hex.size() % 2 != 0 || hex.size() > 2048 || result.size() >= 8) {
            throw std::runtime_error("invalid pattern");
        }
        std::string bytes;
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            bytes.push_back(static_cast<char>(number(std::string_view(hex).substr(i, 2), 16)));
        }
        result.push_back({label, std::move(bytes)});
    }
    if (result.empty()) throw std::runtime_error("no patterns");
    return result;
}

struct Resident {
    std::vector<context_scan::Region> runs;
    bool complete = true;
    int error = 0;
};

Resident resident_pages(pid_t pid, Clock::time_point deadline) {
    Resident result;
    const auto page_value = ::sysconf(_SC_PAGESIZE);
    if (page_value <= 0) throw std::runtime_error("invalid page size");
    const auto page = static_cast<std::size_t>(page_value);
    const auto directory = "/proc/" + std::to_string(pid);
    std::ifstream maps(directory + "/maps");
    Fd pagemap(::open((directory + "/pagemap").c_str(), O_RDONLY | O_CLOEXEC));
    if (!maps || pagemap.get() < 0) {
        result.complete = false;
        result.error = errno != 0 ? errno : EACCES;
        return result;
    }
    std::array<std::uint64_t, 1024> entries{};
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range, perms, offset, device, inode, name;
        fields >> range >> perms >> offset >> device >> inode;
        std::getline(fields, name);
        const auto first = name.find_first_not_of(' ');
        // Include anonymous allocations, [heap], [stack] and named anonymous
        // allocations; skip file-backed mappings. Report this bounded scope.
        if (perms != "rw-p" || (first != std::string::npos && name[first] != '[')) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) throw std::runtime_error("invalid maps range");
        const auto begin = number(std::string_view(range).substr(0, dash), 16);
        const auto end = number(std::string_view(range).substr(dash + 1), 16);
        for (auto address = begin; address < end;) {
            if (Clock::now() >= deadline) { result.complete = false; return result; }
            const auto count = std::min<std::size_t>(entries.size(), (end - address) / page);
            if (count == 0) break;
            ssize_t read{};
            do {
                read = ::pread(pagemap.get(), entries.data(), count * sizeof(entries[0]),
                               static_cast<off_t>(address / page * sizeof(entries[0])));
            } while (read < 0 && errno == EINTR);
            if (read != static_cast<ssize_t>(count * sizeof(entries[0]))) {
                result.complete = false;
                result.error = read < 0 ? errno : EIO;
                return result;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if ((entries[i] & (std::uint64_t{1} << 63)) == 0) continue;
                const auto current = static_cast<std::uintptr_t>(address + i * page);
                if (!result.runs.empty() && result.runs.back().begin + result.runs.back().size == current) {
                    result.runs.back().size += page;
                } else {
                    result.runs.push_back({current, page});
                }
            }
            address += count * page;
        }
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: memory_residue_probe PID PATTERNS_FILE");
        const auto raw_pid = number(argv[1]);
        if (raw_pid == 0 || raw_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
            throw std::runtime_error("invalid PID");
        }
        const auto pid = static_cast<pid_t>(raw_pid);
        const auto needles = patterns(argv[2]);
        const auto start = Clock::now();
        const auto deadline = start + 5s;
        const auto resident = resident_pages(pid, deadline);
        std::size_t remaining_bytes = 256 * 1024 * 1024;
        std::cout << "{\"pid\":" << pid << ",\"scope\":\"resident_anonymous_rw_private\","
                  << "\"pagemap_complete\":" << (resident.complete ? "true" : "false")
                  << ",\"pagemap_errno\":" << resident.error << ",\"patterns\":[";
        bool separator = false;
        for (const auto& pattern : needles) {
            std::size_t hits = 0, bytes = 0;
            int error = 0;
            bool complete = resident.complete;
            for (const auto region : resident.runs) {
                const auto remaining_time = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
                if (remaining_bytes == 0 || remaining_time <= 0us || hits >= 256) { complete = false; break; }
                const auto scan = context_scan::scan(pid, region, pattern.bytes,
                                                     {remaining_bytes, remaining_time, 256 - hits});
                remaining_bytes -= scan.bytes_attempted;
                bytes += scan.bytes_read;
                hits += scan.hits.size();
                complete = complete && scan.fully_read();
                if (scan.error != 0) { error = scan.error; break; }
            }
            if (separator) std::cout << ',';
            separator = true;
            std::cout << "{\"label\":\"" << pattern.label << "\",\"hits\":" << hits
                      << ",\"bytes_read\":" << bytes << ",\"complete\":" << (complete ? "true" : "false")
                      << ",\"errno\":" << error << '}';
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        std::cout << "],\"elapsed_us\":" << elapsed << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

#include "memory/process_memory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "memory/memory_scan.hpp"
#include "text/utf.hpp"

namespace llavon::ime::memory {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

std::uint64_t number(std::string_view input, int base = 10) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value, base);
    if (error != std::errc{} || end != input.data() + input.size()) return 0;
    return value;
}

std::uint64_t start_time(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(file, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream fields(line.substr(close + 1));
    std::string value;
    for (int i = 0; i <= 19; ++i) if (!(fields >> value)) return 0;
    return number(value);
}

std::vector<context_scan::Region> resident(pid_t pid, Clock::time_point deadline, std::stop_token cancel) {
    const auto page_value = ::sysconf(_SC_PAGESIZE);
    if (page_value <= 0) return {};
    const auto page = static_cast<std::size_t>(page_value);
    const auto root = "/proc/" + std::to_string(pid);
    std::ifstream maps(root + "/maps");
    Fd pagemap(::open((root + "/pagemap").c_str(), O_RDONLY | O_CLOEXEC));
    if (!maps || pagemap.get() < 0) return {};
    std::vector<context_scan::Region> regions;
    std::array<std::uint64_t, 1024> entries{};
    std::string line;
    while (std::getline(maps, line)) {
        if (cancel.stop_requested() || Clock::now() >= deadline) break;
        std::istringstream fields(line);
        std::string range, permissions, offset, device, inode, name;
        fields >> range >> permissions >> offset >> device >> inode;
        std::getline(fields, name);
        const auto first = name.find_first_not_of(' ');
        if (permissions != "rw-p" || (first != std::string::npos && name[first] != '[')) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        const auto begin = number(std::string_view(range).substr(0, dash), 16);
        const auto end = number(std::string_view(range).substr(dash + 1), 16);
        for (auto address = begin; address < end;) {
            if (cancel.stop_requested() || Clock::now() >= deadline) return regions;
            const auto count = std::min<std::size_t>(entries.size(), (end - address) / page);
            if (!count) break;
            const auto read = ::pread(pagemap.get(), entries.data(), count * sizeof(entries[0]),
                                      static_cast<off_t>(address / page * sizeof(entries[0])));
            if (read != static_cast<ssize_t>(count * sizeof(entries[0]))) break;
            for (std::size_t i = 0; i < count; ++i) {
                if ((entries[i] & (std::uint64_t{1} << 63)) == 0) continue;
                const auto current = static_cast<std::uintptr_t>(address + i * page);
                if (!regions.empty() && regions.back().begin + regions.back().size == current) regions.back().size += page;
                else regions.push_back({current, page});
            }
            address += count * page;
        }
    }
    return regions;
}

bool read_exact(pid_t pid, std::uintptr_t address, std::string& bytes) {
    iovec local{bytes.data(), bytes.size()};
    iovec remote{reinterpret_cast<void*>(address), bytes.size()};
    return ::process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes.size());
}

std::string utf16le(std::u16string_view text) {
    std::string result;
    result.reserve(text.size() * 2);
    for (const char16_t unit : text) {
        result.push_back(static_cast<char>(unit & 0xff));
        result.push_back(static_cast<char>(unit >> 8));
    }
    return result;
}
} // namespace

std::u16string make_marker() {
    std::array<unsigned char, 16> entropy{};
    std::size_t offset = 0;
    while (offset < entropy.size()) {
        const auto count = ::getrandom(entropy.data() + offset, entropy.size() - offset, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "marker entropy unavailable");
        offset += static_cast<std::size_t>(count);
    }
    std::u16string marker;
    marker.reserve(128);
    for (const auto byte : entropy) {
        for (int bit = 7; bit >= 0; --bit) marker.push_back(byte & (1 << bit) ? u'\u2064' : u'\u2063');
    }
    return marker;
}

Discovery discover(std::string_view program, std::stop_token cancel) {
    Discovery result{{}, "process-unavailable"};
    if (program.empty() || program.find('/') != std::string_view::npos || ::getuid() == 0) return result;
    const auto deadline = Clock::now() + 100ms;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (cancel.stop_requested()) return {{}, "cancelled"};
        if (Clock::now() >= deadline) break;
        const auto pid_value = number(entry.path().filename().string());
        if (pid_value == 0 || pid_value > 2147483647 || pid_value == static_cast<unsigned>(::getpid())) continue;
        struct stat info{};
        if (::stat(entry.path().c_str(), &info) != 0 || info.st_uid != ::getuid()) continue;
        const auto executable = std::filesystem::read_symlink(entry.path() / "exe", error);
        if (error || executable.filename().string() != program) { error.clear(); continue; }
        const auto pid = static_cast<pid_t>(pid_value);
        const auto identity = start_time(pid);
        if (!identity) continue;
        const auto regions = resident(pid, deadline, cancel);
        std::string byte(1, '\0');
        if (regions.empty() || !read_exact(pid, regions.front().begin, byte)) continue;
        result.processes.push_back({pid, identity});
        if (result.processes.size() == 32) break;
    }
    if (!result.processes.empty()) result.status = "prepared";
    return result;
}

Capture capture(const std::vector<Process>& processes, std::u16string_view baseline,
                std::size_t cursor, std::u16string_view marker, std::stop_token cancel) {
    Capture result{std::nullopt, "not-found", 0};
    if (cursor > baseline.size() || baseline.size() > 4096 || marker.size() != 128) return result;
    const auto deadline = Clock::now() + 150ms;
    std::size_t budget = 64 * 1024 * 1024;
    std::u16string marked(baseline);
    marked.insert(cursor, marker);
    const auto prefix = baseline.substr(0, cursor);
    struct Encoding { std::string needle; std::string expected; std::size_t prefix_bytes; bool wide; };
    const std::array encodings{
        Encoding{u16_to_utf8(marker), u16_to_utf8(marked), u16_to_utf8(prefix).size(), false},
        Encoding{utf16le(marker), utf16le(marked), prefix.size() * 2, true},
    };
    for (const auto process : processes) {
        if (start_time(process.pid) != process.start_time) continue;
        const auto regions = resident(process.pid, deadline, cancel);
        for (const auto& encoding : encodings) {
            for (const auto region : regions) {
                std::size_t offset = 0;
                while (offset < region.size) {
                    if (cancel.stop_requested()) { result.status = "cancelled"; return result; }
                    const auto time_left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
                    if (budget == 0 || time_left <= 0us) { result.status = "scan-budget"; return result; }
                    // Overlap chunks so a marker split at a scheduling boundary
                    // remains discoverable. Cancellation is checked each chunk.
                    const auto count = std::min<std::size_t>({256 * 1024, region.size - offset, budget});
                    const auto scan = context_scan::scan(process.pid, {region.begin + offset, count}, encoding.needle,
                                                        {count, std::min(time_left, 2000us), 32});
                    budget -= scan.bytes_attempted;
                    result.bytes_read += scan.bytes_read;
                    for (const auto hit : scan.hits) {
                        if (hit < encoding.prefix_bytes || encoding.expected.size() > budget) continue;
                        std::string candidate(encoding.expected.size(), '\0');
                        budget -= candidate.size();
                        if (!read_exact(process.pid, hit - encoding.prefix_bytes, candidate)) continue;
                        result.bytes_read += candidate.size();
                        if (candidate != encoding.expected || start_time(process.pid) != process.start_time) continue;
                        // Exact full-window validation rejects standalone IPC
                        // markers and unrelated adjacent bytes. Identical copies
                        // yield the same prefix and do not require ranking by age.
                        if (encoding.wide) {
                            std::u16string decoded;
                            for (std::size_t i = 0; i < encoding.prefix_bytes; i += 2) {
                                decoded.push_back(static_cast<char16_t>(static_cast<unsigned char>(candidate[i]) |
                                    (static_cast<unsigned char>(candidate[i + 1]) << 8)));
                            }
                            result.prefix = std::move(decoded);
                        } else {
                            result.prefix = utf8_to_u16(std::string_view(candidate).substr(0, encoding.prefix_bytes));
                        }
                        result.status = "ready";
                        return result;
                    }
                    if (scan.error != 0) break;
                    const auto progress = scan.bytes_attempted;
                    if (progress <= encoding.needle.size()) break;
                    if (offset + progress >= region.size) break;
                    offset += progress - (encoding.needle.size() - 1);
                }
            }
        }
    }
    return result;
}

} // namespace llavon::ime::memory

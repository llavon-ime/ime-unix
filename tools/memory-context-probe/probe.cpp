#include "memory_scan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using context_scan::Limits;
using context_scan::Region;
using context_scan::Result;
using context_scan::Stop;

constexpr std::string_view anchor = "拉麵與咖啡";
constexpr std::string_view prefix = "今天晚餐想吃";
using Marker = std::array<char, 32>;

Marker fresh_marker() {
    std::array<unsigned char, 16> entropy{};
    std::size_t offset = 0;
    while (offset < entropy.size()) {
        const auto count = ::getrandom(entropy.data() + offset, entropy.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "getrandom");
        if (count == 0) throw std::runtime_error("getrandom returned no bytes");
        offset += static_cast<std::size_t>(count);
    }
    constexpr std::string_view hex = "0123456789abcdef";
    Marker marker{};
    for (std::size_t i = 0; i < entropy.size(); ++i) {
        marker[2 * i] = hex[entropy[i] >> 4];
        marker[2 * i + 1] = hex[entropy[i] & 15];
    }
    return marker;
}

std::string_view marker_view(const Marker& marker) { return {marker.data(), marker.size()}; }

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const { return value_; }
    void reset(int value = -1) {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }
private:
    int value_;
};

void transfer(int fd, void* data, std::size_t size, bool writing) {
    auto* bytes = static_cast<char*>(data);
    while (size != 0) {
        const auto count = writing ? ::write(fd, bytes, size) : ::read(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "pipe");
        require(count != 0, "test process closed its pipe");
        const auto done = static_cast<std::size_t>(count);
        bytes += done;
        size -= done;
    }
}

template<class T> void send(int fd, T value) { transfer(fd, &value, sizeof(value), true); }
template<class T> T receive(int fd) {
    T value{};
    transfer(fd, &value, sizeof(value), false);
    return value;
}

std::string utf16_anchor() {
    static_assert(std::endian::native == std::endian::little,
                  "The UTF-16 fixture currently uses little-endian Linux");
    constexpr std::u16string_view value = u"拉麵😀";
    return {reinterpret_cast<const char*>(value.data()), value.size() * sizeof(char16_t)};
}

enum class Layout { flat, boundary, duplicate, utf16, split, gap, empty };
struct Metadata { Region region; std::size_t page_size; };
struct State { std::uint64_t fingerprint; std::size_t cursor; };

// Only this controlled child writes its synthetic document. The scanner never
// writes target memory, injects input, attaches ptrace, or stops the process.
void run_child(int requests, int replies, std::size_t size, Layout layout) {
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<char*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(memory != MAP_FAILED, "mmap failed");
    std::fill_n(memory, size, 'x'); // Touch pages so benchmarks explicitly use resident memory.
    const auto marker_offset = layout == Layout::boundary ? page - 3 : size - page / 2;
    auto put = [&](std::size_t offset, std::string_view text) {
        require(offset <= size && text.size() <= size - offset, "invalid fixture position");
        std::memcpy(memory + offset, text.data(), text.size());
    };
    if (layout == Layout::utf16) {
        put(marker_offset + 1, utf16_anchor()); // Do not assume byte alignment in a scan.
    } else if (layout != Layout::empty) {
        put(marker_offset, anchor);
        if (layout == Layout::split) {
            put(page / 2, prefix); // Logical prefix lives in a separate piece.
        } else {
            put(marker_offset - prefix.size(), prefix);
        }
        if (layout == Layout::duplicate) {
            put(page / 2, anchor); // A stale/undo copy indistinguishable by anchor alone.
        }
    }
    if (layout == Layout::gap) {
        // Would form a false hit if a scanner concatenated noncontiguous pages.
        put(page - 3, anchor.substr(0, 3));
        put(2 * page, anchor.substr(3));
        require(::mprotect(memory + page, page, PROT_NONE) == 0, "mprotect failed");
    }
    std::size_t cursor = marker_offset + anchor.size();
    auto state = [&] {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i < size; ++i) {
            if (layout == Layout::gap && i >= page && i < 2 * page) continue;
            hash = (hash ^ static_cast<unsigned char>(memory[i])) * 1099511628211ULL;
        }
        return State{hash, cursor};
    };
    send(replies, Metadata{{reinterpret_cast<std::uintptr_t>(memory), size}, page});
    for (;;) {
        const auto command = receive<char>(requests);
        if (command == 'Q') break;
        if (command == 'H') {
            send(replies, state());
        } else if (command == 'C') {
            cursor = 0;
            send(replies, state());
        } else if (command == 'Z') {
            std::fill_n(memory + marker_offset, anchor.size(), 'x');
            send(replies, 'R');
        } else if (command == 'N') {
            require(::prctl(PR_SET_DUMPABLE, 0) == 0, "cannot restrict test child reads");
            send(replies, 'R');
        } else if (command == 'M') {
            const auto marker = receive<Marker>(requests);
            // Model an authoritative field plus a newly generated mirror.
            // Both copies have the same prefix and update on every insertion.
            put(page / 2 - prefix.size(), prefix);
            put(page / 2, marker_view(marker));
            put(marker_offset, marker_view(marker));
            cursor = marker_offset + marker.size();
            send(replies, 'R');
        } else if (command == 'P') {
            const auto start = std::chrono::steady_clock::now();
            put(marker_offset, anchor);
            send(replies, 'R');
            // A requested duration, not a guaranteed 1 ms lifetime. Measure it.
            timespec remaining{0, 1'000'000};
            while (::nanosleep(&remaining, &remaining) != 0) {
                if (errno != EINTR) throw std::system_error(errno, std::generic_category());
            }
            std::fill_n(memory + marker_offset, anchor.size(), 'x');
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            send(replies, elapsed);
        } else {
            throw std::runtime_error("invalid fixture command");
        }
    }
    ::munmap(memory, size);
}

class Fixture {
public:
    explicit Fixture(Layout layout = Layout::flat, std::size_t size = 64 * 1024) {
        std::array<int, 2> request_pipe{};
        std::array<int, 2> reply_pipe{};
        require(::pipe(request_pipe.data()) == 0, "pipe failed");
        Fd request_read(request_pipe[0]);
        requests_.reset(request_pipe[1]);
        require(::pipe(reply_pipe.data()) == 0, "pipe failed");
        replies_.reset(reply_pipe[0]);
        Fd reply_write(reply_pipe[1]);
        pid_ = ::fork();
        require(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            requests_.reset();
            replies_.reset();
            try {
                run_child(request_read.get(), reply_write.get(), size, layout);
                ::_exit(0);
            } catch (...) {
                ::_exit(2);
            }
        }
        request_read.reset();
        reply_write.reset();
        try {
            info_ = receive<Metadata>(replies_.get());
        } catch (...) {
            requests_.reset();
            int status{};
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
            throw;
        }
    }
    ~Fixture() {
        if (pid_ < 0) return;
        try { send(requests_.get(), 'Q'); } catch (...) {}
        requests_.reset();
        int status{};
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    [[nodiscard]] pid_t pid() const { return pid_; }
    [[nodiscard]] const Metadata& info() const { return info_; }
    State state(char command = 'H') {
        send(requests_.get(), command);
        return receive<State>(replies_.get());
    }
    void clear() {
        send(requests_.get(), 'Z');
        require(receive<char>(replies_.get()) == 'R', "clear acknowledgement");
    }
    void deny_reads() {
        send(requests_.get(), 'N');
        require(receive<char>(replies_.get()) == 'R', "read restriction acknowledgement");
    }
    void insert_fresh_copies(const Marker& marker) {
        send(requests_.get(), 'M');
        send(requests_.get(), marker);
        require(receive<char>(replies_.get()) == 'R', "marker copies acknowledgement");
    }
    void start_pulse() {
        send(requests_.get(), 'P');
        require(receive<char>(replies_.get()) == 'R', "pulse acknowledgement");
    }
    std::int64_t finish_pulse() { return receive<std::int64_t>(replies_.get()); }
private:
    Fd requests_;
    Fd replies_;
    pid_t pid_ = -1;
    Metadata info_{};
};

Limits unrestricted(Region region) { return {region.size, 5s, 16}; }

void self_test() {
    int tests = 0;
    auto test = [&](std::string_view title, auto body) {
        body();
        ++tests;
        std::cout << "PASS " << title << '\n';
    };
    test("read-only scan preserves target bytes and cursor", [] {
        Fixture target;
        const auto before = target.state();
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        const auto after = target.state();
        require(found.fully_read() && found.hits.size() == 1, "flat anchor");
        require(before.fingerprint == after.fingerprint && before.cursor == after.cursor,
                "scan changed synthetic target");
    });
    test("UTF-8 anchor across a remote page boundary", [] {
        Fixture target(Layout::boundary);
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        require(found.fully_read() && found.hits.size() == 1, "boundary anchor");
        require(found.hits.front() == target.info().region.begin + target.info().page_size - 3,
                "wrong boundary address");
    });
    test("UTF-16LE anchor including surrogate pair and embedded zero bytes", [] {
        Fixture target(Layout::utf16);
        const auto found = context_scan::scan(target.pid(), target.info().region, utf16_anchor(),
                                              unrestricted(target.info().region));
        require(found.fully_read() && found.hits.size() == 1, "UTF-16 anchor");
    });
    test("duplicate anchor stays ambiguous", [] {
        Fixture target(Layout::duplicate);
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        require(found.fully_read() && found.hits.size() == 2, "must preserve both copies");
    });
    test("fresh marker copied twice stays ambiguous after both copies update", [] {
        Fixture target;
        std::vector<std::uintptr_t> previous_hits;
        for (int round = 0; round < 2; ++round) {
            // Generated after fork; the child has no inherited marker to find.
            const auto marker = fresh_marker();
            const auto pattern = marker_view(marker);
            const auto before = context_scan::scan(target.pid(), target.info().region, pattern,
                                                    unrestricted(target.info().region));
            require(before.fully_read() && before.hits.empty(), "marker must be new in this mapping");
            target.insert_fresh_copies(marker);
            const auto after = context_scan::scan(target.pid(), target.info().region, pattern,
                                                   unrestricted(target.info().region));
            require(after.fully_read() && after.hits.size() == 2, "both new copies must remain candidates");
            const auto joined = std::string(prefix) + std::string(pattern);
            const auto with_prefix = context_scan::scan(target.pid(), target.info().region, joined,
                                                         unrestricted(target.info().region));
            require(with_prefix.fully_read() && with_prefix.hits.size() == 2,
                    "adjacent prefix must not disambiguate identical copies");
            if (round != 0) require(previous_hits == after.hits, "both candidates should track new markers");
            previous_hits = after.hits;
        }
    });
    test("unique stable bytes do not identify the current caret", [] {
        Fixture target;
        const auto before = target.state();
        const auto first = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        const auto after = target.state('C');
        const auto second = context_scan::scan(target.pid(), target.info().region, anchor,
                                               unrestricted(target.info().region));
        require(first.hits == second.hits && before.fingerprint == after.fingerprint,
                "text should be identical across cursor-only move");
        require(before.cursor != after.cursor, "fixture did not move its logical cursor");
    });
    test("noncontiguous document prefix cannot be recovered by adjacent bytes", [] {
        Fixture target(Layout::split);
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        const auto joined = context_scan::scan(target.pid(), target.info().region,
                                               std::string(prefix) + std::string(anchor),
                                               unrestricted(target.info().region));
        require(found.hits.size() == 1 && joined.fully_read() && joined.hits.empty(),
                "piece fixture must not create physically adjacent context");
    });
    test("unreadable page skipped without joining unrelated bytes", [] {
        Fixture target(Layout::gap);
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        require(found.stop == Stop::complete && found.unreadable_pages == 1 &&
                    !found.fully_read() && found.hits.size() == 1,
                "gap handling or coverage reporting");
    });
    test("byte budget stops before a late anchor and reports incomplete coverage", [] {
        Fixture target;
        const auto cap = target.info().page_size + 7;
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              Limits{cap, 5s, 16});
        require(found.stop == Stop::byte_budget && found.bytes_attempted == cap &&
                    found.hits.empty() && !found.fully_read(), "byte cap");
    });
    test("expired deadline performs no remote reads", [] {
        Fixture target;
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              Limits{target.info().region.size, 0us, 16});
        require(found.stop == Stop::time_budget && found.read_calls == 0, "time cap");
    });
    test("hit cap never implies a unique complete result", [] {
        Fixture target(Layout::duplicate);
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              Limits{target.info().region.size, 5s, 1});
        require(found.stop == Stop::hit_budget && found.hits.size() == 1 && !found.fully_read(),
                "hit cap");
    });
    test("observed bytes are revalidated after target changes", [] {
        Fixture target;
        const auto first = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        require(first.hits.size() == 1, "initial hit");
        target.clear();
        const Region tracked{first.hits.front(), anchor.size()};
        const auto again = context_scan::scan(target.pid(), tracked, anchor, unrestricted(tracked));
        require(again.fully_read() && again.hits.empty(), "stale hit must be invalidated");
    });
    test("empty region and invalid pattern handled explicitly", [] {
        const auto empty = context_scan::scan(::getpid(), {}, anchor);
        require(empty.fully_read() && empty.read_calls == 0, "empty range");
        bool rejected = false;
        try { (void)context_scan::scan(::getpid(), {}, ""); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "empty pattern not rejected");
    });
    test("permission denial stops immediately without changing system policy", [] {
        Fixture target;
        target.deny_reads();
        const auto found = context_scan::scan(target.pid(), target.info().region, anchor,
                                              unrestricted(target.info().region));
        require(found.stop == Stop::unavailable && found.error == EPERM &&
                    found.read_calls == 1 && found.hits.empty(), "permission denial");
    });
    std::cout << "PASS total=" << tests << '\n';
}

std::int64_t percentile(std::vector<std::int64_t> values, std::size_t percent) {
    std::ranges::sort(values);
    return values[(values.size() - 1) * percent / 100];
}

void report_scan(std::string_view scenario, std::size_t size, const Result& result) {
    std::cout << "{\"scenario\":\"" << scenario << "\",\"region_bytes\":" << size
              << ",\"elapsed_us\":" << result.elapsed.count()
              << ",\"bytes_attempted\":" << result.bytes_attempted
              << ",\"bytes_read\":" << result.bytes_read
              << ",\"read_calls\":" << result.read_calls
              << ",\"hits\":" << result.hits.size()
              << ",\"unreadable_pages\":" << result.unreadable_pages
              << ",\"stop\":\"" << context_scan::name(result.stop) << "\"}\n";
}

void benchmark() {
    constexpr auto size = std::size_t{64} * 1024 * 1024;
    Fixture target(Layout::flat, size);
    const auto region = target.info().region;
    const auto discovery = context_scan::scan(target.pid(), region, anchor, unrestricted(region));
    require(discovery.fully_read() && discovery.hits.size() == 1, "discovery failed");
    report_scan("resident_64MiB_known_mapping_discovery", size, discovery);
    const auto bounded = context_scan::scan(target.pid(), region, anchor);
    report_scan("256KiB_2ms_budget_late_anchor", size, bounded);
    const auto page = target.info().page_size;
    const Region tracked{discovery.hits.front() - discovery.hits.front() % page, page};
    std::vector<std::int64_t> timings;
    std::size_t tracked_hits = 0;
    for (int i = 0; i < 200; ++i) {
        const auto found = context_scan::scan(target.pid(), tracked, anchor);
        tracked_hits += found.fully_read() && found.hits.size() == 1;
        timings.push_back(found.elapsed.count());
    }
    std::cout << "{\"scenario\":\"tracked_page\",\"trials\":200,\"region_bytes\":" << page
              << ",\"captured\":" << tracked_hits
              << ",\"median_us\":" << percentile(timings, 50)
              << ",\"p95_us\":" << percentile(timings, 95)
              << ",\"max_us\":" << *std::ranges::max_element(timings) << "}\n";

    // Synthetic RAM-only pulse: no widget, rendering, IME, or Chromium is
    // involved. The notification gives the scanner favorable synchronization.
    // This experiment measures capture, never absence of GUI side effects.
    for (const auto& [label, selected] :
         {std::pair{"pulse_1ms_known_mapping", region},
          std::pair{"pulse_1ms_tracked_page", tracked}}) {
        target.clear();
        std::size_t captured = 0;
        timings.clear();
        std::vector<std::int64_t> lifetimes;
        constexpr int trials = 40;
        for (int i = 0; i < trials; ++i) {
            target.start_pulse();
            const auto found = context_scan::scan(target.pid(), selected, anchor, unrestricted(selected));
            captured += !found.hits.empty();
            timings.push_back(found.elapsed.count());
            lifetimes.push_back(target.finish_pulse());
        }
        std::cout << "{\"scenario\":\"" << label << "\",\"trials\":" << trials
                  << ",\"captured\":" << captured
                  << ",\"scan_median_us\":" << percentile(timings, 50)
                  << ",\"scan_p95_us\":" << percentile(timings, 95)
                  << ",\"lifetime_min_us\":" << *std::ranges::min_element(lifetimes)
                  << ",\"lifetime_median_us\":" << percentile(lifetimes, 50)
                  << ",\"lifetime_max_us\":" << *std::ranges::max_element(lifetimes) << "}\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    try {
        if (argc != 2) throw std::invalid_argument("usage: context_memory_scan --self-test|--benchmark");
        const std::string_view option(argv[1]);
        if (option == "--self-test") self_test();
        else if (option == "--benchmark") benchmark();
        else throw std::invalid_argument("unknown option");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

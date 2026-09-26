#include "anchor.hpp"
#include "scan.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Tests only use ASCII plus BMP code points, so this is enough.
std::u16string to_utf16(std::string_view input) {
    std::u16string output;
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first < 0x80) {
            output.push_back(static_cast<char16_t>(first));
            ++index;
        } else {
            const char32_t codepoint = static_cast<char32_t>(
                ((first & 0x0F) << 12) |
                ((static_cast<unsigned char>(input[index + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(input[index + 2]) & 0x3F));
            output.push_back(static_cast<char16_t>(codepoint));
            index += 3;
        }
    }
    return output;
}

struct Holder {
    pid_t pid = -1;
    std::uintptr_t address = 0;
};

// The child keeps the anchor text in a heap buffer and reports its address so
// the scanner can be pointed at exactly that buffer (the parent also holds a
// copy of the anchor in its own heap, which is what a real engine avoids by
// never scanning itself).
Holder spawn_holder(const std::string& text, bool utf16) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        std::uintptr_t address = 0;
        if (utf16) {
            [[maybe_unused]] auto* buffer = new std::u16string(to_utf16(text));
            address = reinterpret_cast<std::uintptr_t>(buffer->data());
        } else {
            [[maybe_unused]] auto* buffer = new std::string(text);
            address = reinterpret_cast<std::uintptr_t>(buffer->data());
        }
        [[maybe_unused]] const auto written = ::write(ready[1], &address, sizeof(address));
        ::pause();
        ::_exit(0);
    }
    ::close(ready[1]);
    std::uintptr_t address = 0;
    [[maybe_unused]] const auto got = ::read(ready[0], &address, sizeof(address));
    ::close(ready[0]);
    return {pid, address};
}

Holder spawn_misaligned_utf16_holder(std::string_view text) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        std::string buffer;
        buffer.reserve(256);
        const std::size_t padding = reinterpret_cast<std::uintptr_t>(buffer.data()) % 2 == 0 ? 1 : 2;
        buffer.append(padding, 'x');
        for (const char16_t unit : to_utf16(text)) {
            buffer.push_back(static_cast<char>(unit & 0xff));
            buffer.push_back(static_cast<char>(unit >> 8));
        }
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data() + padding);
        (void)::write(ready[1], &address, sizeof(address));
        ::pause();
        ::_exit(0);
    }
    ::close(ready[1]);
    std::uintptr_t address = 0;
    [[maybe_unused]] const auto got = ::read(ready[0], &address, sizeof(address));
    ::close(ready[0]);
    return {pid, address};
}

Holder spawn_large_holder(std::string_view text) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        constexpr std::size_t kOffset = 20 * 1024 * 1024;
        [[maybe_unused]] auto* buffer = new std::string(24 * 1024 * 1024, 'x');
        buffer->replace(kOffset, text.size(), text);
        const auto address = reinterpret_cast<std::uintptr_t>(buffer->data() + kOffset);
        (void)::write(ready[1], &address, sizeof(address));
        ::pause();
        ::_exit(0);
    }
    ::close(ready[1]);
    std::uintptr_t address = 0;
    [[maybe_unused]] const auto got = ::read(ready[0], &address, sizeof(address));
    ::close(ready[0]);
    return {pid, address};
}

void stop_holder(pid_t pid) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

struct ChangingHolder {
    Holder holder;
    int updates = -1;
    int acknowledgements = -1;
};

ChangingHolder spawn_changing_holder() {
    int commands[2], responses[2];
    if (::pipe(commands) != 0) return {};
    if (::pipe(responses) != 0) {
        ::close(commands[0]); ::close(commands[1]);
        return {};
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(commands[1]); ::close(responses[0]);
        std::string buffer;
        buffer.reserve(256);
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
            buffer = "document prefix ";
            if (step == '1') buffer += "ㄋ";
            if (step == '2') buffer += "ㄋㄧ";
            if (step == '3') buffer += "你";
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]); ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {{pid, address}, commands[1], responses[0]};
}

bool update_holder(const ChangingHolder& holder, char step) {
    char ack = 0;
    return ::write(holder.updates, &step, 1) == 1 &&
           ::read(holder.acknowledgements, &ack, 1) == 1 && ack == step;
}

const char* const kAnchor = "智慧錨點";

}  // namespace

int main() {
    using namespace llavon::memscan;

    AnchorError anchor_error;
    auto parse = [&](const std::string& text) { return parse_anchor(text, anchor_error); };
    check(parse(kAnchor).has_value(), "text anchor accepted");
    check(parse("ab").has_value(), "two code points accepted");
    check(parse("a").has_value(), "one natural phonetic code point accepted");
    check(!parse(std::string("a") + '\x01' + "b").has_value(), "control character rejected");
    check(!parse(std::string("\xff\xfe", 2)).has_value(), "invalid UTF-8 rejected");
    check(!parse(std::string(65, 'a')).has_value(), "65 code points rejected");

    const auto anchor = parse(kAnchor);
    if (!anchor) {
        std::printf("memscan tests failed: the anchor did not parse\n");
        return 1;
    }

    ScanLimits limits;
    limits.timeout = std::chrono::milliseconds(3000);
    const std::string text = std::string("hello magic ") + kAnchor + " seed line\n";

    for (const bool utf16 : {false, true}) {
        const Holder holder = spawn_holder(text, utf16);
        check(holder.pid > 0, std::string("holder spawned (") + (utf16 ? "utf16" : "utf8") + ")");
        if (holder.pid <= 0) continue;
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 256}};
        const auto match = scan_pid(holder.pid, *anchor, 64, 32, limits, hints, error);
        check(match.has_value(),
              std::string("scan finds the anchor (") + (utf16 ? "utf16" : "utf8") + "): " + error.code);
        if (match) {
            check(match->encoding == (utf16 ? Encoding::Utf16Le : Encoding::Utf8),
                  std::string("encoding reported: ") + llavon::memscan::encoding_name(match->encoding));
            check(match->before.ends_with("hello magic "), std::string("text before: ") + match->before);
            check(match->after.starts_with(" seed line"), std::string("text after: ") + match->after);
        }
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_misaligned_utf16_holder(kAnchor);
        check(holder.address % 2 == 1, "test UTF-16 bytes really start at an odd address");
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address - 16, holder.address + 64}};
        (void)scan_pid(holder.pid, *anchor, 64, 0, limits, hints, error, &candidates);
        check(std::ranges::none_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf16Le && item.address == holder.address;
              }), "unaligned UTF-16 byte coincidences are rejected");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_large_holder(std::string("document prefix ") + kAnchor);
        ScanLimits incremental = limits;
        incremental.max_bytes_per_pid = 4 * 1024 * 1024;
        std::uintptr_t cursor = 0;
        bool found = false, advanced = false;
        for (int attempt = 0; attempt < 16 && !found; ++attempt) {
            ScanError error;
            std::vector<Match> candidates;
            (void)scan_pid(holder.pid, *anchor, 64, 0, incremental, {}, error, &candidates, &cursor);
            advanced |= cursor != 0;
            found = std::ranges::any_of(candidates, [&](const Match& match) {
                return match.address == holder.address + std::string("document prefix ").size();
            });
        }
        check(advanced && found, "bounded scans resume instead of rereading the first region");
        stop_holder(holder.pid);
    }

    {
        const ChangingHolder changing = spawn_changing_holder();
        check(changing.holder.address != 0, "mutable client string has a stable address");
        std::uintptr_t previous = 0;
        for (const auto [key, text] : std::vector<std::pair<char, const char*>>{
                 {'1', "ㄋ"}, {'2', "ㄋㄧ"}, {'3', "你"}}) {
            check(update_holder(changing, key), "client changed its natural composition");
            const auto pattern = parse(text);
            ScanError error;
            std::vector<Match> candidates;
            const std::vector<Hint> hint{{changing.holder.pid, changing.holder.address,
                                          changing.holder.address + 256}};
            (void)scan_pid(changing.holder.pid, *pattern, 64, 0, limits, hint, error, &candidates);
            bool found = false;
            for (const auto& candidate : candidates) {
                if (candidate.before.ends_with("document prefix ") &&
                    candidate.address == changing.holder.address + std::string("document prefix ").size()) {
                    found = true;
                    if (previous) check(candidate.address == previous, "validated address remains stable");
                    previous = candidate.address;
                }
            }
            check(found, std::string("natural composition match: ") + text);
        }
        ::close(changing.updates); ::close(changing.acknowledgements);
        stop_holder(changing.holder.pid);
    }

    {
        // Two copies of the anchor: the first sits behind binary bytes, the
        // second behind readable text. The scan must report the readable one.
        const std::string repeated = std::string("\x01\x02", 2) + kAnchor + std::string("\x01\x02", 2) +
                                     "readable run before the anchor " + kAnchor + " tail";
        const Holder holder = spawn_holder(repeated, false);
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        const auto match = scan_pid(holder.pid, *anchor, 64, 32, limits, hints, error);
        check(match.has_value() && match->before.ends_with("readable run before the anchor "),
              std::string("the readable copy wins: ") + (match ? match->before : error.code));
        std::vector<Match> candidates;
        (void)scan_pid(holder.pid, *anchor, 64, 0, limits, hints, error, &candidates);
        bool has_readable = false, has_other = false;
        for (const auto& candidate : candidates) {
            has_readable |= candidate.before.ends_with("readable run before the anchor ");
            has_other |= candidate.address == holder.address + repeated.find(kAnchor);
        }
        check(has_readable && !has_other, "binary-adjacent matches do not exhaust candidate slots");
        stop_holder(holder.pid);
    }

    {
        const std::string competing = std::string("display copy before ") + kAnchor +
                                      std::string("\x01\x02", 2) +
                                      "readable run before the anchor " + kAnchor;
        const Holder holder = spawn_holder(competing, false);
        std::vector<Match> candidates;
        ScanError error;
        // A cached range can contain a stale/formatting copy while the real
        // document copy lives elsewhere. A nearby hit is not sufficient to
        // skip the full scan unless both address and prefix still match.
        const auto first_offset = competing.find(kAnchor);
        const auto second_offset = competing.find(kAnchor, first_offset + 1);
        const Hint misleading{holder.pid, holder.address,
                              holder.address + first_offset + std::string(kAnchor).size(),
                              holder.address + first_offset, "unrelated prefix"};
        (void)scan_pid(holder.pid, *anchor, 64, 0, limits, {misleading}, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == holder.address + second_offset &&
                         item.before.ends_with("readable run before the anchor ");
              }), "a misleading hinted copy falls back to the document copy");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(text, false);
        ScanError error;
        const std::vector<Hint> hints{{holder.pid, 0x1000, 0x2000}};
        const auto match = scan_pid(holder.pid, *anchor, 64, 32, limits, hints, error);
        check(match.has_value(), "bogus hint does not break the scan");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(text, false);
        ScanLimits tight = limits;
        tight.timeout = std::chrono::milliseconds(0);
        ScanError error;
        const auto match = scan_pid(holder.pid, *anchor, 64, 32, tight, {}, error);
        check(!match.has_value() && error.code == "timeout", "timeout budget enforced");
        stop_holder(holder.pid);
    }

    if (::getuid() != 0) {
        ScanError error;
        const auto match = scan_pid(1, *anchor, 64, 32, limits, {}, error);
        check(!match.has_value() && error.code == "foreign-pid", "root process refused");
        check(!same_uid(1), "same_uid rejects root");
    }

    if (failures == 0) {
        std::printf("memscan tests passed\n");
        return 0;
    }
    std::printf("memscan tests failed: %d\n", failures);
    return 1;
}

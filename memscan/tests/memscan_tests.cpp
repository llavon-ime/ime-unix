#include "anchor.hpp"
#include "scan.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
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

Holder spawn_guarded_holder(const std::string& text) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        auto* pages = static_cast<char*>(::mmap(nullptr, 8192, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (pages == MAP_FAILED || ::mprotect(pages, 4096, PROT_NONE) != 0) ::_exit(1);
        std::copy(text.begin(), text.end(), pages + 4096);
        const auto address = reinterpret_cast<std::uintptr_t>(pages + 4096);
        (void)::write(ready[1], &address, sizeof(address));
        ::pause();
        ::_exit(0);
    }
    ::close(ready[1]);
    std::uintptr_t address = 0;
    (void)::read(ready[0], &address, sizeof(address));
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

std::vector<char32_t> to_codepoints(std::string_view input) {
    std::vector<char32_t> output;
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first < 0x80) {
            output.push_back(static_cast<char32_t>(first));
            ++index;
        } else if ((first & 0xE0) == 0xC0) {
            output.push_back(static_cast<char32_t>(
                ((first & 0x1F) << 6) | (static_cast<unsigned char>(input[index + 1]) & 0x3F)));
            index += 2;
        } else {
            output.push_back(static_cast<char32_t>(
                ((first & 0x0F) << 12) |
                ((static_cast<unsigned char>(input[index + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(input[index + 2]) & 0x3F)));
            index += 3;
        }
    }
    return output;
}

// A terminal grid: 12-byte cells holding a UTF-32 code point and attributes.
// Wide characters repeat the code point in a continuation cell.
Holder spawn_cell12_holder(std::string_view prefix, std::string_view text,
                            bool noisy_heap_boundary = false, bool foot_clean = false) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        auto append_cell = [foot_clean](std::string& out, char32_t codepoint, bool continuation) {
            // Foot 1.21's offset8 bit0 is attrs.clean, NOT Kitty continuation.
            // Its wide-glyph spacer is CELL_SPACER from the retained terminal.h.
            if (foot_clean && continuation) codepoint = 0x40200000u;
            for (int shift = 0; shift < 32; shift += 8) {
                out.push_back(static_cast<char>((codepoint >> shift) & 0xff));
            }
            const unsigned char attributes[8] = {
                0x00, 0x00, 0x0e, 0x00,
                static_cast<unsigned char>(foot_clean || continuation ? 0x01 : 0x00), 0x04, 0x00, 0x00};
            out.append(reinterpret_cast<const char*>(attributes), 8);
        };
        std::string buffer;
        buffer.reserve(4096);
        // Explicitly represent the preceding cleared grid cell; this is a
        // decoder test, not a claim that heap allocation proves row boundary.
        if (foot_clean) append_cell(buffer, 0, false);
        if (noisy_heap_boundary) {
            // Three binary cells, then a coincidental valid CJK code point
            // with heap attributes, just before the actual screen row.
            for (int index = 0; index < 3; ++index) {
                buffer.append(4, static_cast<char>(0xFE));
                buffer.append(8, static_cast<char>(0x5A));
            }
            buffer.append("\x80\x7f\0\0\x61\x03\0\0\0\0\0\0", 12);
        }
        for (const char32_t codepoint : to_codepoints(prefix)) {
            append_cell(buffer, codepoint, false);
            if (codepoint > 0x2000) append_cell(buffer, codepoint, true);
        }
        for (const char32_t codepoint : to_codepoints(text)) {
            append_cell(buffer, codepoint, false);
            append_cell(buffer, codepoint, true);
        }
        for (int empty = 0; empty < 4; ++empty) append_cell(buffer, 0, false);
        [[maybe_unused]] auto* heap = new std::string(std::move(buffer));
        const auto address = reinterpret_cast<std::uintptr_t>(heap->data());
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

// Konsole-style 16-byte cells: code point plus twelve attribute bytes. A wide
// character is followed by a continuation cell; the real client marks it as an
// unreal cell (zero code point, both extra-flag bytes clear), older rows use a
// blank cell.
Holder spawn_cell16_holder(std::string_view prefix, std::string_view text, bool unreal = false) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        auto append_cell = [unreal](std::string& out, char32_t codepoint, bool wide) {
            for (int shift = 0; shift < 32; shift += 8) {
                out.push_back(static_cast<char>((codepoint >> shift) & 0xff));
            }
            const unsigned char attributes[12] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                  0x01, 0x01, 0x00, 0x00, 0x01, 0x00};
            out.append(reinterpret_cast<const char*>(attributes), 12);
            if (wide) {
                if (unreal) {
                    out.append(4, '\0');
                    out.append(reinterpret_cast<const char*>(attributes), 10);
                    out.append(2, '\0');
                } else {
                    out.push_back(0x20);
                    out.append(3, '\0');
                    out.append(reinterpret_cast<const char*>(attributes), 12);
                }
            }
        };
        std::string buffer;
        buffer.reserve(4096);
        for (const char32_t codepoint : to_codepoints(prefix)) {
            append_cell(buffer, codepoint, codepoint > 0x2000);
        }
        for (const char32_t codepoint : to_codepoints(text)) {
            append_cell(buffer, codepoint, true);
        }
        [[maybe_unused]] auto* heap = new std::string(std::move(buffer));
        const auto address = reinterpret_cast<std::uintptr_t>(heap->data());
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

// VTE keeps its screen as 20-byte cells: a UTF-32 code point plus a 16-byte
// attribute record. A wide character repeats the code point in a fragment
// cell, flagged in the low attribute byte (bit 4).
Holder spawn_cell20_holder(std::string_view prefix, std::string_view text) {
    int ready[2];
    if (::pipe(ready) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(ready[0]);
        auto append_cell = [](std::string& out, char32_t codepoint, bool fragment) {
            for (int shift = 0; shift < 32; shift += 8) {
                out.push_back(static_cast<char>((codepoint >> shift) & 0xff));
            }
            unsigned char attributes[16] = {0x01, 0x00, 0x00, 0x00};
            if (fragment) attributes[0] = 0x11;
            out.append(reinterpret_cast<const char*>(attributes), 16);
        };
        std::string buffer;
        buffer.reserve(4096);
        for (const char32_t codepoint : to_codepoints(prefix)) {
            append_cell(buffer, codepoint, false);
            if (codepoint > 0x2000) append_cell(buffer, codepoint, true);
        }
        for (const char32_t codepoint : to_codepoints(text)) {
            append_cell(buffer, codepoint, false);
            append_cell(buffer, codepoint, true);
        }
        for (int empty = 0; empty < 4; ++empty) append_cell(buffer, 0, false);
        [[maybe_unused]] auto* heap = new std::string(std::move(buffer));
        const auto address = reinterpret_cast<std::uintptr_t>(heap->data());
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
        // U+4E00 begins with a zero byte in UTF-16LE and UTF-32LE. The
        // fast nonzero-byte search must retain the zero-leading fallback.
        const auto zero_lead = parse("一錨");
        const Holder holder = spawn_holder("hello magic 一錨 seed line", true);
        ScanError error;
        const auto match = scan_pid(holder.pid, *zero_lead, 64, 32, limits,
                                    {{holder.pid, holder.address, holder.address + 256}}, error);
        check(match && match->encoding == Encoding::Utf16Le &&
                  match->before.ends_with("hello magic "),
              "UTF-16 anchor with a zero leading byte is found");
        stop_holder(holder.pid);
    }

    {
        const std::string bare = std::string("\x01\x02", 2) + kAnchor;
        const Holder holder = spawn_holder(bare, false);
        ScanError error;
        ScanStats stats;
        ScanLimits bounded = limits;
        bounded.max_bytes_per_pid = bare.size();
        std::vector<Match> candidates;
        const std::vector<Hint> hint{{holder.pid, holder.address,
                                      holder.address + bare.size()}};
        (void)scan_pid(holder.pid, *anchor, 64, 0, bounded, hint, error,
                       &candidates, nullptr, &stats);
        check(stats.hits > 0 && stats.qualified == 0 && candidates.empty(),
              "a byte hit without a readable prefix is reported but not accepted");
        stop_holder(holder.pid);
    }

    {
        // A multi-anchor request scans the composition and the committed text
        // together and tags every candidate with the anchor that found it.
        const Holder holder = spawn_cell12_holder("document prefix ", "你");
        const auto anchor_a = parse("你");
        const auto anchor_b = parse("prefix");
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<AnchorRequest> requests{{&*anchor_a, nullptr}, {&*anchor_b, nullptr}};
        scan_pid_anchors(holder.pid, requests, 256, 16, limits,
                         {{holder.pid, holder.address, holder.address + 512}}, error, &candidates,
                         nullptr, nullptr, false);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.anchor_index == 0 && item.before.ends_with("document prefix ");
              }), "the composition anchor is tagged index 0");
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.anchor_index == 1 && item.before.ends_with("document ");
              }), "the committed-text anchor is tagged index 1");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_cell12_holder("doc text ", kAnchor);
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf32Cell12Le &&
                         item.address == holder.address + 9 * 12 &&
                         item.before.ends_with("doc text ");
              }), "a 12-byte cell grid is found with the text before the composition");
        check(std::ranges::none_of(candidates, [&](const Match& item) {
                  return item.address == holder.address + 10 * 12;
              }), "a wide character's continuation cell is not a composition start");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_cell12_holder("test input prefix 你", "ㄋㄧㄣ", false, true);
        const auto foot_anchor = parse("ㄋㄧㄣ");
        ScanError error;
        std::vector<Match> candidates;
        (void)scan_pid(holder.pid, *foot_anchor, 512, 16, limits,
                       {{holder.pid, holder.address, holder.address + 1024}}, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::FootCell12Le &&
                         item.before == "test input prefix 你";
              }), "Foot clean cells preserve every prefix character instead of Kitty flag filtering");
        stop_holder(holder.pid);
    }

    {
        // Konsole keeps its screen as 16-byte cells; the composition must be
        // found with the document text in front of it.
        const Holder holder = spawn_cell16_holder("doc text ", kAnchor);
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf32Cell16Le &&
                         item.address == holder.address + 9 * 16 &&
                         item.before.ends_with("doc text ");
              }), "a 16-byte cell grid is found with the text before the composition");
        stop_holder(holder.pid);
    }

    {
        // A grid containing only the first character of the anchor is not a
        // hit, even if it has a convincing document prefix. In particular,
        // wide-cell layouts must verify the whole anchor before ranking it.
        const Holder holder = spawn_cell16_holder("doc text ", "智錯");
        ScanError error;
        std::vector<Match> candidates;
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits,
                       {{holder.pid, holder.address, holder.address + 512}}, error, &candidates);
        check(std::ranges::none_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf32Cell16Le &&
                         item.address == holder.address + 9 * 16;
              }), "a matching first cell with a different tail is rejected");
        stop_holder(holder.pid);
    }

    {
        // A repeated wide character in the anchor is a real character, not a
        // continuation cell: "你好好" has to verify completely in every grid.
        const auto repeated = parse("你好好");
        for (const auto& [holder, cell] : std::initializer_list<std::pair<Holder, std::size_t>>{
                 {spawn_cell12_holder("doc text ", "你好好"), 12},
                 {spawn_cell16_holder("doc text ", "你好好"), 16},
                 {spawn_cell20_holder("doc text ", "你好好"), 20}}) {
            ScanError error;
            std::vector<Match> candidates;
            const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
            (void)scan_pid(holder.pid, *repeated, 256, 16, limits, hints, error, &candidates);
            check(std::ranges::any_of(candidates, [&](const Match& item) {
                      return item.address == holder.address + 9 * cell &&
                             item.before.ends_with("doc text ");
                  }), "a repeated wide character is not skipped as a continuation");
            stop_holder(holder.pid);
        }
    }
    {
        // Heap metadata next to a kitty row can decode to a valid CJK code
        // point. Its attributes differ from the actual row and it must not
        // leak into the context or increase the candidate confidence.
        const Holder holder = spawn_cell12_holder("doc text ", kAnchor, true);
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == holder.address + 4 * 12 + 9 * 12 &&
                         item.encoding == Encoding::Utf32Cell12Le && item.before == "doc text ";
              }), "an orphan heap cell does not become kitty context");
        stop_holder(holder.pid);
    }

    {
        // VTE keeps its screen as 20-byte cells; the composition must be
        // found with the document text in front of it.
        const Holder holder = spawn_cell20_holder("doc text ", kAnchor);
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf32Cell20Le &&
                         item.address == holder.address + 9 * 20 &&
                         item.before.ends_with("doc text ");
              }), "a 20-byte cell grid is found with the text before the composition");
        stop_holder(holder.pid);
    }
    {
        // Matching the repeated code point in VTE's fragment cell must not
        // make the composing character itself part of the document context.
        const Holder holder = spawn_cell20_holder("doc text ", "你");
        const auto one_char = parse("你");
        ScanError error;
        std::vector<Match> candidates;
        (void)scan_pid(holder.pid, *one_char, 256, 16, limits,
                       {{holder.pid, holder.address, holder.address + 512}}, error, &candidates);
        const bool correct = std::ranges::any_of(candidates, [&](const Match& item) {
                   return item.encoding == Encoding::Utf32Cell20Le &&
                          item.address == holder.address + 9 * 20 &&
                          item.before.ends_with("doc text ");
               }) && std::ranges::none_of(candidates, [&](const Match& item) {
                   return item.encoding == Encoding::Utf32Cell20Le &&
                          item.address == holder.address + 10 * 20;
               });
        if (!correct) {
            std::fprintf(stderr, "VTE fragment pid=%d base=0x%lx count=%zu error=%s\n",
                         static_cast<int>(holder.pid), static_cast<unsigned long>(holder.address),
                         candidates.size(), error.code.c_str());
            for (const auto& item : candidates) {
                std::fprintf(stderr, "  address=0x%lx encoding=%s before=%s\n",
                             static_cast<unsigned long>(item.address), encoding_name(item.encoding),
                             item.before.c_str());
            }
        }
        check(correct, "VTE fragment cannot start a composition match");
        stop_holder(holder.pid);
    }

    {
        // VTE repeats the code point in the fragment cell of a wide glyph;
        // decoding it twice would duplicate every CJK character in front of
        // the composition ("文件" not "文文件件").
        const Holder holder = spawn_cell20_holder("這是一段測試文件 ", kAnchor);
        const auto anchor_address = holder.address + 17 * 20;
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 512, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == anchor_address && item.before.ends_with("文件 ") &&
                         item.before.find("文文") == std::string::npos;
              }), "VTE fragment cells are not decoded twice");
        stop_holder(holder.pid);
    }

    {
        // Konsole marks the right half of a wide glyph as an unreal cell: a
        // zero code point with both extra-flag bytes clear. The text in front
        // of the composition must not be cut at it.
        const Holder holder = spawn_cell16_holder("這是一段測試文件 ", kAnchor, true);
        const auto anchor_address = holder.address + 17 * 16;
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == anchor_address && item.before.ends_with("文件 ");
              }), "Konsole unreal placeholder cells do not cut the context");
        stop_holder(holder.pid);
    }

    {
        // Wide characters occupy two cells; the continuation must not be
        // decoded as a second character ("文件" not "文文件件").
        const Holder holder = spawn_cell12_holder("這是一段測試文件 ", kAnchor);
        const auto anchor_address = holder.address + 17 * 12;
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == anchor_address && item.before.ends_with("文件 ") &&
                         item.before.find("文文") == std::string::npos &&
                         item.before.find("這這") == std::string::npos;
              }), "cell-grid continuation cells are not decoded twice");
        stop_holder(holder.pid);
    }

    {
        // A prefix of a longer word in a path or message must not count as a
        // caret hit, but it is also exactly what a caret in the middle of
        // existing text looks like. The scanner keeps it as a continuation
        // candidate; the provider only trusts it when the following text
        // stays the same across composition states. A unique anchor keeps the
        // child's inherited heap copies from filling the candidate budget.
        const auto continuation_anchor = parse("錨點連續字");
        check(continuation_anchor.has_value(), "continuation anchor parses");
        const Holder holder =
            spawn_holder(std::string("doc prefix ") + "錨點連續字" + "續", false);
        const auto anchor_address = holder.address + std::string_view("doc prefix ").size();
        ScanError error;
        ScanStats stats;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 256}};
        (void)scan_pid(holder.pid, *continuation_anchor, 256, 16, limits, hints, error, &candidates,
                       nullptr, &stats);
        check(stats.hits > 0 &&
                  std::ranges::any_of(candidates, [&](const Match& item) {
                      return item.address == anchor_address && item.continuation;
                  }),
              "a match continuing as a longer word is kept as a continuation candidate");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_holder(std::string("doc prefix ") + kAnchor + " 續", false);
        const auto anchor_address = holder.address + std::string_view("doc prefix ").size();
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 256}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == anchor_address && item.encoding == Encoding::Utf8 &&
                         item.confidence < 32;
              }), "a match at a word boundary stays a candidate without the grid bonus");
        stop_holder(holder.pid);
    }

    {
        const Holder holder = spawn_cell12_holder("doc text ", "智a慧錨點");
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512}};
        (void)scan_pid(holder.pid, *anchor, 64, 0, limits, hints, error, &candidates);
        check(std::ranges::none_of(candidates, [&](const Match& item) {
                  return item.encoding == Encoding::Utf32Cell12Le;
              }), "a grid cell whose continuation is not the composition is rejected");
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
        const auto large_anchor = parse("巨量錨點字");
        check(large_anchor.has_value(), "large scan anchor parses");
        const Holder holder =
            spawn_large_holder(std::string("document prefix ") + "巨量錨點字" + " ");
        ScanLimits incremental = limits;
        incremental.max_bytes_per_pid = 4 * 1024 * 1024;
        std::uintptr_t cursor = 0;
        bool found = false, advanced = false;
        for (int attempt = 0; attempt < 16 && !found; ++attempt) {
            ScanError error;
            std::vector<Match> candidates;
            (void)scan_pid(holder.pid, *large_anchor, 64, 16, incremental, {}, error, &candidates,
                           &cursor);
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
        for (const auto [key, composition] : std::vector<std::pair<char, const char*>>{
                 {'1', "ㄋ"}, {'2', "ㄋㄧ"}, {'3', "你"}}) {
            check(update_holder(changing, key), "client changed its natural composition");
            const auto pattern = parse(composition);
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
            check(found, std::string("natural composition match: ") + composition);
        }
        ::close(changing.updates); ::close(changing.acknowledgements);
        stop_holder(changing.holder.pid);
    }

    {
        // Repeated probes only need the pages the client wrote since the last
        // reset; the rest of the address space is skipped.
        const ChangingHolder changing = spawn_changing_holder();
        check(changing.holder.address != 0, "dirty tracking holder spawned");
        check(update_holder(changing, '1'), "client wrote its first composition");
        const auto first = parse("ㄋ");
        const std::string anchor_prefix = "document prefix ";
        const auto anchor_address = changing.holder.address + anchor_prefix.size();
        ScanError error;
        ScanStats stats;
        std::vector<Match> candidates;
        (void)scan_pid(changing.holder.pid, *first, 64, 16, limits, {}, error, &candidates,
                       nullptr, &stats);
        const auto full = std::ranges::find_if(candidates, [&](const Match& item) {
            return item.address == anchor_address;
        });
        check(full != candidates.end(), "full scan finds the initial composition");
        const std::size_t full_bytes = full != candidates.end() ? full->scanned_bytes : 0;
        check(llavon::memscan::reset_soft_dirty(changing.holder.pid), "soft-dirty reset is allowed");
        candidates.clear();
        (void)scan_pid(changing.holder.pid, *first, 64, 16, limits, {}, error, &candidates,
                       nullptr, &stats, true);
        check(candidates.empty(), "an unchanged process has no dirty pages");
        check(update_holder(changing, '2'), "client changed its composition");
        const auto second = parse("ㄋㄧ");
        candidates.clear();
        (void)scan_pid(changing.holder.pid, *second, 64, 16, limits, {}, error, &candidates,
                       nullptr, &stats, true);
        const auto dirty = std::ranges::find_if(candidates, [&](const Match& item) {
            return item.address == anchor_address;
        });
        check(dirty != candidates.end() && dirty->scanned_bytes < full_bytes,
              "a dirty scan reads only the pages the client wrote");
        ::close(changing.updates); ::close(changing.acknowledgements);
        stop_holder(changing.holder.pid);
    }

    {
        // A multi-anchor changed-only scan reads the same dirty set for every
        // anchor: the committed text and the composition that follows it can
        // both be found by one request. The soft-dirty baseline is consumed
        // once, after all anchors were scanned.
        const ChangingHolder changing = spawn_changing_holder();
        const auto composition = parse("ㄋ");
        const auto document = parse("prefix");
        (void)reset_soft_dirty(changing.holder.pid);
        check(update_holder(changing, '1'), "the client wrote the new composition");
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<AnchorRequest> requests{{&*composition, nullptr}, {&*document, nullptr}};
        scan_pid_anchors(changing.holder.pid, requests, 256, 16, limits, {}, error, &candidates,
                         nullptr, nullptr, true);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.anchor_index == 0 && item.before.ends_with("document prefix ");
              }), "a changed-only multi-anchor scan finds the composition");
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.anchor_index == 1 && item.before.ends_with("document ");
              }), "the same dirty set still holds the document text for the other anchor");
        ::close(changing.updates);
        ::close(changing.acknowledgements);
        stop_holder(changing.holder.pid);
    }

    {
        // A hint whose pages were not written since the last reset is still
        // read directly: the committed text never changes, so it must stay
        // findable by a changed-only scan.
        const Holder holder = spawn_cell12_holder("doc text ", kAnchor);
        (void)reset_soft_dirty(holder.pid);
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, holder.address, holder.address + 512,
                                       holder.address + 9 * 12, "doc text "}};
        (void)scan_pid(holder.pid, *anchor, 256, 16, limits, hints, error, &candidates, nullptr,
                       nullptr, true);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == holder.address + 9 * 12 &&
                         item.before.ends_with("doc text ");
              }), "a hint is read even when its pages were not written");
        stop_holder(holder.pid);
    }

    {
        // The matched cell can be on a dirty page while the beginning of its
        // row is clean. A scan range starting inside the row must not truncate
        // the prefix read after the anchor was found.
        const Holder holder = spawn_cell20_holder("document prefix ", kAnchor);
        const auto address = holder.address + 16 * 20;
        ScanError error;
        std::vector<Match> candidates;
        const std::vector<Hint> hints{{holder.pid, address - 13 * 20, address + 20}};
        (void)scan_pid(holder.pid, *anchor, 512, 0, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == address && item.before.ends_with("document prefix ");
              }), "a narrow scan range reads the complete prefix across its start");
        stop_holder(holder.pid);
    }
    {
        // A validated hint can extend into an unmapped page. The scanner
        // still has to reach the adjacent mapped page holding the document.
        const std::string guarded_text = std::string("document prefix ") + kAnchor;
        const Holder holder = spawn_guarded_holder(guarded_text);
        ScanError error;
        std::vector<Match> candidates;
        const auto address = holder.address + std::string_view("document prefix ").size();
        const std::vector<Hint> hints{{holder.pid, address - 8192, address + 4096}};
        (void)scan_pid(holder.pid, *anchor, 256, 0, limits, hints, error, &candidates);
        check(std::ranges::any_of(candidates, [&](const Match& item) {
                  return item.address == address && item.before.ends_with("document prefix ");
              }), "an unmapped hint page cannot hide an adjacent document");
        stop_holder(holder.pid);
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
        const bool found_document = std::ranges::any_of(candidates, [&](const Match& item) {
                   return item.address == holder.address + second_offset &&
                          item.before.ends_with("readable run before the anchor ");
               });
        if (!found_document) {
            std::fprintf(stderr, "hint test: pid=%d document=0x%lx candidates=%zu error=%s\n",
                         static_cast<int>(holder.pid),
                         static_cast<unsigned long>(holder.address + second_offset),
                         candidates.size(), error.code.c_str());
            for (const auto& item : candidates) {
                std::fprintf(stderr, "  address=0x%lx encoding=%s before=%s\n",
                             static_cast<unsigned long>(item.address), encoding_name(item.encoding),
                             item.before.c_str());
            }
        }
        check(found_document, "a misleading hinted copy falls back to the document copy");
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

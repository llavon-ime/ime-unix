#include "scan.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace llavon::memscan {

namespace {

constexpr std::size_t kChunkBytes = 4 * 1024 * 1024;
// Bound the locations sent to temporal verification; duplicate screen and log
// copies must not fill these slots before the document buffer is examined.
constexpr std::size_t kMaxCandidates = 12;
// A window with at least this many trailing printable bytes is considered a
// good sample and ends the scan; shorter runs are kept as the best candidate
// while the scan continues (protocol buffers, layout caches and other copies
// of the composition can have unrelated bytes in front of them).
constexpr int kGoodTextRun = 8;

struct TextRun {
    int length = 0;
    // A run of identical or consecutive code points is not document text;
    // both come from reading a byte stream with the wrong width (repeated
    // ASCII pairs decode to one repeated CJK code point, sequential bytes
    // decode to sequential code points). Such a window is a misread of some
    // other buffer, never the caret's context.
    bool suspicious = false;
};

TextRun trailing_text_run(const std::string& text) {
    TextRun run;
    std::vector<char32_t> codepoints;
    if (!decode_text(text, codepoints)) return run;
    for (auto it = codepoints.rbegin(); it != codepoints.rend(); ++it) {
        if (!plausible_text_codepoint(*it)) break;
        ++run.length;
    }
    const std::size_t start = codepoints.size() - static_cast<std::size_t>(run.length);
    // Only the code points nearest the composition matter. ASCII runs are
    // left alone (separator lines and padding are real text); a long run of
    // identical or sequential non-ASCII code points is a byte stream read
    // with the wrong width, never a caret's context.
    constexpr std::size_t kExaminedTail = 32;
    const std::size_t examined = std::min<std::size_t>(run.length, kExaminedTail);
    const std::size_t examined_start = codepoints.size() - examined;
    std::size_t identical = 1;
    std::size_t consecutive = 1;
    for (std::size_t index = std::max(start, examined_start) + 1; index < codepoints.size();
         ++index) {
        const bool non_ascii = codepoints[index] >= 0x80;
        if (non_ascii && codepoints[index] == codepoints[index - 1]) {
            if (++identical >= 8) run.suspicious = true;
        } else {
            identical = 1;
        }
        if (non_ascii && codepoints[index] == codepoints[index - 1] + 1) {
            if (++consecutive >= 6) run.suspicious = true;
        } else {
            consecutive = 1;
        }
    }
    return run;
}

constexpr std::size_t kPageBytes = 4096;

// A real screen grid repeats its cell attributes; a denser buffer read with a
// wider stride stores the skipped characters there instead, so the attribute
// region varies from cell to cell. Only the cells that carry the decoded text
// run are examined, so heap bytes in front of the buffer cannot mark a real
// grid as a misread.
bool cell_attributes_vary(const std::vector<std::byte>& raw, std::size_t cell,
                          std::size_t run_length) {
    if (cell <= 4 || run_length < 4) return false;
    const std::size_t available = raw.size() / cell;
    const std::size_t cells = std::min({available, run_length, std::size_t{12}});
    if (cells < 4) return false;
    std::vector<std::string_view> patterns;
    patterns.reserve(cells);
    for (std::size_t index = 0; index < cells; ++index) {
        const auto* base = reinterpret_cast<const char*>(raw.data()) +
                           (available - cells + index) * cell + 4;
        patterns.emplace_back(base, cell - 4);
    }
    std::ranges::sort(patterns);
    const std::size_t distinct =
        static_cast<std::size_t>(std::unique(patterns.begin(), patterns.end()) - patterns.begin());
    // Real grids repeat a handful of attribute patterns (main cells, wide
    // continuations, a few styles); a misread gives almost every cell its own
    // pattern because the attribute bytes are the skipped characters.
    return distinct * 4 > cells * 3;
}

struct Deadline {
    std::chrono::steady_clock::time_point at;
    bool expired() const { return std::chrono::steady_clock::now() >= at; }
};

std::optional<std::uintptr_t> parse_hex(std::string_view text) {
    std::uintptr_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
    return value;
}

// Pages written since the last soft-dirty reset. The kernel exposes this
// without privileges for the process owner, which lets repeated probes read
// only what the client touched instead of its whole writable address space.
std::vector<Region> dirty_pages(pid_t pid, const std::vector<Region>& regions) {
#ifdef LLAVON_IME_DEBUG
    const auto debug_start = std::chrono::steady_clock::now();
#endif
    // pread, not streams: /proc/<pid>/pagemap reports size 0, so stdio seeks
    // past the start fail.
    const int pagemap = ::open(("/proc/" + std::to_string(pid) + "/pagemap").c_str(),
                               O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) return {};
    // One pread per page costs more than the scan itself; read whole blocks
    // of page-table entries at once.
    constexpr std::size_t kEntriesPerRead = 8192;
    std::vector<std::uint64_t> entries(kEntriesPerRead);
    std::vector<Region> dirty;
    for (const auto& region : regions) {
        std::uintptr_t run_start = 0;
        std::uintptr_t run_end = 0;
        const auto flush = [&] {
            if (run_end <= run_start) return;
            // Include a little overlap so a pattern spanning a page boundary
            // is still complete.
            const std::uintptr_t start = run_start >= 64 ? run_start - 64 : 0;
            dirty.push_back(Region{start, std::min<std::uintptr_t>(run_end + 64, region.end),
                                   true, region.main_heap});
            run_start = run_end = 0;
        };
        const std::uintptr_t first_page = region.start & ~(kPageBytes - 1);
        const std::size_t page_count = (region.end - first_page + kPageBytes - 1) / kPageBytes;
        for (std::size_t index = 0; index < page_count; index += kEntriesPerRead) {
            const std::size_t count = std::min(kEntriesPerRead, page_count - index);
            const off_t offset =
                static_cast<off_t>(((first_page / kPageBytes) + index) * sizeof(std::uint64_t));
            const ssize_t got = ::pread(pagemap, entries.data(), count * sizeof(std::uint64_t), offset);
            if (got <= 0) break;
            const std::size_t read_count = static_cast<std::size_t>(got) / sizeof(std::uint64_t);
            for (std::size_t entry_index = 0; entry_index < read_count; ++entry_index) {
                const std::uintptr_t page = first_page + (index + entry_index) * kPageBytes;
                if (page >= region.end) break;
                const bool written = (entries[entry_index] & (1ull << 55)) != 0;
                if (!written) {
                    flush();
                    continue;
                }
                const std::uintptr_t page_end = std::min(page + kPageBytes, region.end);
                if (run_start == 0) run_start = std::max(page, region.start);
                run_end = page_end;
            }
        }
        flush();
    }
    ::close(pagemap);
#ifdef LLAVON_IME_DEBUG
    const auto debug_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - debug_start)
                              .count();
    std::fprintf(stderr, "[dirty] ranges=%zu elapsed=%lldms\n", dirty.size(),
                 static_cast<long long>(debug_ms));
#endif
    return dirty;
}

std::vector<Region> read_regions(pid_t pid, std::uintptr_t resume_address = 0) {
    std::vector<Region> regions;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range;
        std::string perms;
        if (!(stream >> range >> perms)) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        const auto start = parse_hex(std::string_view(range).substr(0, dash));
        const auto end = parse_hex(std::string_view(range).substr(dash + 1));
        if (!start || !end || *end <= *start) continue;
        if (perms.size() < 3 || perms[0] != 'r') continue;

        std::string offset;
        std::string device;
        std::string inode;
        std::string path;
        stream >> offset >> device >> inode;
        std::getline(stream, path);
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        const bool special = path.starts_with("[vvar]") || path.starts_with("[vdso]") ||
                             path.starts_with("[vsyscall]");
        if (special) continue;
        // The composition is written by the client, so it can only be in
        // writable memory: anonymous pages, the heap, the stack, or writable
        // shared mappings (Chromium and other toolkits keep text in those).
        // Read-only pages are skipped; they cannot hold what was just typed.
        const bool writable = perms.size() > 1 && perms[1] == 'w';
        if (!writable) continue;
        const bool main_heap = path.starts_with("[heap]");

        regions.push_back(Region{*start, *end, writable, main_heap});
    }
    // The main heap first, then the newest (highest address) arena: that is
    // where a text buffer allocated while typing usually sits, so the first
    // hit comes early instead of after scanning every arena.
    std::ranges::stable_sort(regions, [](const Region& left, const Region& right) {
        if (left.main_heap != right.main_heap) return left.main_heap;
        return left.start > right.start;
    });
    if (resume_address != 0) {
        const auto found = std::ranges::find_if(regions, [=](const Region& region) {
            return region.start <= resume_address && resume_address < region.end;
        });
        if (found != regions.end()) {
            // Resume after the previous bounded scan, then wrap once through
            // the remaining mappings. Keep the skipped prefix for last so a
            // long-lived process is eventually scanned in its entirety.
            const Region prefix{found->start, resume_address, found->writable, found->main_heap};
            std::rotate(regions.begin(), found, regions.end());
            regions.front().start = resume_address;
            if (prefix.end > prefix.start) regions.push_back(prefix);
        }
    }
    return regions;
}

std::vector<std::byte> read_memory(pid_t pid, std::uintptr_t address, std::size_t size,
                                   int* error_out = nullptr) {
    if (error_out != nullptr) *error_out = 0;
    std::vector<std::byte> buffer(size);
    iovec local{buffer.data(), size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const ssize_t read = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (read <= 0) {
        if (error_out != nullptr) *error_out = errno;
        return {};
    }
    buffer.resize(static_cast<std::size_t>(read));
    return buffer;
}

void append_utf8(std::string& output, char32_t codepoint) {
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        codepoint = 0xFFFD;
    }
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string decode_utf8(const std::vector<std::byte>& bytes) {
    std::string output;
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        char32_t codepoint = 0;
        std::size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length = 4;
        } else {
            append_utf8(output, 0xFFFD);
            ++index;
            continue;
        }
        if (index + length > bytes.size()) {
            append_utf8(output, 0xFFFD);
            break;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(bytes[index + offset]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (!valid || (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000)) {
            append_utf8(output, 0xFFFD);
            ++index;
            continue;
        }
        append_utf8(output, codepoint);
        index += length;
    }
    return output;
}

std::string decode_utf16le(const std::vector<std::byte>& bytes) {
    std::string output;
    const std::size_t units = bytes.size() / 2;
    for (std::size_t index = 0; index < units; ++index) {
        const auto low = static_cast<unsigned char>(bytes[index * 2]);
        const auto high = static_cast<unsigned char>(bytes[index * 2 + 1]);
        char32_t unit = static_cast<char32_t>(low | (high << 8));
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units) {
            const auto low2 = static_cast<unsigned char>(bytes[(index + 1) * 2]);
            const auto high2 = static_cast<unsigned char>(bytes[(index + 1) * 2 + 1]);
            const char32_t next = static_cast<char32_t>(low2 | (high2 << 8));
            if (next >= 0xDC00 && next <= 0xDFFF) {
                append_utf8(output, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++index;
                continue;
            }
        }
        append_utf8(output, unit);
    }
    return output;
}

std::string decode_utf32le(const std::vector<std::byte>& bytes) {
    std::string output;
    const std::size_t units = bytes.size() / 4;
    for (std::size_t index = 0; index < units; ++index) {
        const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data() + index * 4);
        const char32_t codepoint = static_cast<char32_t>(raw[0] | (raw[1] << 8) | (raw[2] << 16) |
                                                         (static_cast<char32_t>(raw[3]) << 24));
        append_utf8(output, codepoint);
    }
    return output;
}

char32_t read_cell_codepoint(const std::byte* bytes) {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<char32_t>(raw[0] | (raw[1] << 8) | (raw[2] << 16) |
                                 (static_cast<char32_t>(raw[3]) << 24));
}

// Screen grids interleave a UTF-32 code point with cell attributes. A wide
// character occupies two cells; the continuation must not be emitted as a
// second character. Kitty marks its continuation in the attributes, Konsole
// fills it with an unreal placeholder, VTE repeats the code point in a
// fragment cell, so those cells are skipped for every grid.
std::string decode_utf32_cell(const std::vector<std::byte>& bytes, std::size_t cell) {
    std::string output;
    for (std::size_t offset = 0; offset + cell <= bytes.size(); offset += cell) {
        const char32_t codepoint = read_cell_codepoint(bytes.data() + offset);
        // Continuation cells: kitty marks them in the attributes, foot uses a
        // spacer code point above the Unicode range.
        if (codepoint > 0x10FFFF) continue;
        if (cell == 12 && (static_cast<unsigned char>(bytes[offset + 8]) & 0x01) != 0) continue;
        if (cell == 20 && (static_cast<unsigned char>(bytes[offset + 4]) & 0x10) != 0) continue;
        if (cell == 16 && bytes[offset + 14] == std::byte{0} &&
            bytes[offset + 15] == std::byte{0}) {
            continue;
        }
        // Empty cells decode to NUL, which trims the heap bytes before a row
        // in decode_window.
        append_utf8(output, codepoint);
    }
    return output;
}

std::string decode(const std::vector<std::byte>& bytes, Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return decode_utf8(bytes);
        case Encoding::Utf16Le: return decode_utf16le(bytes);
        case Encoding::Utf32Le: return decode_utf32le(bytes);
        case Encoding::Utf32Cell8Le: return decode_utf32_cell(bytes, 8);
        case Encoding::Utf32Cell12Le: return decode_utf32_cell(bytes, 12);
        case Encoding::Utf32Cell16Le: return decode_utf32_cell(bytes, 16);
        case Encoding::Utf32Cell20Le: return decode_utf32_cell(bytes, 20);
        case Encoding::Utf32Cell24Le: return decode_utf32_cell(bytes, 24);
    }
    return {};
}

// Cell grids are addressed in cells but the code point inside a cell is
// 4-byte aligned; the cell stride would reject every real match.
std::size_t unit_size(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return 1;
        case Encoding::Utf16Le: return 2;
        case Encoding::Utf32Le: return 4;
        case Encoding::Utf32Cell8Le: return 4;
        case Encoding::Utf32Cell12Le: return 4;
        case Encoding::Utf32Cell16Le: return 4;
        case Encoding::Utf32Cell20Le: return 4;
        case Encoding::Utf32Cell24Le: return 4;
    }
    return 1;
}

constexpr std::size_t kCellBytes = 12;

// A terminal grid stores each character in a 12-byte cell. A wide character
// duplicates its code point in the next cell, so the walk advances two cells
// whenever the following cell repeats the current code point. This verifies
// the characters after the first one, whose cell supplied the byte pattern,
// and returns how many cells the whole composition occupies.
std::optional<std::size_t> verify_cell12(pid_t pid, std::uintptr_t address,
                                         const std::vector<char32_t>& codepoints) {
    if (codepoints.empty()) return std::nullopt;
    // A match that starts at the continuation cell of a wide character would
    // report the composition one cell late and leak its first character into
    // the context. Only the first cell of a character may start a match.
    if (address >= kCellBytes) {
        const auto previous = read_memory(pid, address - kCellBytes, kCellBytes);
        const auto current = read_memory(pid, address, kCellBytes);
        if (previous.size() == kCellBytes && current.size() == kCellBytes) {
            const auto* previous_raw = reinterpret_cast<const unsigned char*>(previous.data());
            const char32_t previous_value = static_cast<char32_t>(
                previous_raw[0] | (previous_raw[1] << 8) | (previous_raw[2] << 16) |
                (static_cast<char32_t>(previous_raw[3]) << 24));
            const bool previous_first_cell =
                (static_cast<unsigned char>(previous[8]) & 0x01) == 0;
            const bool current_continuation = (static_cast<unsigned char>(current[8]) & 0x01) != 0;
            if (previous_first_cell && current_continuation &&
                previous_value == codepoints.front()) {
                return std::nullopt;
            }
        }
    }
    const std::size_t needed = (codepoints.size() * 2 + 1) * kCellBytes;
    const auto bytes = read_memory(pid, address, needed);
    std::size_t offset = 0;
    for (const char32_t codepoint : codepoints) {
        if (offset + 4 > bytes.size()) return std::nullopt;
        const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
        const char32_t value = static_cast<char32_t>(raw[0] | (raw[1] << 8) | (raw[2] << 16) |
                                                     (static_cast<char32_t>(raw[3]) << 24));
        if (value != codepoint) return std::nullopt;
        bool wide = false;
        if (offset + kCellBytes + 4 <= bytes.size()) {
            const char32_t following =
                read_cell_codepoint(bytes.data() + offset + kCellBytes);
            // kitty repeats the code point and flags the second cell; foot
            // stores a spacer above the Unicode range.
            wide = following > 0x10FFFF ||
                   (following == value && value > 0x2000 &&
                    offset + kCellBytes + 8 < bytes.size() &&
                    (static_cast<unsigned char>(bytes[offset + kCellBytes + 8]) & 0x01) != 0);
        }
        offset += wide ? 2 * kCellBytes : kCellBytes;
    }
    return offset / kCellBytes;
}

// Skip wide-character continuation cells while walking the anchor: Konsole
// leaves an unreal placeholder behind a wide glyph, VTE repeats the code
// point in a fragment cell, and a repeated code point in front of a wide
// character is the same thing. Every code point still has to match; accepting
// a one-character prefix makes any unrelated screen row beginning with that
// character a convincing false hit.
std::size_t skip_cell_continuations(pid_t pid, std::uintptr_t address, std::size_t offset,
                                    std::size_t cell, char32_t previous, char32_t next) {
    for (;;) {
        const auto current = read_memory(pid, address + offset, cell);
        if (current.size() < cell) return offset;
        const char32_t value = read_cell_codepoint(current.data());
        // Each grid marks the continuation in its own way; a repeated code
        // point is only a continuation when the grid has no explicit marker
        // (otherwise "你好好" or "ll" would be skipped and the walk misaligned).
        bool continuation = false;
        switch (cell) {
            case 12:
                // kitty flags the second cell in the attributes; foot stores a
                // spacer above the Unicode range.
                continuation = value > 0x10FFFF ||
                               (static_cast<unsigned char>(current[8]) & 0x01) != 0;
                break;
            case 16:
                // Konsole marks the right half of a wide glyph as an unreal
                // cell: code point zero with both extra-flag bytes clear.
                // Older rows use a blank cell after a wide glyph instead; a
                // real space in the anchor must still be consumed.
                continuation = (value == 0 && current[14] == std::byte{0} &&
                                current[15] == std::byte{0}) ||
                               (previous > 0x2000 && value == U' ' && next != U' ');
                break;
            case 20:
                // VTE repeats the code point in a fragment cell and flags it
                // in the low attribute byte.
                continuation = (static_cast<unsigned char>(current[4]) & 0x10) != 0;
                break;
            case 24:
                // Alacritty fills the second cell with a space and sets the
                // wide-char-spacer flag.
                continuation = (value == U' ' &&
                                (static_cast<unsigned char>(current[4]) & 0x40) != 0) ||
                               (previous > 0x2000 && value == U' ' && next != U' ');
                break;
            default:
                // Unknown grid layout: a spacer or a repeated wide glyph.
                continuation = value > 0x10FFFF || (value == previous && previous > 0x2000);
                break;
        }
        if (continuation) {
            offset += cell;
            continue;
        }
        return offset;
    }
}

std::optional<std::size_t> verify_cell_prefix(pid_t pid, std::uintptr_t address,
                                              const std::vector<char32_t>& codepoints,
                                              std::size_t cell) {
    if (codepoints.empty()) return std::nullopt;
    const auto first = read_memory(pid, address, cell);
    if (first.size() < cell || read_cell_codepoint(first.data()) != codepoints.front()) {
        return std::nullopt;
    }
    // VTE repeats a wide character in a flagged fragment cell. A one-char
    // anchor matching that second cell would put the real character in the
    // reported "before" text and leak the live preedit into context.
    if (cell == 20 && (static_cast<unsigned char>(first[4]) & 0x10) != 0) {
        return std::nullopt;
    }
    std::size_t offset = skip_cell_continuations(pid, address, cell, cell, codepoints.front(),
                                                 codepoints.size() > 1 ? codepoints[1] : 0);
    for (std::size_t index = 1; index < codepoints.size(); ++index) {
        const auto current = read_memory(pid, address + offset, cell);
        if (current.size() < cell || read_cell_codepoint(current.data()) != codepoints[index]) {
            return std::nullopt;
        }
        offset += cell;
        offset = skip_cell_continuations(pid, address, offset, cell, codepoints[index],
                                         index + 1 < codepoints.size() ? codepoints[index + 1] : 0);
    }
    return offset;
}

// Decodes a window and trims the heap metadata a partially readable area can
// contribute: everything up to the last NUL before the match, nothing after
// the first NUL behind it, and replacement characters a cut lead byte leaves.
std::string decode_window(std::vector<std::byte> raw, Encoding encoding) {
    if (const std::size_t cell = cell_bytes(encoding); cell >= 12) {
        // A heap object can end immediately before a screen row. In kitty a
        // few binary cells were followed by one plausible CJK code point
        // with unrelated attributes, then a uniform ASCII row; decoding
        // only code points let that orphan byte sequence become the first
        // character of the context. Require two invalid cells and several
        // agreeing row attributes before treating this as a boundary.
        std::size_t invalid = 0;
        std::size_t cut = 0;
        const std::size_t count = raw.size() / cell;
        for (std::size_t index = 0; index < count; ++index) {
            const char32_t codepoint = read_cell_codepoint(raw.data() + index * cell);
            if (codepoint > 0x10FFFF) {
                ++invalid;
                continue;
            }
            if (invalid >= 2 && index + 3 < count && codepoint > 0x7F &&
                std::memcmp(raw.data() + index * cell + 4,
                            raw.data() + (index + 1) * cell + 4, cell - 4) != 0) {
                const auto* attributes = raw.data() + (index + 1) * cell + 4;
                bool uniform = true;
                for (std::size_t next = index + 2; next <= index + 3; ++next) {
                    if (std::memcmp(attributes, raw.data() + next * cell + 4, cell - 4) != 0) {
                        uniform = false;
                        break;
                    }
                }
                if (uniform) cut = (index + 1) * cell;
            }
            invalid = 0;
        }
        if (cut != 0) raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(cut));
    }
    std::string decoded = decode(raw, encoding);
    // Terminal grids store a wide character as two cells: the code point and a
    // continuation marker (U+FFFF). Noncharacters are not document text, so
    // drop them instead of letting the trim stop there.
    if (encoding == Encoding::Utf32Le || cell_bytes(encoding) != 0) {
        std::vector<char32_t> codepoints;
        if (decode_text(decoded, codepoints)) {
            decoded.clear();
            for (const char32_t codepoint : codepoints) {
                if ((codepoint & 0xFFFEu) == 0xFFFEu) continue;
                if (codepoint >= 0xFDD0 && codepoint <= 0xFDEF) continue;
                if (codepoint <= 0x7F) decoded.push_back(static_cast<char>(codepoint));
                else if (codepoint <= 0x7FF) {
                    decoded.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else if (codepoint <= 0xFFFF) {
                    decoded.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else {
                    decoded.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    decoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    decoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
            }
        }
    }
    if (const auto nul = decoded.rfind('\0'); nul != std::string::npos) decoded.erase(0, nul + 1);
    // Heap neighbours are arbitrary bytes and decoding them can produce
    // plausible-looking code points (misaligned UTF-16 turns binary into
    // Hangul). Only the trailing run of plausible document text is what sits
    // in front of the caret; everything before it is dropped.
    std::vector<char32_t> codepoints;
    if (decode_text(decoded, codepoints)) {
        std::size_t keep = codepoints.size();
        while (keep > 0 && plausible_text_codepoint(codepoints[keep - 1])) --keep;
        std::size_t bytes = 0;
        for (std::size_t index = 0; index < keep; ++index) {
            const char32_t codepoint = codepoints[index];
            if (codepoint <= 0x7F) bytes += 1;
            else if (codepoint <= 0x7FF) bytes += 2;
            else if (codepoint <= 0xFFFF) bytes += 3;
            else bytes += 4;
        }
        decoded.erase(0, bytes);
    }
    if (!decoded.empty()) {
        const std::string replacement = "\xEF\xBF\xBD";
        while (decoded.starts_with(replacement)) decoded.erase(0, replacement.size());
    }
    return decoded;
}

std::string decode_after_window(std::vector<std::byte> raw, Encoding encoding) {
    std::string decoded = decode(raw, encoding);
    if (const auto nul = decoded.find('\0'); nul != std::string::npos) decoded.resize(nul);
    return decoded;
}

// The composition sits at the caret, so what follows it is a boundary (a
// blank cell, space, punctuation) or the text after the caret. A match whose
// next code point continues the same word is a prefix of a longer string
// elsewhere in the process (a path, a message, a URL), never the caret.
bool continues_word(const std::string& text) {
    std::vector<char32_t> codepoints;
    if (!decode_text(text, codepoints) || codepoints.empty()) return false;
    const char32_t next = codepoints.front();
    if ((next >= U'0' && next <= U'9') || (next >= U'A' && next <= U'Z') ||
        (next >= U'a' && next <= U'z') || next == U'_') {
        return true;
    }
    if (next >= 0x3040 && next <= 0x30FF) return true;  // kana
    if (next >= 0x3100 && next <= 0x312F) return true;  // bopomofo
    if (next >= 0x3400 && next <= 0x9FFF) return true;  // CJK
    if (next >= 0xF900 && next <= 0xFAFF) return true;  // CJK compatibility
    return false;
}

}  // namespace


bool same_uid(pid_t pid) {
    if (pid <= 1) return false;
    struct stat info {};
    if (::stat(("/proc/" + std::to_string(pid)).c_str(), &info) != 0) return false;
    return info.st_uid == ::getuid();
}

bool reset_soft_dirty(pid_t pid) {
    if (!same_uid(pid)) return false;
    std::ofstream clear("/proc/" + std::to_string(pid) + "/clear_refs");
    if (!clear) return false;
    clear << "4\n";
    return clear.good();
}

std::optional<Match> scan_pid(pid_t pid, const Anchor& anchor, std::size_t before_bytes,
                              std::size_t after_bytes, const ScanLimits& limits,
                              const std::vector<Hint>& hints, ScanError& error,
                              std::vector<Match>* candidates, std::uintptr_t* next_address,
                              ScanStats* stats, bool changed_only, bool reset_dirty_after) {
    if (!same_uid(pid)) {
        error = {"foreign-pid", "process " + std::to_string(pid) + " is not owned by this user"};
        return std::nullopt;
    }
    const std::size_t window_limit = std::max<std::size_t>(limits.window_limit, 1024);
    before_bytes = std::min(before_bytes, window_limit);
    after_bytes = std::min(after_bytes, window_limit);
    const Deadline deadline{std::chrono::steady_clock::now() + limits.timeout};
    std::size_t scanned = 0;
    bool truncated = false;
    bool denied = false;

    // Regions to try, in order: explicit hints first, then the pages the
    // client wrote since the last reset. A hint whose pages were not written
    // cannot hold the new composition, so it is dropped immediately instead of
    // being scanned or trusted; a hint that misses falls back to the dirty
    // set, never to a full scan. Checking the hint's own pages costs a few
    // pagemap entries, so the validated path never parses the maps.
    std::vector<Region> hint_regions;
    for (const auto& hint : hints) {
        if (hint.pid == pid && hint.end > hint.start) {
            hint_regions.push_back(Region{hint.start, hint.end, true});
        }
    }
    // Hints are read directly, even when their pages were not written since
    // the last reset: the text in front of the caret does not change while the
    // composition does, and a committed-text anchor is never rewritten at all.
    // A stale hint is harmless — the anchor still has to match there and the
    // dirty set is scanned afterwards.
    const std::size_t hint_count = hint_regions.size();
    std::vector<Region> regions = hint_regions;
    // The dirty fallback is read lazily: a matching hint ends the scan first.
    bool fallback_added = hint_count == 0;
    if (hint_count == 0) {
        regions = read_regions(pid, next_address ? *next_address : 0);
        if (changed_only) {
            // Only pages the client wrote since the last reset can hold a new
            // composition, so the rest of the address space is skipped.
            regions = dirty_pages(pid, regions);
            // Reset after the dirty set was read, before scanning it. A write
            // that lands while the scan runs is then recorded for the next
            // request instead of being cleared by a reset afterwards. A
            // multi-anchor request defers the reset until every anchor has
            // read the same dirty set.
            if (reset_dirty_after) reset_soft_dirty(pid);
        }
    }

    std::optional<Match> best;
    int best_score = -1;
    // Candidate slots filled by hits whose following text is a boundary. A
    // word-continuation hit may be a static string prefix, so it must not end
    // the scan before the real document buffer was seen.
    // Keep enough of the previous chunk that a hit's context window is
    // usually already in the buffer, so most hits need no extra syscalls.
    const std::size_t overlap =
        anchor.max_pattern_bytes > 0 ? anchor.max_pattern_bytes - 1 : 0;
    const std::size_t tail_bytes = std::max(overlap, before_bytes + anchor.max_pattern_bytes);

    for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
        const auto& region = regions[region_index];
        const auto resume_at = [&](std::uintptr_t address) {
            if (next_address == nullptr) return;
            if (address < region.end) *next_address = address;
            else if (region_index + 1 < regions.size()) *next_address = regions[region_index + 1].start;
            else *next_address = 0;
        };
        std::vector<std::byte> tail;
        std::uintptr_t position = region.start;
        while (position < region.end) {
            if (deadline.expired()) {
                resume_at(position);
                error = {"timeout", "scan budget exhausted"};
                return std::nullopt;
            }
            if (scanned >= limits.max_bytes_per_pid) {
                resume_at(position);
                truncated = true;
                break;
            }
            // A hint is a neighbourhood, not a mapped range. Its start can
            // fall in an unmapped page just before the client's buffer; one
            // large failed read would skip the actual anchor altogether.
            const std::size_t chunk_limit = region_index < hint_count
                                                ? kPageBytes - position % kPageBytes : kChunkBytes;
            const std::size_t request = std::min({chunk_limit, static_cast<std::size_t>(region.end - position),
                                                  limits.max_bytes_per_pid - scanned});
            int read_error = 0;
            auto chunk = read_memory(pid, position, request, &read_error);
            if (chunk.empty()) {
                if (read_error == EPERM || read_error == EACCES) denied = true;
                tail.clear();
                position += request;
                continue;
            }
            scanned += chunk.size();

            std::vector<std::byte> area;
            area.reserve(tail.size() + chunk.size());
            area.insert(area.end(), tail.begin(), tail.end());
            area.insert(area.end(), chunk.begin(), chunk.end());
            const auto area_start = position - tail.size();
            const auto area_end = area_start + area.size();
            const auto slice_or_read = [&](std::uintptr_t from, std::size_t size) {
                if (size == 0) return std::vector<std::byte>{};
                if (from >= area_start && from <= area_end && size <= area_end - from) {
                    const auto offset = static_cast<std::size_t>(from - area_start);
                    return std::vector<std::byte>(area.begin() + offset,
                                                  area.begin() + offset + size);
                }
                return read_memory(pid, from, size);
            };

            for (std::size_t index = 0; index < anchor.patterns.size(); ++index) {
                const auto& pattern = anchor.patterns[index];
                const auto encoding = anchor.encodings[index];
                const std::size_t unit = unit_size(encoding);
                std::size_t search_from = 0;
                for (;;) {
                // Most text patterns have a nonzero leading byte. SIMD-optimized
                // memchr finds that byte cheaply; compare the full pattern only
                // at those locations. A zero leading byte is common in UTF-16/
                // UTF-32 heap data, so use memmem there to avoid excessive
                // comparisons of zero-filled pages.
                const void* found = nullptr;
                if (search_from < area.size()) {
                    found = pattern[0] == std::byte{0}
                                ? memmem(area.data() + search_from, area.size() - search_from,
                                         pattern.data(), pattern.size())
                                : std::memchr(area.data() + search_from,
                                              static_cast<unsigned char>(pattern[0]),
                                              area.size() - search_from);
                }
                if (found == nullptr) break;
                const auto offset = static_cast<std::size_t>(
                    static_cast<const std::byte*>(found) - area.data());
                search_from = offset + 1;
                if (area.size() - offset < pattern.size() ||
                    std::memcmp(area.data() + offset, pattern.data(), pattern.size()) != 0) {
                    continue;
                }
                const auto address = area_start + offset;
                // Genuine char16_t/char32_t buffers are aligned to their
                // code-unit width. Misaligned byte coincidences (observed
                // in unrelated GUI processes) must not consume candidate
                // slots or be treated as document text.
                if (address % unit != 0) continue;
                // The cell pattern covers only the first character, so the
                // verified walk determines where the composition ends.
                std::size_t match_bytes = pattern.size();
                if (const std::size_t cell = cell_bytes(encoding); cell != 0) {
                    if (cell == kCellBytes) {
                        // The cell pattern contains only the first code point.
                        // Common first letters occur hundreds of times in a
                        // terminal screen. Reject them from the already-read
                        // chunk before doing process_vm_readv for each one.
                        // At a chunk boundary, leave verification to the
                        // existing cross-process walk instead.
                        if (anchor.codepoints.size() > 1 &&
                            offset <= area.size() &&
                            area.size() - offset >= 2 * cell + 4) {
                            const char32_t first = anchor.codepoints.front();
                            const char32_t following = read_cell_codepoint(area.data() + offset + cell);
                            const std::size_t next = cell * (following == first || following > 0x10FFFF ? 2 : 1);
                            if (area.size() - offset >= next + 4 &&
                                read_cell_codepoint(area.data() + offset + next) != anchor.codepoints[1]) {
                                continue;
                            }
                        }
                        const auto cells = verify_cell12(pid, address, anchor.codepoints);
                        if (!cells) continue;
                        match_bytes = *cells * kCellBytes;
                    } else {
                        const auto span = verify_cell_prefix(pid, address, anchor.codepoints, cell);
                        if (!span) continue;
                        match_bytes = *span;
                    }
                }
                if (stats != nullptr) ++stats->hits;

                // Cell grids are read as whole 12-byte cells so the decoder
                // starts at a cell boundary; other encodings use their unit.
                const std::size_t window_unit =
                    cell_bytes(encoding) != 0 ? cell_bytes(encoding) : unit;
                const std::size_t before_window = before_bytes - before_bytes % window_unit;
                std::size_t after_window = after_bytes - after_bytes % window_unit;
                // The word-continuation check needs at least one full unit of
                // following text; a 16-byte request cannot cover a 24-byte
                // cell and would make that grid blind to static prefixes.
                if (after_bytes > 0) after_window = std::max(after_window, window_unit);
                // The hit may be on a dirty page while the preceding row is
                // on an unchanged page. Restricting the *window* to the dirty
                // region silently drops its first characters. Searching is
                // still confined to the region; read adjacent bytes only
                // after a matching anchor has been located.
                const std::uintptr_t left_edge = address >= before_window ? address - before_window : 0;
                const std::uintptr_t right_edge = address + match_bytes + after_window;
                auto before_raw = address > left_edge
                                      ? slice_or_read(left_edge, address - left_edge)
                                      : std::vector<std::byte>{};
                auto after_raw = right_edge > address + match_bytes
                                      ? slice_or_read(address + match_bytes,
                                                      right_edge - address - match_bytes)
                                      : std::vector<std::byte>{};
                if (before_raw.empty() && address > left_edge) {
                    // The requested window can begin in an unmapped page.
                    // Recover the readable part of the hit's own page.
                    const auto page_start = address - address % kPageBytes;
                    if (page_start < address && page_start > left_edge) {
                        before_raw = slice_or_read(page_start, address - page_start);
                    }
                    if (before_raw.empty() && address > region.start && left_edge < region.start) {
                        before_raw = slice_or_read(region.start, address - region.start);
                    }
                }
                if (after_raw.empty() && region.end > address + match_bytes && right_edge > region.end) {
                    after_raw = slice_or_read(address + match_bytes, region.end - address - match_bytes);
                }
                // Keep the windows aligned to the encoding unit so the decoder
                // starts at a code unit boundary.
                if (before_raw.size() % window_unit != 0) {
                    before_raw.erase(before_raw.begin(),
                                     before_raw.begin() + static_cast<std::ptrdiff_t>(before_raw.size() % window_unit));
                }

                Match match;
                match.pid = pid;
                match.encoding = encoding;
                match.address = address;
                match.before_bytes = before_raw.size();
                match.after_bytes = after_raw.size();
                match.scanned_bytes = scanned;
                match.truncated = truncated;
                // Keep the raw window for the attribute check; the decode copy
                // is cheap compared to the scan itself.
                match.before = decode_window(before_raw, encoding);
                match.after = decode_after_window(std::move(after_raw), encoding);
                const TextRun run = trailing_text_run(match.before);
                const int score = run.length;
                const bool misread_attributes =
                    cell_bytes(encoding) != 0 &&
                    cell_attributes_vary(before_raw, cell_bytes(encoding),
                                         static_cast<std::size_t>(run.length));
                const bool word_continuation = continues_word(match.after);
                // Screen grids show the composition itself, so they are the
                // strongest lead; plain string copies can be paths, messages
                // or protocol buffers.
                match.confidence = std::min(score, 32) +
                                   (cell_bytes(encoding) != 0 ? 32 : 0);
#ifdef LLAVON_IME_DEBUG
                // Dump usable text windows only. Logging every binary hit
                // creates many copies of the anchor in the scanned client
                // and can drown out the document buffer itself.
                if (score >= kGoodTextRun && !run.suspicious && !misread_attributes) {
                    char head[160];
                    std::snprintf(head, sizeof(head),
                                  "hit pid=%d enc=%s addr=0x%lx before=%zu after=%zu raw=",
                                  static_cast<int>(pid), encoding_name(encoding),
                                  static_cast<unsigned long>(address), before_raw.size(),
                                  match.after_bytes);
                    static constexpr char kHex[] = "0123456789abcdef";
                    std::string line = head;
                    line.reserve(line.size() + before_raw.size() * 2 + 1);
                    for (const std::byte byte : before_raw) {
                        const auto value = static_cast<unsigned>(byte);
                        line.push_back(kHex[value >> 4]);
                        line.push_back(kHex[value & 0xF]);
                    }
                    line.push_back('\n');
                    std::fwrite(line.data(), 1, line.size(), stderr);
                }
#endif
                if (stats != nullptr && score >= kGoodTextRun && !word_continuation &&
                    !run.suspicious && !misread_attributes) {
                    ++stats->qualified;
                }
                if (candidates != nullptr) {
                    // The provider must verify a location across two distinct
                    // natural preedit states; never turn a single nice-looking
                    // copy into a trusted result. Bound memory and scan time.
                    // A misread byte stream (repeated or sequential code
                    // points, or a grid whose attributes vary per cell) is not
                    // a caret location at all.
                    if (score < kGoodTextRun || run.suspicious || misread_attributes) continue;
                    // A hit that continues as a longer word is what both a
                    // static string prefix and a caret in the middle of
                    // existing text look like. Keep it as a continuation
                    // candidate; the provider only trusts it when the text
                    // after it is stable across composition states, which a
                    // static string's shrinking tail cannot be.
                    match.continuation = word_continuation;
                    if (candidates->size() < kMaxCandidates) {
                        candidates->push_back(match);
                    } else if (cell_bytes(encoding) != 0 &&
                               !std::ranges::any_of(*candidates, [](const Match& item) {
                                   return cell_bytes(item.encoding) != 0;
                               })) {
                        // Byte strings copied by the client (including its
                        // debug output) can fill every slot before the screen
                        // grid is visited. Keep a slot for the first real
                        // grid location so it can be verified next state.
                        const auto plain = std::ranges::find_if(*candidates, [](const Match& item) {
                            return cell_bytes(item.encoding) == 0;
                        });
                        if (plain != candidates->end()) *plain = match;
                    } else if (region_index < hint_count &&
                               std::ranges::any_of(hints, [&](const Hint& hint) {
                                   return hint.pid == pid && hint.match_address == match.address;
                               })) {
                        // The hinted range can contain unrelated copies
                        // before the validated address; reserve a slot for it.
                        candidates->back() = match;
                    } else if (!word_continuation) {
                        // A strong hit displaces a continuation placeholder
                        // instead of ending the scan without the document
                        // buffer.
                        const auto weak = std::ranges::find_if(
                            *candidates, [](const Match& item) { return item.continuation; });
                        if (weak != candidates->end()) {
                            *weak = match;
                        } else {
                            // When copies have filled the slots, prefer the
                            // one closest to the validated address. In
                            // particular, an old copy inside a narrow hint
                            // must not crowd out the adjacent document row.
                            const auto distance = [&](std::uintptr_t address) {
                                std::uintptr_t nearest = std::numeric_limits<std::uintptr_t>::max();
                                for (const auto& hint : hints) {
                                    if (hint.pid != pid || hint.match_address == 0) continue;
                                    const auto delta = address > hint.match_address
                                                           ? address - hint.match_address
                                                           : hint.match_address - address;
                                    nearest = std::min(nearest, delta);
                                }
                                return nearest;
                            };
                            const auto duplicate = std::ranges::find_if(*candidates, [&](const Match& item) {
                                return item.before == match.before &&
                                       std::ranges::count_if(*candidates, [&](const Match& other) {
                                           return other.before == item.before;
                                       }) > 1 && distance(match.address) < distance(item.address);
                            });
                            if (duplicate != candidates->end()) *duplicate = match;
                        }
                    }
                    // A process can keep many copies of the same row (and
                    // debug logs can create still more). Keep looking until
                    // the slots represent distinct prefixes; otherwise the
                    // real document copy may sit just beyond the first 12.
                    if (candidates->size() == kMaxCandidates && region_index >= hint_count &&
                        (!limits.stop_at_strong_grid_candidate ||
                         std::ranges::any_of(*candidates, [](const Match& item) {
                             return cell_bytes(item.encoding) != 0;
                         })) &&
                        std::ranges::all_of(*candidates, [](const Match& item) {
                            return !item.continuation;
                        }) &&
                        std::ranges::all_of(*candidates, [&](const Match& item) {
                            return std::ranges::count_if(*candidates, [&](const Match& other) {
                                return item.before == other.before;
                            }) == 1;
                        })) {
                        resume_at(position + request);
                        return std::nullopt;
                    }
                } else if (score >= kGoodTextRun && !word_continuation && !run.suspicious &&
                           !misread_attributes) {
                    return match;
                }
                if (!best || score > best_score) {
                    best_score = score;
                    best = std::move(match);
                }
                }
            }

            if (chunk.size() > tail_bytes) {
                tail.assign(chunk.end() - static_cast<std::ptrdiff_t>(tail_bytes), chunk.end());
            } else {
                tail = chunk;
            }
            position += request;
            // For a known terminal, the first complete, plausible screen row
            // is enough to start temporal verification. Finish the chunk to
            // retain competing copies nearby, rather than reading every map
            // before the next raw key can be examined.
            if (candidates != nullptr && limits.stop_at_strong_grid_candidate &&
                region_index >= hint_count &&
                std::ranges::any_of(*candidates, [](const Match& item) {
                    return !item.continuation && cell_bytes(item.encoding) != 0;
                })) {
                if (next_address != nullptr) *next_address = 0;
                return std::nullopt;
            }
        }
        if (truncated) break;
        // The hinted region was scanned first, but the scan continues: a
        // validated hint can be an old copy of the row (scrollback, an older
        // screen state) while the current row sits elsewhere, and the engine
        // has to be able to compare them.
        if (region_index + 1 == regions.size() && !fallback_added) {
            fallback_added = true;
            auto fallback = read_regions(pid, next_address ? *next_address : 0);
            if (changed_only) {
                fallback = dirty_pages(pid, fallback);
                if (reset_dirty_after) reset_soft_dirty(pid);
            }
            regions.insert(regions.end(), fallback.begin(), fallback.end());
        }
    }
    // A completed scan wraps to the beginning; the next state starts from the
    // main heap again, where a freshly typed composition usually sits. A
    // truncated scan keeps its resume cursor so the rest is scanned next.
    if (!truncated && next_address != nullptr) *next_address = 0;
    if (best) return best;
    if (denied) {
        error = {"denied", "process memory read denied; grant CAP_SYS_PTRACE to the helper "
                            "or set kernel.yama.ptrace_scope=0"};
    } else if (truncated) {
        error = {"budget", "composition not found within the byte budget"};
    } else {
        error = {"not-found", "composition not found"};
    }
    return std::nullopt;
}

void scan_pid_anchors(pid_t pid, const std::vector<AnchorRequest>& anchors,
                      std::size_t before_bytes, std::size_t after_bytes,
                      const ScanLimits& limits, const std::vector<Hint>& hints,
                      ScanError& error, std::vector<Match>* candidates,
                      std::uintptr_t* next_address, ScanStats* stats, bool changed_only) {
    if (anchors.empty()) {
        error = {"not-found", "no anchor"};
        return;
    }
    // The same dirty set is used for every anchor: a changed-only scan reads
    // it once and the caller resets the baseline after this function returns.
    bool denied = false;
    ScanError reported;
    for (std::size_t index = 0; index < anchors.size(); ++index) {
        const Anchor* use =
            anchors[index].narrowed != nullptr ? anchors[index].narrowed : anchors[index].full;
        if (use == nullptr) continue;
        const std::size_t before_count = candidates != nullptr ? candidates->size() : 0;
        ScanError local;
        // Only the first anchor keeps the resume cursor: the others read the
        // dirty set from the start, which is what a fresh state needs.
        (void)scan_pid(pid, *use, before_bytes, after_bytes, limits, hints, local, candidates,
                       index == 0 ? next_address : nullptr, stats, changed_only, false);
        if (anchors[index].narrowed != nullptr && anchors[index].full != nullptr &&
            (candidates == nullptr || candidates->size() == before_count)) {
            // The client's layout may differ from the table; try every encoding
            // before giving up on this anchor.
            (void)scan_pid(pid, *anchors[index].full, before_bytes, after_bytes, limits, hints,
                           local, candidates, nullptr, stats, changed_only, false);
        }
        if (local.code == "denied") denied = true;
        if (!local.code.empty() && local.code != "not-found" && reported.code.empty()) {
            reported = local;
        }
        if (candidates != nullptr) {
            for (std::size_t i = before_count; i < candidates->size(); ++i) {
                (*candidates)[i].anchor_index = index;
            }
        }
    }
    // Every anchor has read the same dirty set; consume it now.
    if (changed_only) reset_soft_dirty(pid);
    if (candidates != nullptr && !candidates->empty()) {
        error = {};
        return;
    }
    if (denied) {
        error = {"denied", "process memory read denied; grant CAP_SYS_PTRACE to the helper "
                            "or set kernel.yama.ptrace_scope=0"};
    } else if (!reported.code.empty()) {
        error = std::move(reported);
    } else {
        error = {"not-found", "composition not found"};
    }
}


}  // namespace llavon::memscan

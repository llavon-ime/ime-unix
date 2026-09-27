#include "anchor.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace llavon::memscan {

namespace {

constexpr std::size_t kMinCodepoints = 1;
constexpr std::size_t kMaxCodepoints = 64;

bool decode_utf8(std::string_view input, std::vector<char32_t>& output) {
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
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
            return false;
        }
        if (index + length > input.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        output.push_back(codepoint);
        index += length;
    }
    return true;
}

std::vector<std::byte> to_utf8_bytes(const std::vector<char32_t>& codepoints) {
    std::vector<std::byte> bytes;
    for (const char32_t codepoint : codepoints) {
        if (codepoint <= 0x7F) {
            bytes.push_back(static_cast<std::byte>(codepoint));
        } else if (codepoint <= 0x7FF) {
            bytes.push_back(static_cast<std::byte>(0xC0 | (codepoint >> 6)));
            bytes.push_back(static_cast<std::byte>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            bytes.push_back(static_cast<std::byte>(0xE0 | (codepoint >> 12)));
            bytes.push_back(static_cast<std::byte>(0x80 | ((codepoint >> 6) & 0x3F)));
            bytes.push_back(static_cast<std::byte>(0x80 | (codepoint & 0x3F)));
        } else {
            bytes.push_back(static_cast<std::byte>(0xF0 | (codepoint >> 18)));
            bytes.push_back(static_cast<std::byte>(0x80 | ((codepoint >> 12) & 0x3F)));
            bytes.push_back(static_cast<std::byte>(0x80 | ((codepoint >> 6) & 0x3F)));
            bytes.push_back(static_cast<std::byte>(0x80 | (codepoint & 0x3F)));
        }
    }
    return bytes;
}

std::vector<std::byte> to_utf16_bytes(const std::vector<char32_t>& codepoints) {
    std::vector<std::byte> bytes;
    for (const char32_t codepoint : codepoints) {
        auto push = [&bytes](std::uint16_t unit) {
            bytes.push_back(static_cast<std::byte>(unit & 0xFF));
            bytes.push_back(static_cast<std::byte>((unit >> 8) & 0xFF));
        };
        if (codepoint <= 0xFFFF) {
            push(static_cast<std::uint16_t>(codepoint));
        } else {
            const char32_t adjusted = codepoint - 0x10000;
            push(static_cast<std::uint16_t>(0xD800 | (adjusted >> 10)));
            push(static_cast<std::uint16_t>(0xDC00 | (adjusted & 0x3FF)));
        }
    }
    return bytes;
}

std::vector<std::byte> to_utf32_bytes(const std::vector<char32_t>& codepoints) {
    std::vector<std::byte> bytes;
    for (const char32_t codepoint : codepoints) {
        bytes.push_back(static_cast<std::byte>(codepoint & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 8) & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 16) & 0xFF));
        bytes.push_back(static_cast<std::byte>((codepoint >> 24) & 0xFF));
    }
    return bytes;
}

// Terminal grids interleave each code point with cell attributes, so the
// bytes around a cell are not part of the text. Only the first character's
// cell is used as the byte pattern; the rest is verified cell by cell.
std::vector<std::byte> to_utf32_cell_bytes(char32_t codepoint) {
    return {static_cast<std::byte>(codepoint & 0xFF),
            static_cast<std::byte>((codepoint >> 8) & 0xFF),
            static_cast<std::byte>((codepoint >> 16) & 0xFF),
            static_cast<std::byte>((codepoint >> 24) & 0xFF)};
}

}  // namespace

bool decode_text(const std::string& utf8, std::vector<char32_t>& output) {
    return decode_utf8(utf8, output);
}

bool plausible_text_codepoint(char32_t codepoint) {
    if (codepoint == '\n' || codepoint == '\t') return true;
    if (codepoint >= 0x20 && codepoint <= 0x7E) return true;
    if (codepoint >= 0xA0 && codepoint <= 0x36F) return true;    // Latin, IPA, marks
    if (codepoint >= 0x370 && codepoint <= 0x4FF) return true;    // Greek, Cyrillic
    if (codepoint >= 0x2000 && codepoint <= 0x206F) return true;  // punctuation
    if (codepoint >= 0x2E80 && codepoint <= 0x9FFF) return true;  // CJK
    if (codepoint >= 0x3000 && codepoint <= 0x303F) return true;  // CJK punctuation
    if (codepoint >= 0x3100 && codepoint <= 0x312F) return true;  // Bopomofo
    if (codepoint >= 0x31A0 && codepoint <= 0x31BF) return true;  // Bopomofo extended
    if (codepoint >= 0xFF00 && codepoint <= 0xFFEF) return true;  // Fullwidth forms
    return false;
}

const char* encoding_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return "utf8";
        case Encoding::Utf16Le: return "utf16le";
        case Encoding::Utf32Le: return "utf32le";
        case Encoding::Utf32Cell8Le: return "utf32cell8le";
        case Encoding::Utf32Cell12Le: return "utf32cell12le";
        case Encoding::Utf32Cell16Le: return "utf32cell16le";
        case Encoding::Utf32Cell20Le: return "utf32cell20le";
        case Encoding::Utf32Cell24Le: return "utf32cell24le";
    }
    return "unknown";
}

std::size_t cell_bytes(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf32Cell8Le: return 8;
        case Encoding::Utf32Cell12Le: return 12;
        case Encoding::Utf32Cell16Le: return 16;
        case Encoding::Utf32Cell20Le: return 20;
        case Encoding::Utf32Cell24Le: return 24;
        default: return 0;
    }
}

std::optional<std::string> parse_anchor_text(const std::string& utf8, AnchorError& error) {
    std::vector<char32_t> codepoints;
    if (!decode_utf8(utf8, codepoints)) {
        error = {"anchor-invalid", "anchor is not valid UTF-8"};
        return std::nullopt;
    }
    if (codepoints.size() < kMinCodepoints || codepoints.size() > kMaxCodepoints) {
        error = {"anchor-invalid", "anchor must have 1..64 code points"};
        return std::nullopt;
    }
    for (const char32_t codepoint : codepoints) {
        if (codepoint < 0x20 || codepoint == 0x7F) {
            error = {"anchor-invalid", "anchor must not contain control characters"};
            return std::nullopt;
        }
    }
    return utf8;
}

std::optional<Anchor> parse_anchor(const std::string& utf8, AnchorError& error) {
    const auto text = parse_anchor_text(utf8, error);
    if (!text) return std::nullopt;

    std::vector<char32_t> codepoints;
    decode_utf8(*text, codepoints);
    Anchor anchor;
    anchor.text = *text;
    anchor.codepoints = codepoints;
    const auto first_cell = to_utf32_cell_bytes(codepoints.front());
    anchor.encodings = {Encoding::Utf8, Encoding::Utf16Le, Encoding::Utf32Le,
                        Encoding::Utf32Cell8Le, Encoding::Utf32Cell12Le,
                        Encoding::Utf32Cell16Le, Encoding::Utf32Cell20Le,
                        Encoding::Utf32Cell24Le};
    anchor.patterns = {to_utf8_bytes(codepoints), to_utf16_bytes(codepoints),
                       to_utf32_bytes(codepoints), first_cell, first_cell, first_cell,
                       first_cell, first_cell};
    for (const auto& pattern : anchor.patterns) {
        anchor.max_pattern_bytes = std::max(anchor.max_pattern_bytes, pattern.size());
    }
    return anchor;
}

}  // namespace llavon::memscan

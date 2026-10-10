#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llavon::memscan {

// Byte encodings the text can be stored in. UI toolkits use UTF-8 (GTK,
// terminals) or UTF-16 (Qt, Java, Chromium); terminals that store cells as
// code points need UTF-32. The Cell variants are screen grids whose first
// four bytes per cell are a UTF-32 code point: 8-byte cells, 12-byte cells
// (kitty, foot), 16-byte cells (Konsole), 20-byte cells (VTE), and 24-byte
// cells (Alacritty).
enum class Encoding : std::uint8_t {
    Utf8,
    Utf16Le,
    Utf32Le,
    Utf32Cell8Le,
    Utf32Cell12Le,
    Utf32Cell16Le,
    Utf32Cell20Le,
    Utf32Cell24Le,
    // Same stride as Kitty, but attrs.clean is not a continuation marker.
    // This is a decoding schema, never an input-field identity credential.
    FootCell12Le,
};

// Bytes per screen cell for the cell encodings; 0 for the byte encodings.
std::size_t cell_bytes(Encoding encoding);

const char* encoding_name(Encoding encoding);

// Text to search for (the composition the input method is showing).
struct Anchor {
    std::string text;                              // canonical UTF-8
    std::vector<char32_t> codepoints;              // decoded text
    std::vector<std::vector<std::byte>> patterns;  // parallel to encodings
    std::vector<Encoding> encodings;
    std::size_t max_pattern_bytes = 0;
};

struct AnchorError {
    std::string code;
    std::string detail;
};

// Validates one piece of text: valid UTF-8 with 1..64 code points and no
// control characters. The helper searches for text only, never arbitrary byte
// patterns. Returns the canonical text, or nullopt with `error` set.
std::optional<std::string> parse_anchor_text(const std::string& utf8, AnchorError& error);

// Decodes UTF-8 text into code points. Returns false when the input is not
// valid UTF-8.
bool decode_text(const std::string& utf8, std::vector<char32_t>& output);

// True for code points that plausibly appear in a document the IME serves.
// Ranges binary data decodes into are excluded: a window made of those is heap
// metadata, not text.
bool plausible_text_codepoint(char32_t codepoint);

std::optional<Anchor> parse_anchor(const std::string& utf8, AnchorError& error);

}  // namespace llavon::memscan

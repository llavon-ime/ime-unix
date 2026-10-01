#pragma once

#include <charconv>
#include <cctype>
#include <concepts>
#include <optional>
#include <string_view>

namespace llavon::ime {

// Keeps the decimal grammar of stoi/strtol (leading whitespace and '+').
// Prefix mode is for legacy callers that intentionally accepted a suffix.
template <std::integral T>
[[nodiscard]] std::optional<T> parse_decimal(std::string_view text, bool complete = true) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    if (text.starts_with('+')) {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') return std::nullopt;
    }
    if (text.empty()) return std::nullopt;
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || (complete && parsed.ptr != text.data() + text.size()))
        return std::nullopt;
    return value;
}

}  // namespace llavon::ime

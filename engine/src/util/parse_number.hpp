#pragma once

#include <charconv>
#include <cctype>
#include <concepts>
#include <memory>
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
    const auto* end = std::to_address(text.end());
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || (complete && parsed.ptr != end))
        return std::nullopt;
    return value;
}

}  // namespace llavon::ime

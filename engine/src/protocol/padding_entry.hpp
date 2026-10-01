#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace llavon::ime::protocol {

// A position is either an unresolved reading or an explicitly chosen scalar.
// The variant is internal; the wire representation remains tag 0/1 + payload.
class PaddingEntry final {
public:
    PaddingEntry() = default;
    explicit PaddingEntry(std::u16string reading) : value_(std::move(reading)) {}
    explicit PaddingEntry(char32_t character) : value_(character) {}
    // Compatibility for existing request fixtures; inactive fields are ignored
    // just as they are by the wire encoder.
    PaddingEntry(bool chosen, std::u16string reading, char32_t character)
        : value_(chosen ? Value(character) : Value(std::move(reading))) {}

    [[nodiscard]] bool chosen() const noexcept { return std::holds_alternative<char32_t>(value_); }
    [[nodiscard]] char32_t chosen_char() const noexcept {
        const auto* character = std::get_if<char32_t>(&value_);
        return character ? *character : 0;
    }
    [[nodiscard]] std::u16string_view bopomofo() const noexcept {
        const auto* reading = std::get_if<std::u16string>(&value_);
        return reading ? std::u16string_view(*reading) : std::u16string_view{};
    }

private:
    using Value = std::variant<std::u16string, char32_t>;
    Value value_;
};

}  // namespace llavon::ime::protocol

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "debug/debug_log.hpp"
#include "text/utf.hpp"

namespace llavon::ime {

// Debug logging for the prediction context: the text handed to and taken from
// the model, truncated to keep the log readable.
#ifdef LLAVON_IME_DEBUG
inline std::string context_preview(std::u16string_view text, std::size_t max_units = 40) {
    std::u16string preview(text.substr(0, std::min(text.size(), max_units)));
    if (!preview.empty() && preview.back() >= 0xD800 && preview.back() <= 0xDBFF) preview.pop_back();
    try {
        return u16_to_utf8(preview);
    } catch (const std::exception&) {
        return "<invalid>";
    }
}

inline void log_context(const char* source, std::u16string_view text) {
    const std::string preview = context_preview(text);
    LLAVON_DEBUG_LOG("CTX", "source=%s units=%zu text=\"%s\"", source, text.size(),
                     preview.c_str());
}
#else
inline void log_context(const char*, std::u16string_view) {}
#endif

}  // namespace llavon::ime

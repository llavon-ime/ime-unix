#include "test_suites.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "bopomofo/keymap.hpp"
#include "buffer/composition_buffer.hpp"
#include "engine/fallback_engine.hpp"

int run_fallback_engine_tests() {
    using namespace llavon::ime;

    FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
    // Token IDs put "of" above "add" numerically, but real language data
    // says "of" is substantially more common. This guards the original bug.
    if (fallback.latin_frequency(u"of") <= fallback.latin_frequency(u"add")) return EXIT_FAILURE;
    if (!fallback.is_known_english(u"ADDED") || !fallback.is_known_english(u"nefarious")) return EXIT_FAILURE;
    CompositionBuffer buffer;
    for (const char32_t key : std::u32string(U"su3")) {
        if (!buffer.add_bopomofo_key(key, BopomofoKeyboardLayout::Standard)) return EXIT_FAILURE;
    }

    const auto predictions = fallback.predict(buffer);
    if (predictions.size() != 1 || predictions[0].candidates.size() < 2) return EXIT_FAILURE;

    const auto& segment = buffer.segments()[0];
    const auto merged_empty = fallback.merge_model_candidates(segment, {});
    if (merged_empty != predictions[0].candidates) return EXIT_FAILURE;

    const char32_t model_first = predictions[0].candidates.back();
    const auto merged_partial = fallback.merge_model_candidates(segment, {model_first});
    if (merged_partial.empty() || merged_partial.front() != model_first) return EXIT_FAILURE;
    for (const char32_t candidate : predictions[0].candidates) {
        if (std::find(merged_partial.begin(), merged_partial.end(), candidate) == merged_partial.end()) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

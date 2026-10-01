#include "test_suites.h"
#include "context/caret_prefix_sampler.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace llavon::ime {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

bool test_simple_prefix() {
    CaretPrefixSampler sampler(80);
    std::u16string text = u"今天天氣很好，現在繼續寫";
    sampler.set_text(std::move(text), 6);
    bool ok = check(sampler.text_before_caret() == u"今天天氣很好", "caret splits the text correctly");
    ok &= check(sampler.window_start() == 0, "full sample starts at zero");
    ok &= check(sampler.sampled() && sampler.usable(), "sample is usable");
    return ok;
}

bool test_multiline_prefix_is_kept() {
    // AT-SPI gives the whole widget text, so earlier lines are real context.
    CaretPrefixSampler sampler(80);
    std::u16string text = u"第一行\n第二行內容";
    sampler.set_text(std::move(text), 10);
    bool ok = check(sampler.usable(), "multi-line sample is usable");
    ok &= check(sampler.text_before_caret() == u"第一行\n第二行內容", "all text before the caret is kept");
    return ok;
}

bool test_window_trim_drops_oldest() {
    CaretPrefixSampler sampler(5);
    std::u16string text = u"abcdefghij";
    sampler.set_text(std::move(text), 10);
    bool ok = check(sampler.usable(), "long sample is usable");
    ok &= check(sampler.window_start() == 5, "window_start accounts for the dropped prefix");
    ok &= check(sampler.text_before_caret() == u"fghij", "text is truncated to the newest code units");
    return ok;
}

bool test_no_text_not_sampled() {
    CaretPrefixSampler sampler(80);
    sampler.set_text(std::u16string(), 0);
    return check(!sampler.sampled() && !sampler.usable(), "no text means not sampled");
}

bool test_empty_pre_caret() {
    CaretPrefixSampler sampler(80);
    sampler.set_text(std::u16string(u"rest"), 0);
    return check(sampler.usable() && sampler.text_before_caret().empty(),
                 "caret at text start is usable with an empty prefix");
}

bool test_caret_past_end_clamped() {
    CaretPrefixSampler sampler(80);
    std::u16string text = u"abc";
    sampler.set_text(std::move(text), 99);
    bool ok = check(sampler.usable(), "overlong caret clamps");
    ok &= check(sampler.text_before_caret() == u"abc", "clamped caret takes the whole text");
    return ok;
}

bool test_window_trim_keeps_pairs_whole() {
    CaretPrefixSampler sampler(3);
    std::u16string text = u"\U0001F600ab";
    sampler.set_text(std::move(text), 4);
    bool ok = check(sampler.usable(), "surrogate sample is usable");
    ok &= check(sampler.text_before_caret() == u"ab", "a pair straddling the window is dropped whole");
    ok &= check(sampler.window_start() == 2, "window_start skips past the dropped pair");
    return ok;
}

bool test_caret_inside_pair_drops_incomplete_scalar() {
    CaretPrefixSampler sampler(80);
    std::u16string text = u"a\U0001F600b";
    sampler.set_text(std::move(text), 2);
    bool ok = check(sampler.text_before_caret() == u"a", "a caret inside a pair keeps only complete scalars");
    return ok;
}

bool test_reset_to_empty_clears_state() {
    CaretPrefixSampler sampler(80);
    sampler.set_text(std::u16string(u"abc"), 3);
    sampler.set_text(std::u16string(), 0);
    bool ok = check(!sampler.sampled() && !sampler.usable(), "an empty widget clears sampled/usable");
    ok &= check(sampler.text_before_caret().empty(), "an empty widget clears the sample");
    return ok;
}

bool test_incomplete_trailing_pair_is_dropped() {
    CaretPrefixSampler sampler(80);
    std::u16string text;
    text.push_back(u'a');
    text.push_back(static_cast<char16_t>(0xD83D));
    sampler.set_text(std::move(text), 2);
    return check(sampler.text_before_caret() == u"a", "a trailing lone high surrogate is dropped");
}

bool test_randomized_matches_reference() {
    uint32_t state = 0x9E3779B9U;
    auto next = [&state]() {
        state = state * 1664525U + 1013904223U;
        return state >> 16U;
    };
    for (int iteration = 0; iteration < 500; ++iteration) {
        const size_t max_units = next() % 12U;
        std::u16string full;
        for (size_t i = 0, count = next() % 32U; i < count; ++i) {
            switch (next() % 4U) {
                case 0:
                    full.push_back(u'a' + static_cast<char16_t>(next() % 26U));
                    break;
                case 1:
                    full.push_back(static_cast<char16_t>(0x4E00 + next() % 0x100));
                    break;
                default:
                    full.push_back(static_cast<char16_t>(0xD83D));
                    full.push_back(static_cast<char16_t>(0xDE00 + next() % 0x100));
                    break;
            }
        }
        const size_t caret = next() % (full.size() + 1U);

        std::u16string reference = full.substr(0, caret);
        if (!reference.empty() && reference.back() >= 0xD800 && reference.back() <= 0xDBFF) {
            reference.pop_back();
        }
        if (reference.size() > max_units) {
            size_t start = reference.size() - max_units;
            if (start > 0 && reference[start] >= 0xDC00 && reference[start] <= 0xDFFF &&
                reference[start - 1] >= 0xD800 && reference[start - 1] <= 0xDBFF) {
                ++start;
            }
            reference.erase(0, start);
        }

        CaretPrefixSampler sampler(max_units);
        sampler.set_text(full, caret);
        if (!check(sampler.text_before_caret() == reference, "randomized sample matches the reference") ||
            !check(sampler.text_before_caret().size() <= max_units, "randomized sample respects the cap")) {
            std::printf("  caret=%zu full_size=%zu max=%zu got=%zu want=%zu\n", caret, full.size(),
                        max_units, sampler.text_before_caret().size(), reference.size());
            return false;
        }
    }
    return true;
}

}  // namespace
}  // namespace llavon::ime

int run_caret_prefix_sampler_tests() {
    using namespace llavon::ime;
    bool ok = true;
    ok &= test_simple_prefix();
    ok &= test_multiline_prefix_is_kept();
    ok &= test_window_trim_drops_oldest();
    ok &= test_no_text_not_sampled();
    ok &= test_empty_pre_caret();
    ok &= test_caret_past_end_clamped();
    ok &= test_window_trim_keeps_pairs_whole();
    ok &= test_caret_inside_pair_drops_incomplete_scalar();
    ok &= test_reset_to_empty_clears_state();
    ok &= test_incomplete_trailing_pair_is_dropped();
    ok &= test_randomized_matches_reference();
    if (ok) std::printf("caret prefix sampler tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

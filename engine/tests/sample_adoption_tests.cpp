#include "test_suites.h"
#include "context/sample_adoption.hpp"

#include <cstdlib>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace llavon::ime {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

std::optional<std::u16string> strip(std::u16string_view sample,
                                    std::initializer_list<PreeditSegmentState> segments) {
    const std::vector<PreeditSegmentState> states(segments);
    return strip_preedit_suffix(sample, states);
}

bool test_no_segments_keeps_sample() {
    const auto text = strip(u"早安，世界", {});
    return check(text.has_value() && *text == u"早安，世界", "no composition keeps the sample");
}

bool test_strips_current_rendered_preedit() {
    // The widget already shows the selected candidate.
    const auto text = strip(u"早安你", {{u"你", u"ㄋㄧˇ"}});
    return check(text.has_value() && *text == u"早安", "the rendered preedit is stripped");
}

bool test_strips_lagged_reading_preedit() {
    // The client still shows the previous keystroke's reading, a prefix of
    // the current reading.
    const auto text = strip(u"早安ㄋㄧ", {{u"ㄋㄧˇ", u"ㄋㄧˇ"}});
    return check(text.has_value() && *text == u"早安", "a lagged reading preedit is stripped");
}

bool test_strips_full_reading_when_candidate_visible() {
    const auto text = strip(u"早安ㄋㄧˇ", {{u"你", u"ㄋㄧˇ"}});
    return check(text.has_value() && *text == u"早安", "the full reading is stripped when a candidate is shown");
}

bool test_strips_multi_segment_composition() {
    // The first segment already shows its candidate while the second is still
    // being typed, which is the common multi-syllable case.
    const auto text = strip(u"早安你ㄏ", {{u"你", u"ㄋㄧˇ"}, {u"好", u"ㄏㄠˇ"}});
    return check(text.has_value() && *text == u"早安", "a multi-segment composition is stripped");
}

bool test_strips_when_earlier_segment_lags_as_reading() {
    // The sample still shows the first segment as a reading while the engine
    // has already selected a candidate for it.
    const auto text = strip(u"早安ㄋㄧˇㄏ", {{u"你", u"ㄋㄧˇ"}, {u"好", u"ㄏㄠˇ"}});
    return check(text.has_value() && *text == u"早安", "a mixed segment state is stripped");
}

bool test_sample_equal_to_preedit_is_empty_prefix() {
    const auto text = strip(u"ㄋㄧˇ", {{u"你", u"ㄋㄧˇ"}});
    return check(text.has_value() && text->empty(), "a sample that is only the preedit yields an empty prefix");
}

bool test_window_truncated_inside_preedit_is_rejected() {
    // The sample window kept only a suffix of the reading, so the text before
    // it is unknown and the sample must not be adopted.
    const auto text = strip(u"ㄧˇ", {{u"你", u"ㄋㄧˇ"}});
    return check(!text.has_value(), "a sample cut inside the preedit is rejected");
}

bool test_unrelated_text_is_rejected_while_composing() {
    // Preedit-free samples are handled by the engine's sequence check, not
    // here: an unrelated tail while composing is treated as unusable.
    const auto text = strip(u"早安，世界", {{u"你", u"ㄋㄧˇ"}});
    return check(!text.has_value(), "unrelated text is rejected while composing");
}

bool test_ambiguous_committed_tail_with_missing_segment_is_rejected() {
    // "早安" is committed text and the user composes the same word again, but
    // the client has not exposed the second segment. Stripping "早" would be
    // indistinguishable from deleting committed text.
    const auto text = strip(u"早安早", {{u"早", u"ㄗㄠˇ"}, {u"安", u"ㄢ"}});
    return check(!text.has_value(), "an ambiguous tail with a missing segment is rejected");
}

bool test_missing_trailing_segment_is_rejected() {
    // A client that renders preedit out-of-line may expose only committed text.
    // A later segment must not match an empty suffix and erase that text.
    const auto text = strip(u"你", {{u"你", u"ㄋㄧˇ"}, {u"好", u"ㄏㄠˇ"}});
    return check(!text.has_value(), "a missing trailing segment cannot consume committed text");
}

bool test_empty_sample_yields_empty_prefix() {
    const auto text = strip(u"", {{u"你", u"ㄋㄧˇ"}});
    return check(text.has_value() && text->empty(), "an empty sample yields an empty prefix");
}

bool test_surrogate_pair_preedit() {
    const auto text = strip(u"字\U0001F600", {{u"\U0001F600", u"\U0001F600"}});
    bool ok = check(text.has_value() && *text == u"字", "a surrogate pair preedit is stripped whole");
    std::u16string half_pair = u"字";
    half_pair.push_back(static_cast<char16_t>(0xDE00));
    const auto partial = strip(half_pair, {{u"\U0001F600", u"\U0001F600"}});
    ok &= check(!partial.has_value(), "a half pair does not match the preedit");
    return ok;
}

}  // namespace
}  // namespace llavon::ime

int run_sample_adoption_tests() {
    using namespace llavon::ime;
    bool ok = true;
    ok &= test_no_segments_keeps_sample();
    ok &= test_strips_current_rendered_preedit();
    ok &= test_strips_lagged_reading_preedit();
    ok &= test_strips_full_reading_when_candidate_visible();
    ok &= test_strips_multi_segment_composition();
    ok &= test_strips_when_earlier_segment_lags_as_reading();
    ok &= test_sample_equal_to_preedit_is_empty_prefix();
    ok &= test_window_truncated_inside_preedit_is_rejected();
    ok &= test_unrelated_text_is_rejected_while_composing();
    ok &= test_ambiguous_committed_tail_with_missing_segment_is_rejected();
    ok &= test_missing_trailing_segment_is_rejected();
    ok &= test_empty_sample_yields_empty_prefix();
    ok &= test_surrogate_pair_preedit();
    if (ok) std::printf("sample adoption tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

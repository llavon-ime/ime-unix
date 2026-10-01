#include "test_suites.h"
#include "input/mixed_input_decoder.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "engine/fallback_engine.hpp"
#include "text/utf.hpp"

namespace llavon::ime {

namespace {

std::u16string utf16(const char* text) {
    return utf8_to_u16(text);
}

std::string u8(const std::u16string& text) {
    return u16_to_utf8(text);
}

class DecoderTest {
public:
    explicit DecoderTest(std::filesystem::path table_path)
        : fallback_(std::move(table_path)),
          decoder_([this](std::u16string_view reading) { return fallback_.lookup(reading); },
                   [this](std::u16string_view word) { return fallback_.latin_frequency(word); }) {}

    bool run() {
        bool ok = true;
        ok &= test_su3();
        ok &= test_hello_space();
        ok &= test_gmail_domain();
        ok &= test_gmail_283();
        ok &= test_gmail_5j_space();
        ok &= test_hello4();
        ok &= test_mp3();
        ok &= test_hsu_nef();
        ok &= test_hsu_hd();
        ok &= test_hsu_if();
        ok &= test_hsu_dyf();
        ok &= test_space_first_tone();
        ok &= test_qwerty_283();
        ok &= test_lossless_cover();
        ok &= test_raw_fallback_always_first();
        ok &= test_literal_tie_guard();
        ok &= test_no_gap_paths();
        ok &= test_dedup();
        ok &= test_long_input_performance();
        ok &= test_expand_candidates();
        ok &= test_exhaustive_standard();
        ok &= test_exhaustive_hsu();
        ok &= test_fuzz();
        ok &= test_performance();
        if (ok) std::printf("mixed input decoder tests passed\n");
        return ok;
    }

private:
    bool test_literal_tie_guard() {
        const MixedInputDecoder tied([](std::u16string_view reading) -> std::vector<char32_t> {
                if (reading == u"ㄨ ") return {U'屋', U'巫'};
                if (reading == u"ㄕ ") return {U'失', U'師'};
                if (reading == u"ㄙㄢ ") return {U'三'};
                return {};
            },
            [this](auto word) { return fallback_.latin_frequency(word); },
            [](std::u16string_view, bool) { return 0.0; },
            [](std::u16string_view, char32_t ch) {
                return ch == U'三' ? -2.0 * (3.4 + 0.8) : -(3.4 + 0.4);
            });
        // Identical literal/reading costs must preserve the raw tie winner,
        // independent of equal-score beam ordering and lexical-span proposals.
        const auto result = tied.decode(u"j g n0 ", BopomofoKeyboardLayout::Standard, false);
        if (result.best_path == 0) return true;
        std::printf("[FAIL] literal tie was reclassified\n");
        return false;
    }

    bool find_path(const MixedDecodeResult& result, const std::u16string& rendered) const {
        for (const auto& path : result.paths) {
            if (path.rendered == rendered) return true;
        }
        return false;
    }

    bool check_path(const char* name, const MixedDecodeResult& result, const std::u16string& rendered) {
        if (find_path(result, rendered)) return true;
        std::printf("[FAIL] %s: no path renders \"%s\"\n", name, u8(rendered).c_str());
        for (const auto& path : result.paths) {
            std::printf("  path \"%s\" (%.1f)\n", u8(path.rendered).c_str(), path.score);
        }
        return false;
    }

    bool test_su3() {
        const auto result = decoder_.decode(utf16("su3"), BopomofoKeyboardLayout::Standard, false);
        bool ok = result.raw == utf16("su3");
        ok &= result.paths.size() >= 2;
        ok &= check_path("su3", result, utf16("你"));
        return ok;
    }

    bool test_hello_space() {
        const auto result = decoder_.decode(utf16("hello"), BopomofoKeyboardLayout::Standard, true);
        bool ok = result.raw == utf16("hello");
        ok &= result.paths.size() == 1;  // no Chinese interference
        return ok;
    }

    bool test_gmail_domain() {
        // "gmail.com" must never split "com" as Bopomofo.
        const auto result = decoder_.decode(utf16("gmail.com"), BopomofoKeyboardLayout::Standard, true);
        bool ok = find_path(result, utf16("gmail.com"));
        for (const auto& path : result.paths) {
            for (const auto& segment : path.segments) {
                ok &= segment.kind != MixedSegmentKind::Bopomofo;
            }
        }
        if (!ok) std::printf("[FAIL] gmail.com split into Bopomofo\n");
        return ok;
    }

    bool test_gmail_283() {
        const auto result = decoder_.decode(utf16("gmail.com283"), BopomofoKeyboardLayout::Standard, false);
        bool ok = result.raw == utf16("gmail.com283");
        ok &= result.paths.front().rendered == utf16("gmail.com283");  // raw fallback first
        ok &= check_path("gmail.com283", result, utf16("gmail.com打"));
        return ok;
    }

    bool test_gmail_5j_space() {
        const auto result = decoder_.decode(utf16("gmail.com5j/"), BopomofoKeyboardLayout::Standard, true);
        bool ok = result.paths.front().rendered == utf16("gmail.com5j/");
        ok &= check_path("gmail.com5j/", result, utf16("gmail.com中"));
        return ok;
    }

    bool test_hello4() {
        const auto result = decoder_.decode(utf16("hello4"), BopomofoKeyboardLayout::Standard, false);
        bool ok = result.paths.front().rendered == utf16("hello4");
        ok &= check_path("hello4", result, utf16("hell欸"));
        return ok;
    }

    bool test_mp3() {
        const auto result = decoder_.decode(utf16("mp3"), BopomofoKeyboardLayout::Standard, true);
        bool ok = result.paths.front().rendered == utf16("mp3");
        return ok;
    }

    bool test_hsu_nef() {
        const auto result = decoder_.decode(utf16("nef"), BopomofoKeyboardLayout::Hsu, false);
        bool ok = check_path("nef", result, utf16("你"));
        return ok;
    }

    bool test_hsu_hd() {
        const auto result = decoder_.decode(utf16("hd"), BopomofoKeyboardLayout::Hsu, false);
        bool ok = check_path("hd", result, utf16("哦"));
        ok &= check_path("hd", result, utf16("hd"));  // raw fallback always exists
        return ok;
    }

    bool test_hsu_if() {
        const auto result = decoder_.decode(utf16("if"), BopomofoKeyboardLayout::Hsu, false);
        bool ok = result.paths.front().rendered == utf16("if");
        ok &= check_path("if", result, utf16("矮"));
        return ok;
    }

    bool test_hsu_dyf() {
        const auto result = decoder_.decode(utf16("dyf"), BopomofoKeyboardLayout::Hsu, false);
        bool ok = check_path("dyf", result, utf16("打"));
        return ok;
    }

    bool test_space_first_tone() {
        bool ok = true;
        const auto rup = decoder_.decode(utf16("rup"), BopomofoKeyboardLayout::Standard, true);
        ok &= check_path("rup", rup, utf16("今"));
        const auto wu0 = decoder_.decode(utf16("wu0"), BopomofoKeyboardLayout::Standard, true);
        ok &= check_path("wu0", wu0, utf16("天"));
        const auto ten = decoder_.decode(utf16("10"), BopomofoKeyboardLayout::Standard, true);
        ok &= check_path("10", ten, utf16("班"));
        const auto u = decoder_.decode(utf16("u"), BopomofoKeyboardLayout::Standard, true);
        ok &= check_path("u", u, utf16("一"));
        return ok;
    }

    bool test_qwerty_283() {
        // Suffix parsing is grammatical and does not depend on the lexicon.
        const auto result = decoder_.decode(utf16("qwerty283"), BopomofoKeyboardLayout::Standard, false);
        bool ok = check_path("qwerty283", result, utf16("qwerty打"));
        return ok;
    }

    bool test_lossless_cover() {
        // Every path's segments must cover the raw input exactly once.
        const std::u16string raw = utf16("gmail.com5j/283hello4");
        const auto result = decoder_.decode(raw, BopomofoKeyboardLayout::Standard, true);
        bool ok = true;
        for (const auto& path : result.paths) {
            std::u16string covered;
            for (const auto& segment : path.segments) {
                if (segment.begin != covered.size()) ok = false;
                if (segment.end > raw.size()) ok = false;
                if (raw.substr(segment.begin, segment.end - segment.begin) != segment.raw) ok = false;
                covered.resize(segment.end);
            }
            if (covered.size() != raw.size()) ok = false;
        }
        if (!ok) std::printf("[FAIL] path coverage broken for %s\n", u8(raw).c_str());
        return ok;
    }

    bool test_raw_fallback_always_first() {
        const auto result = decoder_.decode(utf16("gmail.com283"), BopomofoKeyboardLayout::Standard, false);
        bool ok = !result.paths.empty() && result.paths.front().rendered == utf16("gmail.com283");
        return ok;
    }

    bool test_no_gap_paths() {
        // Decoding the same raw text twice must produce identical raw coverage.
        const auto result = decoder_.decode(utf16("user@example.com283"), BopomofoKeyboardLayout::Standard, false);
        bool ok = !result.paths.empty() && result.paths.front().rendered == utf16("user@example.com283");
        ok &= check_path("email suffix", result, utf16("user@example.com打"));
        return ok;
    }

    bool test_dedup() {
        const auto result = decoder_.decode(utf16("mp3283"), BopomofoKeyboardLayout::Standard, false);
        bool ok = !result.paths.empty();
        std::vector<std::u16string> rendered;
        for (const auto& path : result.paths) rendered.push_back(path.rendered);
        for (size_t i = 0; i < rendered.size(); ++i) {
            for (size_t j = i + 1; j < rendered.size(); ++j) {
                if (rendered[i] == rendered[j]) {
                    ok = false;
                    std::printf("[FAIL] duplicate rendered path \"%s\"\n", u8(rendered[i]).c_str());
                }
            }
        }
        return ok;
    }

    bool test_long_input_performance() {
        std::u16string raw;
        for (size_t i = 0; i < 256; ++i) raw.push_back(static_cast<char16_t>('a' + (i % 26)));
        const auto begin = std::chrono::steady_clock::now();
        const auto result = decoder_.decode(raw, BopomofoKeyboardLayout::Standard, false);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count();
        bool ok = !result.paths.empty() && result.paths.front().rendered == raw;
        ok &= elapsed < 100;
        if (!ok) std::printf("[FAIL] long input decode: %lld ms\n", static_cast<long long>(elapsed));
        return ok;
    }

    bool test_expand_candidates() {
        const auto result = decoder_.decode(utf16("gmail.com283"), BopomofoKeyboardLayout::Standard, false);
        const auto entries = decoder_.expand_candidates(result, 10);
        bool ok = !entries.empty() && entries.front().text == result.paths[result.best_path].rendered;
        bool found_raw = false;
        bool found_chinese = false;
        for (const auto& entry : entries) {
            found_raw |= entry.text == utf16("gmail.com283");
            found_chinese |= entry.text == utf16("gmail.com打");
        }
        ok &= found_raw && found_chinese;

        const auto hsu = decoder_.decode(utf16("gmail.comjxl"), BopomofoKeyboardLayout::Hsu, true);
        const auto hsu_entries = decoder_.expand_candidates(hsu, 10, 0);
        ok &= hsu_entries.size() >= 2 && hsu_entries[1].text == utf16("gmail.com中");

        return ok;
    }

    // Exhaustive validation: every valid Standard syllable must keep a
    // `prefix + Chinese` path after an English prefix, the exact raw ASCII
    // path must always exist, and every path must cover the raw input exactly
    // once without gaps or duplication.
    bool test_exhaustive_standard() {
        std::vector<char32_t> body_keys;
        for (char32_t key : {U'1', U'2', U'5', U'8', U'9', U'0', U'-', U'q', U'w', U'e', U'r', U't', U'y', U'u',
                             U'i', U'o', U'p', U'a', U's', U'd', U'f', U'g', U'h', U'j', U'k', U'l', U';', U'z',
                             U'x', U'c', U'v', U'b', U'n', U'm', U',', U'.', U'/'}) {
            body_keys.push_back(key);
        }
        const std::vector<char32_t> tones = {U'3', U'6', U'4', U'7'};
        const std::u16string prefix = utf16("hello");

        size_t valid_readings = 0;
        bool ok = true;
        for (const char32_t key : body_keys) {
            for (const char32_t tone : tones) {
                ok &= verify_mixed_suffix(prefix, key, tone, BopomofoKeyboardLayout::Standard, &valid_readings);
            }
        }
        for (const char32_t first : body_keys) {
            for (const char32_t second : body_keys) {
                for (const char32_t tone : tones) {
                    ok &= verify_mixed_suffix(prefix, first, second, tone, BopomofoKeyboardLayout::Standard,
                                              &valid_readings);
                }
            }
        }
        for (const char32_t first : body_keys) {
            for (const char32_t second : body_keys) {
                for (const char32_t third : body_keys) {
                    for (const char32_t tone : tones) {
                        ok &= verify_mixed_suffix(prefix, first, second, third, tone, BopomofoKeyboardLayout::Standard,
                                                  &valid_readings);
                    }
                }
            }
        }
        for (const char32_t first : body_keys) {
            for (const char32_t second : body_keys) {
                ok &= verify_mixed_suffix(prefix, first, second, U' ', BopomofoKeyboardLayout::Standard,
                                          &valid_readings);
            }
        }
        if (valid_readings == 0) {
            std::printf("[FAIL] exhaustive standard: no valid readings enumerated\n");
            ok = false;
        }
        std::printf("exhaustive standard: %zu valid suffix readings\n", valid_readings);
        return ok;
    }

    bool test_exhaustive_hsu() {
        std::vector<char32_t> keys;
        for (char32_t key = U'a'; key <= U'z'; ++key) keys.push_back(key);
        const std::vector<char32_t> tones = {U'd', U'f', U'j', U's'};
        const std::u16string prefix = utf16("hello");

        size_t valid_readings = 0;
        bool ok = true;
        for (const char32_t key : keys) {
            for (const char32_t tone : tones) {
                ok &= verify_mixed_suffix(prefix, key, tone, BopomofoKeyboardLayout::Hsu, &valid_readings);
            }
        }
        for (const char32_t first : keys) {
            for (const char32_t second : keys) {
                for (const char32_t tone : tones) {
                    ok &= verify_mixed_suffix(prefix, first, second, tone, BopomofoKeyboardLayout::Hsu,
                                              &valid_readings);
                }
            }
        }
        for (const char32_t first : keys) {
            for (const char32_t second : keys) {
                ok &= verify_mixed_suffix(prefix, first, second, U' ', BopomofoKeyboardLayout::Hsu, &valid_readings);
            }
        }
        if (valid_readings == 0) {
            std::printf("[FAIL] exhaustive hsu: no valid readings enumerated\n");
            ok = false;
        }
        std::printf("exhaustive hsu: %zu valid suffix readings\n", valid_readings);
        return ok;
    }

    bool test_fuzz() {
        bool ok = true;
        std::mt19937 rng(0xC0FFEE);
        const std::u16string alphabet = utf16("abcdefghijklmnopqrstuvwxyz0123456789.@/_-+:");
        for (size_t round = 0; round < 200; ++round) {
            std::u16string raw;
            const size_t length = 1 + rng() % 40;
            for (size_t i = 0; i < length; ++i) raw.push_back(alphabet[rng() % alphabet.size()]);
            const auto layout = (rng() % 2) == 0 ? BopomofoKeyboardLayout::Standard : BopomofoKeyboardLayout::Hsu;
            const auto result = decoder_.decode(raw, layout, (rng() % 2) == 0);
            ok &= !result.paths.empty();
            ok &= result.paths.front().rendered == raw;
            ok &= covers_exactly_once(raw, result);
            ok &= no_duplicate_rendered(result);
        }
        if (!ok) std::printf("[FAIL] fuzz decode\n");
        return ok;
    }

    bool test_performance() {
        bool ok = true;
        const std::vector<std::u16string> samples = {
            utf16("user@example.com/very/long/path/with/many/segments1234567890"),
            utf16("https://secure.example.org/downloads/release/v2.14.3-beta1.tar.gz"),
            utf16("int main() { return std::accumulate(begin, end, 0); } // 256-char"),
            utf16("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz")};
        for (const auto& sample : samples) {
            const auto begin = std::chrono::steady_clock::now();
            const auto result = decoder_.decode(sample, BopomofoKeyboardLayout::Standard, false);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
            ok &= !result.paths.empty() && result.paths.front().rendered == sample;
            if (elapsed > 100'000) {
                std::printf("[FAIL] decode of %zu chars took %lld us\n", sample.size(),
                            static_cast<long long>(elapsed));
                ok = false;
            }
        }
        return ok;
    }

    bool verify_mixed_suffix(const std::u16string& prefix, char32_t key, char32_t tone,
                             BopomofoKeyboardLayout layout, size_t* valid_count) {
        return verify_mixed_suffix(prefix, key, 0, 0, tone, layout, valid_count);
    }

    bool verify_mixed_suffix(const std::u16string& prefix, char32_t first, char32_t second, char32_t tone,
                             BopomofoKeyboardLayout layout, size_t* valid_count) {
        return verify_mixed_suffix(prefix, first, second, 0, tone, layout, valid_count);
    }

    // Verifies that a suffix formed by the given body keys and tone keeps a
    // mixed path after the English prefix. Only paths are checked that would
    // be structurally useful to the engine: explicit-tone suffixes of any
    // body length, first-tone (space) suffixes with at least two body keys.
    bool verify_mixed_suffix(const std::u16string& prefix, char32_t first, char32_t second, char32_t third,
                             char32_t tone, BopomofoKeyboardLayout layout, size_t* valid_count) {
        if (tone == U' ' && second == 0) return true;  // 1-key space suffix stays English
        if (tone == U' ' && third != 0) return true;

        std::u16string body;
        body.push_back(static_cast<char16_t>(first));
        if (second != 0) body.push_back(static_cast<char16_t>(second));
        if (third != 0) body.push_back(static_cast<char16_t>(third));

        CompositionBuffer scratch;
        const auto result = scratch.add_bopomofo_keys(body, tone, layout, true);
        if (!result || !result->completed || scratch.segments().size() != 1) return true;
        const auto reading = scratch.segments().front().reading();
        if (fallback_.lookup(reading).empty()) return true;
        ++*valid_count;

        const bool space_tone = tone == U' ';
        const auto raw = prefix + body + (space_tone ? std::u16string() : std::u16string(1, static_cast<char16_t>(tone)));
        const auto decoded = decoder_.decode(raw, layout, space_tone);

        bool ok = !decoded.paths.empty() && decoded.paths.front().rendered == raw;
        bool found = false;
        for (const auto& path : decoded.paths) {
            if (path.segments.empty() || path.segments.back().kind != MixedSegmentKind::Bopomofo) continue;
            const auto& tail = path.segments.back();
            if (tail.begin == prefix.size() && tail.end == raw.size() && tail.reading == reading) found = true;
        }
        if (!found) {
            std::printf("[FAIL] no mixed path for %s + \"%s\" + tone %d (%s)\n",
                        u8(prefix).c_str(), u8(body).c_str(), static_cast<int>(tone), u8(reading).c_str());
            ok = false;
        }
        ok &= covers_exactly_once(raw, decoded);
        return ok;
    }

    bool covers_exactly_once(const std::u16string& raw, const MixedDecodeResult& result) {
        for (const auto& path : result.paths) {
            std::u16string covered;
            for (const auto& segment : path.segments) {
                if (segment.begin != covered.size()) return false;
                if (segment.end > raw.size()) return false;
                covered.resize(segment.end);
            }
            if (covered.size() != raw.size()) return false;
        }
        return true;
    }

    bool no_duplicate_rendered(const MixedDecodeResult& result) {
        std::vector<std::u16string> rendered;
        for (const auto& path : result.paths) rendered.push_back(path.rendered);
        for (size_t i = 0; i < rendered.size(); ++i) {
            for (size_t j = i + 1; j < rendered.size(); ++j) {
                if (rendered[i] == rendered[j]) return false;
            }
        }
        return true;
    }

    FallbackEngine fallback_;
    MixedInputDecoder decoder_;
};

}  // namespace

}  // namespace llavon::ime

int run_mixed_input_decoder_tests() {
    llavon::ime::DecoderTest test(LLAVON_IME_TEST_TABLE_PATH);
    return test.run() ? EXIT_SUCCESS : EXIT_FAILURE;
}

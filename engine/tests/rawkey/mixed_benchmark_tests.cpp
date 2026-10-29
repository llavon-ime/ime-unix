#include "raw_key_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

using namespace llavon::ime::rawkey;

namespace {

std::string visible_text(const Harness& harness) {
    std::string text;
    for (const auto& commit : harness.commits()) text += commit;
    text += harness.preedit();
    return text;
}

bool closed_ascii_boundary(std::string_view text, std::string_view token) {
    const auto position = text.rfind(token);
    if (position == std::string_view::npos) return false;
    const auto suffix = text.substr(position + token.size());
    return std::ranges::any_of(suffix, [](char byte) {
        return static_cast<unsigned char>(byte) >= 0x80;
    });
}

}  // namespace

RAWKEY_SUITE("mixed input benchmark", engine_test_mixed_benchmark) {
    struct Sample { const char* keys; const char* expected; };
    const Sample samples[] = {
        {"hello", "hello"}, {"adds", "adds"}, {"added", "added"},
        {"adding", "adding"}, {"fixed", "fixed"}, {"stopped", "stopped"},
        {"nefarious", "nefarious"}, {"keyboard", "keyboard"},
        {"friend", "friend"}, {"world", "world"}, {"wonderful", "wonderful"},
        {"florpington", "florpington"}, {"end-to-end", "end-to-end"},
        {"it's", "it's"}, {"we'd", "we'd"}, {"utf8", "utf8"},
        {"user_name", "user_name"}, {"https://example.org", "https://example.org"},
    };
    size_t correct = 0;
    size_t total = 0;
    std::vector<long long> latency;
    for (const char* layout : {"標準", "許氏"}) {
        const auto check = [&](const Sample& sample) {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            for (const char* ch = sample.keys; *ch; ++ch) {
                const auto start = std::chrono::steady_clock::now();
                harness.key(Key(*ch));
                latency.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start).count());
            }
            ++total;
            if (harness.preedit() == sample.expected) ++correct;
            else std::printf("mixed benchmark mismatch [%s] %s => %s (expected %s)\n",
                             layout, sample.keys, harness.preedit().c_str(), sample.expected);
            harness.key("Return");
            RAWKEY_ASSERT(harness.composition_empty());
        };
        for (const auto& sample : samples) check(sample);
        if (std::string_view(layout) == "標準") {
            for (const Sample sample : {Sample{"su3", "你"}, {"283", "打"},
                                        {"su3cl3ji3", "你好我"}, {"hello283", "hello打"}}) check(sample);
        } else {
            for (const Sample sample : {Sample{"nef", "你"}, {"dyf", "打"},
                                        {"nefhwfxhf", "你好我"}, {"nefhello", "你hello"}}) check(sample);
        }
    }
    std::sort(latency.begin(), latency.end());
    std::printf("mixed benchmark: %zu/%zu exact previews; key latency p50=%lldus p95=%lldus\n",
                correct, total, latency[latency.size() / 2], latency[latency.size() * 95 / 100]);
    RAWKEY_ASSERT(correct == total);
}

RAWKEY_SUITE("mixed input editing and context", engine_test_mixed_editing) {
    for (const char* layout : {"標準", "許氏"}) {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
        harness.type("hello world ");
        RAWKEY_ASSERT(harness.preedit() == "hello world ");
        RAWKEY_ASSERT(harness.commits().empty());
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "hello world");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "hello worl");
        harness.type("d ");
        harness.type(std::string_view(layout) == "標準" ? "su3" : "nef");
        RAWKEY_ASSERT(harness.preedit() == "hello world 你");
        harness.expect_commit("hello world 你");
        RAWKEY_ASSERT(harness.last_commit() == "hello world 你");
    }
    {
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", "許氏"}, {"SmartEnglish", "True"}});
        harness.type("gen ");
        RAWKEY_ASSERT(harness.preedit() == "今");
        harness.key("Shift+BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "gen");
        harness.type("eral");
        RAWKEY_ASSERT(harness.preedit() == "general");
        harness.expect_commit("general");
        harness.type("nef");
        harness.key("Down");
        harness.choose_text("你");
        harness.type("farious");
        RAWKEY_ASSERT(harness.preedit() == "你farious");
        harness.expect_commit("你farious");
        harness.type("if");
        harness.key("Down");
        harness.choose_text("if");
        harness.type("nef");
        RAWKEY_ASSERT(harness.preedit() == "if你");
        harness.expect_commit("if你");
    }
    {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type("ru6");
        RAWKEY_ASSERT(harness.preedit() == "及");
        harness.reset();
        harness.set_surrounding("年", 1, 1);
        harness.type("ru6");
        RAWKEY_ASSERT(harness.context_text() == "年");
        RAWKEY_ASSERT(harness.preedit() == "級");
        harness.key("Down");
        harness.choose_text("及");
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "及好");
        harness.expect_commit("及好");
    }
}

RAWKEY_SUITE("mixed ASCII boundaries close within one syllable", mixed_ascii_boundary_closure) {
    struct Sample {
        const char* prefix_keys;
        const char* token;
        const char* visible_token;
        const char* following_keys;
        std::size_t completion_keys;
    };
    const Sample samples[] = {
        {"g.3ek4GPTu.6OpenAIm62018", "2018", "2018", "su06wjo tj ", 4},
        {"GPTai6vu/6g4ru m6Transformer", "Transformer", "Transformer", "ai6vu/62k7xo4", 3},
        // P and T pass directly to the host after G commits the composition;
        // the harness can observe only the pending suffix of this island.
        {"w8 ap704gjp4vm41u0 cl4e.4t/6xk7GPT-n", "GPT-n", "-n", "vu4xu,4", 3},
    };

    for (const auto& sample : samples) {
        Harness harness;
        harness.set_config("SmartEnglish", "True");
        harness.type(sample.prefix_keys);
        RAWKEY_ASSERT(!closed_ascii_boundary(visible_text(harness), sample.visible_token));

        bool closed = false;
        std::size_t keys = 0;
        for (const char key : std::string_view(sample.following_keys)) {
            harness.key(Key(key));
            ++keys;
            const bool current = closed_ascii_boundary(visible_text(harness), sample.visible_token);
            if (closed && !current) {
                throw Failure{std::string(sample.token) + " boundary reopened after key " + key};
            }
            closed = closed || current;
            if (keys == sample.completion_keys && !closed) {
                throw Failure{std::string(sample.token) + " boundary remained open after " +
                              std::to_string(keys) + " keys: " + visible_text(harness)};
            }
        }
        RAWKEY_ASSERT(closed);
    }

    // Closing a mixed boundary must not make a standalone English sentence
    // look Chinese merely because one suffix can also spell a reading.
    Harness english;
    english.set_config("SmartEnglish", "True");
    english.type("transformer models in 2026");
    RAWKEY_ASSERT(visible_text(english) == "transformer models in 2026");
}

RAWKEY_SUITE("mixed input technical vocabulary and long lines", engine_test_mixed_vocabulary) {
    std::vector<long long> long_latency;
    for (const char* layout : {"標準", "許氏"}) {
        for (const char* word : {"api", "json", "javascript", "typescript", "postgresql", "websocket",
                                 "backspace", "permissions", "hyperparameter", "synchronization",
                                 "uninitialized", "llavon", "sandford", "xylophonic"}) {
            Harness harness;
            harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
            harness.type(word);
            if (harness.preedit() != word) throw Failure{std::string(layout) + ": " + word + " => " + harness.preedit()};
            harness.expect_space_then_commit(std::string(word) + " ");
        }
        Harness harness;
        harness.set_configs({{"BopomofoKeyboardLayout", layout}, {"SmartEnglish", "True"}});
        const std::string raw = std::string_view(layout) == "標準" ? "su3cl3ji3" : "nefhwfxhf";
        std::string expected;
        for (int repeat = 0; repeat < 12; ++repeat) {
            for (const auto ch : raw) {
                const auto start = std::chrono::steady_clock::now();
                harness.key(Key(ch));
                long_latency.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
            }
            expected += "你好我";
            RAWKEY_ASSERT(harness.preedit() == expected);
            RAWKEY_ASSERT(harness.commits().empty());
        }
        harness.expect_commit(expected);
        RAWKEY_ASSERT(harness.last_commit() == expected);
    }
    std::sort(long_latency.begin(), long_latency.end());
    std::printf("mixed long-line benchmark: p95=%lldus max=%lldus (108 raw keys per layout)\n",
                long_latency[long_latency.size() * 95 / 100], long_latency.back());
    RAWKEY_ASSERT(long_latency[long_latency.size() * 95 / 100] < 20'000);
}

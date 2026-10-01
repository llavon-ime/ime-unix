// Offline experiment using the existing service protocol. No production
// behaviour or ime-core API is changed. Quality mismatches are observations;
// malformed fixtures, invalid readings and transport failures are errors.
#include "rawkey/raw_key_harness.hpp"

#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "input/mixed_input_decoder.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using namespace llavon::ime;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct SocketDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                ("model-mixed-probe-" + std::to_string(getpid()));
    SocketDirectory() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("socket directory already exists");
    }
    ~SocketDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::u16string codepoint(char32_t ch) {
    return utf8_to_u16(char32_to_utf8(ch));
}

std::u16string keys_for(std::u16string_view reading, BopomofoKeyboardLayout layout) {
    if (reading.empty()) throw std::runtime_error("empty fixture reading");
    // This is fixture encoding only; replay through the actual keymap below
    // verifies every generated syllable before any measurement starts.
    const std::u16string symbols = u"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ ˊˇˋ˙";
    constexpr std::string_view hsu = "bpmfdtnlgkhjvcjvcrzasexuyhgeiawomnkll dfjs";
    if (symbols.size() != hsu.size()) throw std::runtime_error("invalid Hsu fixture mapping");
    std::u16string result;
    for (const auto symbol : reading) {
        if (layout == BopomofoKeyboardLayout::Hsu) {
            const auto at = symbols.find(symbol);
            if (at == std::u16string::npos) throw std::runtime_error("unknown fixture symbol");
            result.push_back(static_cast<char16_t>(hsu[at]));
        } else {
            bool found = false;
            for (char32_t key = U' '; key <= U'~'; ++key) {
                if (lookup_bopomofo_key(key, false) == symbol) {
                    result.push_back(static_cast<char16_t>(key));
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("unknown fixture symbol");
        }
    }
    CompositionBuffer replay;
    const auto completed = replay.add_bopomofo_keys(result.substr(0, result.size() - 1),
                                                   result.back(), layout, true);
    if (!completed || !completed->completed || replay.segments().size() != 1 ||
        replay.segments().front().reading() != reading) {
        throw std::runtime_error("fixture keymap roundtrip failed: " + u16_to_utf8(reading));
    }
    return result;
}

struct Fixture {
    std::string id;
    std::string category;
    std::u16string context;
    std::u16string expected;
    std::u16string raw;
    MixedPath oracle;
};

Fixture fixture_for(const Json& input, BopomofoKeyboardLayout layout, const FallbackEngine& fallback) {
    Fixture fixture;
    fixture.id = input.at("id").get<std::string>();
    fixture.category = input.at("category").get<std::string>();
    fixture.context = utf8_to_u16(input.value("context", std::string()));
    for (const auto& piece : input.at("pieces")) {
        MixedSegment segment;
        const auto text = utf8_to_u16(piece.at("text").get<std::string>());
        segment.begin = fixture.raw.size();
        if (piece.contains("reading")) {
            segment.kind = MixedSegmentKind::Bopomofo;
            segment.reading = utf8_to_u16(piece.at("reading").get<std::string>());
            segment.raw = keys_for(segment.reading, layout);
            segment.candidates = fallback.lookup(segment.reading);
            if (text.size() != 1 || std::ranges::find(segment.candidates, static_cast<char32_t>(text.front())) ==
                                      segment.candidates.end()) {
                throw std::runtime_error("fixture target not in reading table: " + fixture.id);
            }
        } else {
            segment.kind = MixedSegmentKind::Latin;
            if (std::ranges::any_of(text, [](char16_t ch) { return ch > 127; })) {
                throw std::runtime_error("literal fixture must contain raw ASCII keys: " + fixture.id);
            }
            segment.raw = text;
        }
        fixture.raw += segment.raw;
        segment.end = fixture.raw.size();
        fixture.expected += text;
        fixture.oracle.segments.push_back(std::move(segment));
    }
    return fixture;
}

Json reading_signature(const MixedPath& path) {
    Json result = Json::array();
    for (const auto& segment : path.segments) {
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            result.push_back({segment.begin, segment.end, u16_to_utf8(segment.reading)});
        }
    }
    return result;
}

class Model {
public:
    explicit Model(ServiceTransportOptions options) : transport_(std::move(options)) {
        const auto response = exchange([&](auto callback) { transport_.open_session(std::move(callback)); });
        session_ = std::get<protocol::OpenSessionResponse>(response).session_id;
    }

    std::u16string refine(const MixedPath& path, std::u16string context) {
        std::u16string output;
        std::vector<protocol::PaddingEntry> run;
        const auto flush = [&]() {
            if (run.empty()) return;
            const auto id = ++request_id_;
            const auto response = exchange([&](auto callback) {
                transport_.predict(session_, id, id, context + output, run, std::move(callback));
            });
            const auto& prediction = std::get<protocol::Prediction>(response);
            if (prediction.candidates.size() != run.size()) throw std::runtime_error("model run size mismatch");
            for (const auto& candidates : prediction.candidates) {
                if (candidates.empty()) throw std::runtime_error("empty model candidates");
                output += codepoint(candidates.front());
            }
            run.clear();
        };
        for (const auto& segment : path.segments) {
            if (segment.consumed_boundary) continue;
            if (segment.kind == MixedSegmentKind::Bopomofo) {
                run.push_back({false, segment.reading, 0});
            } else {
                flush();
                output += segment.raw;
            }
        }
        flush();
        return output;
    }

    std::uint64_t request_count() const { return request_id_; }

private:
    protocol::Message exchange(const std::function<void(ServiceTransport::Callback)>& send) {
        auto promise = std::make_shared<std::promise<protocol::Message>>();
        auto future = promise->get_future();
        send([promise](auto response) { promise->set_value(std::move(response)); });
        if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
            throw std::runtime_error("model service timeout");
        }
        auto response = future.get();
        if (const auto* error = std::get_if<protocol::Error>(&response)) {
            throw std::runtime_error("model service: " + error->message);
        }
        return response;
    }

    ServiceTransport transport_;
    protocol::SessionId session_{};
    std::uint64_t request_id_ = 0;
};

struct Observation {
    std::u16string text;
    long long latency_us = 0;
    std::uint64_t requests = 0;
    std::string error;
};

Observation observe(Model& model, const MixedPath& path, const std::u16string& context) {
    const auto before = model.request_count();
    const auto start = Clock::now();
    std::u16string text;
    std::string error;
    try {
        text = model.refine(path, context);
    } catch (const std::runtime_error& failure) {
        // A decoder can expose table readings absent from the model token
        // vocabulary. Retain that arm as a failed observation rather than
        // losing the entire report or counting it as a quality mismatch.
        error = failure.what();
        if (!error.starts_with("model service: invalid bpmf:")) throw;
    }
    return {text, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count(),
            model.request_count() - before, std::move(error)};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: model_mixed_probe SERVICE MODEL FIXTURES_JSON REPORT_JSON\n");
        return 1;
    }
    try {
        const std::vector<std::string> arguments(argv, argv + argc);
        const FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
        const auto lookup = [&](auto reading) { return fallback.lookup(reading); };
        const auto frequency = [&](auto word) { return fallback.latin_frequency(word); };
        const auto neutral_chinese = [](std::u16string_view, char32_t) { return -5.0; };
        const MixedInputDecoder baseline(lookup, frequency);
        // Same segmentation/English scoring, but no Chinese wordfreq or phrase
        // bonus. The inherited candidate-rank and structural priors remain.
        const MixedInputDecoder neutral(lookup, frequency, {}, neutral_chinese);
        // A separate ablation also removes English wordfreq and its internal
        // dictionary boundaries; it is deliberately untuned.
        const MixedInputDecoder no_lexicon(lookup, [](std::u16string_view) { return 0.0; },
            [](std::u16string_view, bool) { return -6.0; }, neutral_chinese);

        std::ifstream fixture_file(arguments[3]);
        if (!fixture_file) throw std::runtime_error("cannot open fixtures");
        const auto inputs = Json::parse(fixture_file);
        // Validate both keyboard encodings before starting the service.
        for (const auto layout : {BopomofoKeyboardLayout::Standard, BopomofoKeyboardLayout::Hsu}) {
            for (const auto& input : inputs) (void)fixture_for(input, layout, fallback);
        }
        ServiceTransportOptions options;
        const SocketDirectory socket_directory;
        options.service_path = arguments[1];
        options.model_path = arguments[2];
        options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path();
        options.socket_path = socket_directory.path / "service.sock";
        options.idle_timeout_seconds = 120;
        Model model(options);
        const MixedPath warmup = fixture_for(inputs.at(0), BopomofoKeyboardLayout::Standard, fallback).oracle;
        const auto cold = observe(model, warmup, {});

        Json rows = Json::array();
        for (const auto layout : {BopomofoKeyboardLayout::Standard, BopomofoKeyboardLayout::Hsu}) {
            const std::string layout_name = layout == BopomofoKeyboardLayout::Standard ? "standard" : "hsu";
            for (const auto& input : inputs) {
                const auto fixture = fixture_for(input, layout, fallback);
                // Baseline preview/commit uses actual raw-key engine events.
                rawkey::Harness harness;
                harness.set_configs({{"SmartEnglish", "True"},
                    {"BopomofoKeyboardLayout", layout == BopomofoKeyboardLayout::Standard ? "標準" : "許氏"}});
                harness.set_surrounding(u16_to_utf8(fixture.context), fixture.context.size(), fixture.context.size());
                harness.type(u16_to_utf8(fixture.raw));
                const auto displayed = harness.preedit();
                RAWKEY_ASSERT(harness.commits().empty());
                harness.expect_commit(displayed);
                RAWKEY_ASSERT(harness.composition_empty());

                const auto decoded = baseline.decode(fixture.raw, layout, false, fixture.context);
                const auto stripped = neutral.decode(fixture.raw, layout, false, fixture.context);
                const auto unlexicalized = no_lexicon.decode(fixture.raw, layout, false, fixture.context);
                const auto& base_path = decoded.paths.at(decoded.best_path);
                const auto& neutral_path = stripped.paths.at(stripped.best_path);
                const auto& unlexicalized_path = unlexicalized.paths.at(unlexicalized.best_path);
                if (u16_to_utf8(base_path.rendered) != displayed) {
                    throw std::runtime_error("direct decoder differs from raw-key preview: " + fixture.id);
                }
                const auto original_model = observe(model, base_path, fixture.context);
                const auto neutral_model = observe(model, neutral_path, fixture.context);
                const auto unlexicalized_model = observe(model, unlexicalized_path, fixture.context);
                const auto oracle_model = observe(model, fixture.oracle, fixture.context);
                Json row{{"id", fixture.id}, {"category", fixture.category}, {"layout", layout_name},
                    {"context", u16_to_utf8(fixture.context)}, {"raw", u16_to_utf8(fixture.raw)},
                    {"expected", u16_to_utf8(fixture.expected)},
                    {"baseline", displayed}, {"neutral", u16_to_utf8(neutral_path.rendered)},
                    {"baseline_model", u16_to_utf8(original_model.text)},
                    {"neutral_model", u16_to_utf8(neutral_model.text)},
                    {"no_lexicon_model", u16_to_utf8(unlexicalized_model.text)},
                    {"oracle_model", u16_to_utf8(oracle_model.text)}};
                const auto signature = reading_signature(fixture.oracle);
                row["baseline_boundaries_correct"] = reading_signature(base_path) == signature;
                row["neutral_boundaries_correct"] = reading_signature(neutral_path) == signature;
                row["no_lexicon_boundaries_correct"] = reading_signature(unlexicalized_path) == signature;
                row["baseline_boundary_in_beam"] = std::ranges::any_of(decoded.paths, [&](const auto& path) {
                    return reading_signature(path) == signature;
                });
                row["baseline_target_in_beam"] = std::ranges::any_of(decoded.paths, [&](const auto& path) {
                    return path.rendered == fixture.expected;
                });
                const auto page = baseline.expand_candidates(decoded, 9);
                row["baseline_target_in_first_page"] = std::ranges::any_of(page, [&](const auto& entry) {
                    return entry.text == fixture.expected;
                });
                row["target_candidate_ranks"] = Json::array();
                std::size_t expected_offset = 0;
                for (const auto& segment : fixture.oracle.segments) {
                    if (segment.kind == MixedSegmentKind::Bopomofo) {
                        const auto target = static_cast<char32_t>(fixture.expected.at(expected_offset));
                        const auto found = std::ranges::find(segment.candidates, target);
                        row["target_candidate_ranks"].push_back({
                            {"reading", u16_to_utf8(segment.reading)}, {"target", char32_to_utf8(target)},
                            {"rank", std::distance(segment.candidates.begin(), found) + 1},
                            {"candidate_count", segment.candidates.size()}});
                        ++expected_offset;
                    } else {
                        expected_offset += segment.raw.size();
                    }
                }
                row["baseline_readings"] = reading_signature(base_path);
                row["oracle_readings"] = signature;
                row["neutral_boundary_in_beam"] = std::ranges::any_of(stripped.paths, [&](const auto& path) {
                    return reading_signature(path) == signature;
                });
                const std::pair<std::string, Observation> observations[] = {
                    {"baseline_model", original_model}, {"neutral_model", neutral_model},
                    {"no_lexicon_model", unlexicalized_model}, {"oracle_model", oracle_model}};
                for (const auto& [name, observation] : observations) {
                    row[name + "_latency_us"] = observation.latency_us;
                    row[name + "_requests"] = observation.requests;
                    row[name + "_error"] = observation.error;
                }
                std::printf("[%s/%s] %s | baseline=%s | no-ZH+model=%s | oracle=%s\n", layout_name.c_str(),
                    fixture.category.c_str(), u16_to_utf8(fixture.expected).c_str(), displayed.c_str(),
                    u16_to_utf8(neutral_model.text).c_str(), u16_to_utf8(oracle_model.text).c_str());
                rows.push_back(std::move(row));
            }
        }
        Json summary = Json::object();
        for (const auto& row : rows) {
            for (const auto& group : {std::string("all"), row.at("layout").get<std::string>(),
                                     row.at("category").get<std::string>()}) {
                auto& count = summary[group];
                if (count.is_null()) count = Json::object();
                count["total"] = count.value("total", 0) + 1;
                for (const auto* arm : {"baseline", "neutral", "baseline_model", "neutral_model",
                                       "no_lexicon_model", "oracle_model"}) {
                    count[arm] = count.value(arm, 0) + (row.at(arm) == row.at("expected") ? 1 : 0);
                }
            }
        }
        Json report{{"service", arguments[1]}, {"model", arguments[2]},
            {"cold_first_prediction_us", cold.latency_us}, {"summary", summary}, {"rows", rows}};
        std::ofstream output(arguments[4]);
        if (!output) throw std::runtime_error("cannot create report");
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write report");
        std::printf("%s\n", summary.dump(2).c_str());
        return 0;
    } catch (const rawkey::Failure& error) {
        std::fprintf(stderr, "raw-key invariant failed: %s\n", error.message.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "experiment failed: %s\n", error.what());
    }
    return 1;
}

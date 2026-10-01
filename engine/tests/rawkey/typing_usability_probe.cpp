// A measured user-flow simulation, not a pass/fail accuracy assertion. Keys
// enter the real Engine through raw_key_harness; unhandled keys and commits
// are applied to a small client document so subsequent context is genuine.
#include "raw_key_harness.hpp"
#include "live_mixed_model_preview.hpp"

#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
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
#include <set>
#include <sstream>
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
using namespace llavon::ime::rawkey;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

long long microseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

std::size_t distance(std::u16string_view a, std::u16string_view b) {
    std::vector<std::size_t> previous(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) previous[j] = j;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::vector<std::size_t> next(b.size() + 1);
        next[0] = i + 1;
        for (std::size_t j = 0; j < b.size(); ++j) {
            next[j + 1] = std::min({next[j] + 1, previous[j + 1] + 1,
                                    previous[j] + (a[i] == b[j] ? 0U : 1U)});
        }
        previous = std::move(next);
    }
    return previous.back();
}

long long percentile(std::vector<long long> samples, std::size_t percent) {
    if (samples.empty()) return 0;
    std::ranges::sort(samples);
    return samples[std::min(samples.size() - 1, samples.size() * percent / 100)];
}

std::u16string encode(std::u16string reading, BopomofoKeyboardLayout layout) {
    if (reading.empty()) throw std::runtime_error("empty reading");
    if (reading.back() != u'ˊ' && reading.back() != u'ˇ' && reading.back() != u'ˋ' && reading.back() != u'˙') {
        reading.push_back(u' ');
    }
    const std::u16string symbols = u"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ ˊˇˋ˙";
    constexpr std::string_view hsu = "bpmfdtnlgkhjvcjvcrzasexuyhgeiawomnkll dfjs";
    constexpr std::string_view et26 = "bpmfdtnlvkhgvcgycjqwsexuaorwiqzpmntlh fjkd";
    const std::vector<std::u16string> cp26{
        u"q", u"qq", u"a", u"z", u"w", u"ww", u"s", u"x", u"e", u"d", u"c", u"r", u"f", u"v",
        u"t", u"tt", u"g", u"b", u"y", u"h", u"n", u"u", u"j", u"m", u"uu", u"i", u"k", u"b",
        u"ii", u"o", u"l", u"mm", u"oo", u"p", u"ll", u"n", u"pp", u" ", u"e", u"r", u"d", u"y"};
    std::u16string raw;
    for (const auto symbol : reading) {
        if (is_compact_bopomofo_layout(layout)) {
            const auto at = symbols.find(symbol);
            if (at == std::u16string::npos) throw std::runtime_error("unknown reading symbol");
            if (layout == BopomofoKeyboardLayout::DachenCp26) {
                // CP26's vowel cycles depend on an already typed medial:
                // u+u+u gives ㄧㄚ, j+u gives ㄨㄚ; u+m gives ㄧㄡ.
                if (symbol == u'ㄚ' && reading.find(u'ㄨ') != std::u16string::npos) raw += u"u";
                else if (symbol == u'ㄡ' && (reading.find(u'ㄧ') != std::u16string::npos ||
                                          reading.find(u'ㄨ') != std::u16string::npos)) raw += u"m";
                else raw += cp26.at(at);
            }
            else raw.push_back(static_cast<char16_t>((layout == BopomofoKeyboardLayout::Hsu ? hsu : et26).at(at)));
        } else {
            bool found = false;
            for (char32_t key = U' '; key <= U'~'; ++key) {
                if (lookup_bopomofo_key(key, layout, false) == symbol) {
                    raw.push_back(static_cast<char16_t>(key));
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("unknown reading symbol");
        }
    }
    CompositionBuffer replay;
    const auto result = replay.add_bopomofo_keys(raw.substr(0, raw.size() - 1), raw.back(), layout, true);
    if (!result || !result->completed || replay.segments().size() != 1 ||
        replay.segments().front().reading() != reading) {
        throw std::runtime_error("keymap roundtrip: " + u16_to_utf8(reading));
    }
    return raw;
}

struct Unit { std::u16string raw; std::u16string expected; };

std::vector<Unit> units_for(const Json& parts, BopomofoKeyboardLayout layout, const FallbackEngine& fallback) {
    std::vector<Unit> units;
    for (const auto& part : parts) {
        const auto text = utf8_to_u16(part.at("text").get<std::string>());
        if (!part.contains("readings")) {
            if (std::ranges::any_of(text, [](char16_t c) { return c > 127; })) {
                throw std::runtime_error("non-ASCII literal fixture");
            }
            units.push_back({text, text});
            continue;
        }
        std::istringstream readings(part.at("readings").get<std::string>());
        std::string value;
        std::size_t i = 0;
        while (std::getline(readings, value, '|')) {
            auto reading = utf8_to_u16(value);
            const auto raw = encode(reading, layout);
            if (reading.back() != u'ˊ' && reading.back() != u'ˇ' && reading.back() != u'ˋ' && reading.back() != u'˙') {
                reading.push_back(u' ');
            }
            const auto candidates = fallback.lookup(reading);
            if (i >= text.size() || std::ranges::find(candidates, static_cast<char32_t>(text[i])) == candidates.end()) {
                throw std::runtime_error("target/reading mismatch: " + u16_to_utf8(text) + " / " + value);
            }
            units.push_back({raw, text.substr(i++, 1)});
        }
        if (i != text.size()) throw std::runtime_error("reading count mismatch: " + u16_to_utf8(text));
    }
    return units;
}

class Server {
public:
    Server(const std::string& service, const std::string& model) {
        root_ = std::filesystem::temp_directory_path() / ("typing-usability-" + std::to_string(getpid()));
        if (!std::filesystem::create_directory(root_)) throw std::runtime_error("socket directory already exists");
        ServiceTransportOptions options;
        options.socket_path = root_ / "service.sock";
        options.service_path = service;
        options.model_path = model;
        options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path();
        options.idle_timeout_seconds = 120;
        socket = options.socket_path.string();
        transport_ = std::make_unique<ServiceTransport>(options);
        const auto opened = exchange([&](auto callback) { transport_->open_session(std::move(callback)); });
        const auto session = std::get<protocol::OpenSessionResponse>(opened).session_id;
        const auto start = Clock::now();
        const auto response = exchange([&](auto callback) {
            transport_->predict(session, 1, 1, {}, {{false, u"ㄋㄧˇ", 0}}, std::move(callback));
        });
        if (std::get<protocol::Prediction>(response).candidates.empty()) throw std::runtime_error("warmup failed");
        cold_us = microseconds(start);
        (void)exchange([&](auto callback) { transport_->close_session(session, std::move(callback)); });
    }
    ~Server() {
        transport_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    std::string socket;
    long long cold_us = 0;
private:
    protocol::Message exchange(const std::function<void(ServiceTransport::Callback)>& send) {
        auto promise = std::make_shared<std::promise<protocol::Message>>();
        auto future = promise->get_future();
        send([promise](auto response) { promise->set_value(std::move(response)); });
        if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) throw std::runtime_error("service timeout");
        auto response = future.get();
        if (const auto* error = std::get_if<protocol::Error>(&response)) throw std::runtime_error(error->message);
        return response;
    }
    std::filesystem::path root_;
    std::unique_ptr<ServiceTransport> transport_;
};

class Driver {
public:
    Driver(BopomofoKeyboardLayout layout, const std::string& socket, bool smart = true, bool trace = false,
           std::size_t model_paths = 0, const FallbackEngine* fallback = nullptr, bool production_model = false)
        : trace_(trace) {
        HarnessOptions options;
        options.socket_path = socket;
        options.config.smart_english = smart;
        options.config.smart_model_preview = production_model;
        // Quality fixtures are simulated commits, not user training samples.
        options.on_training_commit = [](const auto&, auto) {};
        options.on_training_discard = [](const auto&) {};
        harness = std::make_unique<Harness>(options);
        harness->set_configs({{"BopomofoKeyboardLayout", std::string(bopomofo_keyboard_layout_name(layout))},
                             {"SelectionKeys", "數字鍵"}, {"CandidatePageSize", "9"}});
        sync();
        if (model_paths != 0) {
            if (!fallback) throw std::runtime_error("missing model-preview reading table");
            preview = std::make_unique<LiveMixedModelPreview>(*harness, *fallback, socket, model_paths);
        }
    }
    void key(const Key& key, std::string label, int cadence_ms = 0) {
        const auto start = Clock::now();
        const bool accepted = harness->key_accepted(key);
        latency.push_back(microseconds(start));
        ++key_count;
        apply_commits();
        if (!accepted) client_key(key.input_key());
        sync();
        if (preview) preview->on_key();
        if (cadence_ms != 0) pause(cadence_ms);
        if (trace_) events.push_back({{"key", std::move(label)}, {"accepted", accepted},
            {"document", u16_to_utf8(document)}, {"preedit", harness->preedit()},
            {"caret", cursor}, {"candidates", harness->candidates()}});
    }
    void key(std::string_view spec, int cadence_ms = 0) { key(Key(spec), std::string(spec), cadence_ms); }
    void type(std::u16string_view raw, int cadence_ms = 0) {
        for (const auto ch : raw) {
            const auto printable = Key(static_cast<char32_t>(ch));
            key(ch >= u'A' && ch <= u'Z' ? printable.with(kShift) : printable,
                u16_to_utf8(std::u16string(1, ch)), cadence_ms);
        }
    }
    void pause(int milliseconds) {
        (void)harness->pump_until([] { return false; }, std::chrono::milliseconds(milliseconds));
        apply_commits();
        sync();
    }
    bool settle() {
        const bool ready = harness->pump_until([&] {
            return !harness->session()->prediction.pending && harness->pending_model_idle() && (!preview || preview->idle());
        },
                                               std::chrono::seconds(10));
        apply_commits();
        sync();
        return ready;
    }
    std::u16string visible() const {
        return document.substr(0, cursor) + utf8_to_u16(harness->preedit()) + document.substr(cursor);
    }
    std::uint64_t requests() const { return harness->session()->prediction.next_request_id - 1; }
    std::unique_ptr<Harness> harness;
    std::unique_ptr<LiveMixedModelPreview> preview;
    std::u16string document;
    std::size_t cursor = 0;
    std::size_t key_count = 0;
    std::vector<long long> latency;
    Json events = Json::array();
private:
    void sync() { harness->set_surrounding(u16_to_utf8(document), cursor, cursor); }
    void apply_commits() {
        const auto commits = harness->commits();
        for (; applied_ < commits.size(); ++applied_) {
            const auto text = utf8_to_u16(commits[applied_]);
            document.insert(cursor, text);
            cursor += text.size();
        }
    }
    void client_key(const InputKey& key) {
        if (key.sym == keysym::BackSpace) {
            if (cursor != 0) document.erase(--cursor, 1);
        } else if (key.sym == keysym::Delete) {
            if (cursor < document.size()) document.erase(cursor, 1);
        } else if (key.sym == keysym::Left) {
            if (cursor != 0) --cursor;
        } else if (key.sym == keysym::Right) {
            if (cursor < document.size()) ++cursor;
        } else if (key.sym == keysym::Return) {
            document.insert(cursor++, 1, u'\n');
        } else if (key.sym >= U' ' && key.sym <= U'~' && !key.has(InputKeyState::Ctrl) && !key.has(InputKeyState::Super)) {
            document.insert(cursor++, 1, static_cast<char16_t>(key.sym));
        }
    }
    std::size_t applied_ = 0;
    bool trace_ = false;
};

Json article(const Json& input, BopomofoKeyboardLayout layout, const std::string& socket,
             const FallbackEngine& fallback, const std::string& mode, int cadence, std::size_t model_paths = 0) {
    const bool smart = mode != "traditional";
    Driver driver(layout, socket, smart, false, model_paths, &fallback, mode == "production");
    std::u16string expected;
    Json snapshots = Json::array();
    std::size_t early_commits = 0;
    std::size_t rewritten_units = 0;
    std::size_t exact_units = 0;
    std::size_t total_units = 0;
    std::u16string previous_complete;
    const auto start = Clock::now();
    for (const auto& clause : input.at("clauses")) {
        const auto units = units_for(clause.at("parts"), layout, fallback);
        for (const auto& unit : units) {
            bool rewritten = false;
            const auto commits_before = driver.harness->commits().size();
            for (const auto ch : unit.raw) {
                driver.type(std::u16string(1, ch), cadence);
                if (!previous_complete.empty() && !driver.visible().starts_with(previous_complete)) rewritten = true;
            }
            early_commits += driver.harness->commits().size() - commits_before;
            expected += unit.expected;
            ++total_units;
            if (driver.visible() == expected) ++exact_units;
            if (rewritten) ++rewritten_units;
            previous_complete = driver.visible();
        }
        const auto punctuation = clause.at("punctuation").get<std::string>();
        const auto before = driver.visible();
        driver.key(punctuation == "，" ? "Control+," : "Control+.", cadence);
        expected += utf8_to_u16(punctuation);
        if (cadence != 0) driver.pause(120);
        snapshots.push_back({{"expected", u16_to_utf8(expected)}, {"before_punctuation", u16_to_utf8(before)},
                              {"after_punctuation", u16_to_utf8(driver.visible())}});
        previous_complete.clear();
        if (mode != "burst-paragraph" && punctuation == "。") {
            driver.key("Return", cadence);
        }
    }
    if (!driver.harness->composition_empty()) driver.key("Return", cadence);
    const bool settled = driver.settle();
    const auto edits = distance(expected, driver.document);
    Json row{{"id", input.at("id")}, {"category", input.at("category")}, {"mode", mode},
        {"layout", bopomofo_keyboard_layout_name(layout)},
        {"cadence_ms", cadence}, {"expected", u16_to_utf8(expected)}, {"actual", u16_to_utf8(driver.document)},
        {"exact", expected == driver.document}, {"edit_distance", edits}, {"expected_characters", expected.size()},
        {"cer", static_cast<double>(edits) / static_cast<double>(expected.size())},
        {"unit_prefix_exact", exact_units}, {"units", total_units}, {"earlier_prefix_rewrites", rewritten_units},
        {"commits_during_printable_typing", early_commits}, {"model_requests", driver.requests()},
        {"key_count", driver.key_count}, {"key_p50_us", percentile(driver.latency, 50)},
        {"key_p95_us", percentile(driver.latency, 95)}, {"key_max_us", percentile(driver.latency, 100)},
        {"wall_ms", microseconds(start) / 1000}, {"settled", settled}, {"checkpoints", snapshots}};
    row["production_pending_requests"] = driver.harness->pending_model_requests();
    if (driver.preview) {
        const auto& stats = driver.preview->stats;
        row["live_model"] = {{"requests", stats.requests}, {"applied", stats.applied}, {"stale", stats.stale},
            {"failures", stats.failures}, {"cache_hits", stats.cache_hits},
            {"early_best_applied", stats.early_best_applied},
            {"unsupported_readings", stats.unsupported_readings},
            {"error_examples", stats.error_examples},
            {"best_p50_us", percentile(stats.best_latency_us, 50)}, {"best_p95_us", percentile(stats.best_latency_us, 95)},
            {"job_p50_us", percentile(stats.job_latency_us, 50)}, {"job_p95_us", percentile(stats.job_latency_us, 95)}};
    }
    return row;
}

Json workflows(const Json& cases, BopomofoKeyboardLayout layout, const std::string& socket,
               const FallbackEngine& fallback, std::size_t model_paths = 0, bool production_model = false) {
    Json rows = Json::array();
    for (const auto& input : cases) {
        if (input.contains("layouts") && std::ranges::none_of(input.at("layouts"), [&](const auto& name) {
            return name.template get<std::string>() == bopomofo_keyboard_layout_name(layout);
        })) continue;
        Driver driver(layout, socket, true, true, model_paths, &fallback, production_model);
        const auto initial_context = utf8_to_u16(input.value("initial_context", ""));
        driver.document = initial_context;
        driver.cursor = initial_context.size();
        driver.pause(0);
        std::string error;
        try {
            for (const auto& step : input.at("steps")) {
                if (step.contains("type")) driver.type(utf8_to_u16(step.at("type").get<std::string>()));
                if (step.contains("zh")) {
                    const Json parts = Json::array({{{"text", step.at("zh")}, {"readings", step.at("readings")}}});
                    for (const auto& unit : units_for(parts, layout, fallback)) driver.type(unit.raw);
                }
                if (step.contains("key")) {
                    for (int i = 0; i < step.value("repeat", 1); ++i) driver.key(step.at("key").get<std::string>());
                }
                if (step.contains("tone")) driver.type(std::u16string(1, encode(u"ㄧˇ", layout).back()));
                if (step.contains("choose")) {
                    const auto target = step.at("choose").get<std::string>();
                    bool chosen = false;
                    for (int page = 0; page < 8; ++page) {
                        const auto candidates = driver.harness->candidates();
                        const auto at = std::ranges::find(candidates, target);
                        if (at != candidates.end()) {
                            driver.key(Key(static_cast<char32_t>(U'1' + std::distance(candidates.begin(), at))), "choose:" + target);
                            chosen = true;
                            break;
                        }
                        driver.key("Page_Down");
                        if (driver.harness->candidates() == candidates) break;
                    }
                    if (!chosen) throw std::runtime_error("manual target missing");
                }
                if (step.contains("caps_type")) {
                    for (const auto ch : utf8_to_u16(step.at("caps_type").get<std::string>())) {
                        driver.key(Key(static_cast<char32_t>(ch)).with(kCapsLock), "CapsLock:" + u16_to_utf8(std::u16string(1, ch)));
                    }
                }
                if (step.contains("focus_out")) { driver.harness->focus_out(); if (driver.preview) driver.preview->on_key(); driver.pause(0); }
                if (step.contains("activate")) driver.harness->activate();
                if (step.contains("pause_ms")) driver.pause(step.at("pause_ms").get<int>());
                if (step.contains("sensitive")) driver.harness->host().set_sensitive(step.at("sensitive").get<bool>());
                if (step.contains("config")) {
                    for (const auto& [name, value] : step.at("config").items()) driver.harness->set_config(name, value.get<std::string>());
                    if (driver.preview) driver.preview->on_key();
                }
                if (input.value("settle_each_step", true) && !driver.settle()) throw std::runtime_error("prediction did not settle");
                if (step.contains("expect_preedit") && driver.harness->preedit() != step.at("expect_preedit").get<std::string>()) {
                    throw std::runtime_error("editing checkpoint: expected " + step.at("expect_preedit").get<std::string>() +
                                             ", got " + driver.harness->preedit());
                }
            }
            if (!driver.settle()) throw std::runtime_error("final prediction did not settle");
        } catch (const std::exception& failure) { error = failure.what(); }
        const auto expected = u16_to_utf8(initial_context) + input.at("expected").get<std::string>();
        Json row{{"id", input.at("id")}, {"layout", bopomofo_keyboard_layout_name(layout)},
            {"expected", expected}, {"actual", u16_to_utf8(driver.document)}, {"preedit", driver.harness->preedit()},
            {"passed", error.empty() && u16_to_utf8(driver.document) == expected && driver.harness->composition_empty()},
            {"error", error}, {"model_paths", model_paths}, {"model_requests", driver.requests()}, {"key_count", driver.key_count}, {"trace", driver.events}};
        row["production_model"] = production_model;
        row["production_pending_requests"] = driver.harness->pending_model_requests();
        if (driver.preview) row["live_model"] = {{"requests", driver.preview->stats.requests},
            {"applied", driver.preview->stats.applied}, {"stale", driver.preview->stats.stale}, {"failures", driver.preview->stats.failures}};
        rows.push_back(std::move(row));
    }
    return rows;
}

char16_t adjacent(char16_t original) {
    constexpr std::u16string_view rows[] = {u"1234567890-", u"qwertyuiop", u"asdfghjkl;", u"zxcvbnm,./"};
    for (const auto row : rows) {
        const auto at = row.find(original);
        if (at != std::u16string_view::npos) return row[at + 1 < row.size() ? at + 1 : at - 1];
    }
    return u'x';
}

Json repaired_article(const Json& input, BopomofoKeyboardLayout layout, const std::string& socket,
                      const FallbackEngine& fallback, int cadence) {
    Driver driver(layout, socket);
    std::u16string expected;
    Json repairs = Json::array();
    std::size_t clause_index = 0;
    for (const auto& clause : input.at("clauses")) {
        const auto units = units_for(clause.at("parts"), layout, fallback);
        std::u16string raw;
        for (const auto& unit : units) { raw += unit.raw; expected += unit.expected; }
        if (clause_index % 2 == 0) {
            const std::size_t p = units.front().raw.size() - 1;
            const auto fault = (clause_index / 2) % 4;
            const std::size_t removed = fault == 3 ? 2 : 1;
            std::u16string replacement;
            if (fault == 0) replacement = std::u16string(1, adjacent(raw[p]));
            if (fault == 1) replacement = std::u16string(2, raw[p]);
            if (fault == 3) replacement = std::u16string{raw[p + 1], raw[p]};
            const auto detected = std::min(raw.size(), p + removed + 6);
            const auto before_keys = driver.key_count;
            driver.type(raw.substr(0, p) + replacement + raw.substr(p + removed, detected - p - removed), cadence);
            const auto wrong_preview = driver.visible();
            const auto erase = replacement.size() + detected - p - removed;
            // This experiment repairs an exact raw offset, not a count of
            // displayed Chinese characters. Use the explicit raw-key undo.
            for (std::size_t i = 0; i < erase; ++i) driver.key("Shift+BackSpace", cadence);
            driver.type(raw.substr(p, detected - p), cadence);
            const bool raw_restored = driver.harness->session()->pending_token.raw == raw.substr(0, detected);
            repairs.push_back({{"clause", clause_index}, {"fault", fault}, {"raw_restored", raw_restored},
                {"wrong_preview", u16_to_utf8(wrong_preview)}, {"repaired_preview", u16_to_utf8(driver.visible())},
                {"backspaces", erase}, {"repair_key", "Shift+BackSpace"},
                {"extra_keys", driver.key_count - before_keys - detected}});
            driver.type(raw.substr(detected), cadence);
        } else {
            driver.type(raw, cadence);
        }
        const auto punctuation = clause.at("punctuation").get<std::string>();
        driver.key(punctuation == "，" ? "Control+," : "Control+.", cadence);
        expected += utf8_to_u16(punctuation);
        driver.pause(120);
        if (punctuation == "。") driver.key("Return", cadence);
        ++clause_index;
    }
    if (!driver.harness->composition_empty()) driver.key("Return", cadence);
    if (!driver.settle()) throw std::runtime_error("repaired article prediction timeout");
    const auto edits = distance(expected, driver.document);
    return {{"id", input.at("id")}, {"category", input.at("category")}, {"mode", "normal-with-typos"},
        {"layout", bopomofo_keyboard_layout_name(layout)}, {"cadence_ms", cadence},
        {"expected", u16_to_utf8(expected)}, {"actual", u16_to_utf8(driver.document)}, {"exact", expected == driver.document},
        {"edit_distance", edits}, {"expected_characters", expected.size()},
        {"cer", static_cast<double>(edits) / static_cast<double>(expected.size())},
        {"model_requests", driver.requests()}, {"key_count", driver.key_count}, {"repairs", repairs},
        {"key_p95_us", percentile(driver.latency, 95)}};
}

Json typos(const Json& articles, BopomofoKeyboardLayout layout, const FallbackEngine& fallback,
           const std::string& socket = {}, std::size_t model_paths = 0, bool production_model = false) {
    // These stress reversible pending input at burst speed. Their
    // reference is the clean engine output, not an assumed correct sentence.
    Json rows = Json::array();
    std::vector<std::pair<std::string, std::vector<Unit>>> seeds;
    for (const auto& input : articles) {
        std::size_t included = 0;
        for (const auto& clause : input.at("clauses")) {
            auto units = units_for(clause.at("parts"), layout, fallback);
            std::u16string raw;
            for (const auto& unit : units) raw += unit.raw;
            if (std::ranges::any_of(raw, [](char16_t c) { return c >= u'A' && c <= u'Z'; })) continue;
            seeds.emplace_back(input.at("id").get<std::string>() + ":" + std::to_string(included), std::move(units));
            if (++included == 2) break;
        }
    }
    seeds.push_back({"english", {{u"hello world", u"hello world"}}});
    seeds.push_back({"english-hsu-tone-letters", {{u"added fixes", u"added fixes"}}});
    for (const auto& [id, units] : seeds) {
        std::u16string raw;
        std::u16string expected;
        std::set<std::size_t> positions{0, 1};
        for (const auto& unit : units) {
            raw += unit.raw;
            expected += unit.expected;
            if (positions.size() < 4) positions.insert(raw.size() - 1);
        }
        positions.insert(raw.size() / 2);
        positions.insert(raw.size() - 1);
        Driver clean(layout, socket, true, false, model_paths, &fallback, production_model);
        clean.type(raw);
        if (!clean.settle()) throw std::runtime_error("clean typo reference did not settle");
        const auto clean_preview = clean.visible();
        clean.key("Return");
        const auto clean_commit = clean.document;
        for (const auto p : positions) {
            for (const std::string kind : {"substitute", "duplicate", "omit", "transpose", "extra-space"}) {
                const std::size_t removed = kind == "extra-space" ? 0 : kind == "transpose" ? 2 : 1;
                if (p + removed > raw.size()) continue;
                std::u16string replacement;
                if (kind == "substitute") replacement = std::u16string(1, adjacent(raw[p]));
                if (kind == "duplicate") replacement = std::u16string(2, raw[p]);
                if (kind == "transpose") replacement = std::u16string{raw[p + 1], raw[p]};
                if (kind == "extra-space") replacement = u" ";
                if (replacement == raw.substr(p, removed)) continue;
                const auto damaged = raw.substr(0, p) + replacement + raw.substr(p + removed);
                Driver wrong(layout, socket, true, false, model_paths, &fallback, production_model);
                wrong.type(damaged);
                if (!wrong.settle()) throw std::runtime_error("damaged typo input did not settle");
                wrong.key("Return");
                Json row{{"seed", id}, {"layout", bopomofo_keyboard_layout_name(layout)},
                    {"kind", kind}, {"position", p}, {"raw", u16_to_utf8(raw)}, {"damaged_raw", u16_to_utf8(damaged)},
                    {"model_paths", model_paths},
                    {"production_model", production_model},
                    {"expected", u16_to_utf8(expected)}, {"clean", u16_to_utf8(clean_commit)},
                    {"uncorrected", u16_to_utf8(wrong.document)}, {"uncorrected_exact", wrong.document == expected},
                    {"uncorrected_matches_clean", wrong.document == clean_commit}};
                for (const std::size_t lag : {0U, 6U}) {
                    const auto detected = std::min(raw.size(), p + removed + lag);
                    Driver corrected(layout, socket, true, true, model_paths, &fallback, production_model);
                    corrected.type(raw.substr(0, p) + replacement + raw.substr(p + removed, detected - p - removed));
                    const auto damaged_preview = corrected.visible();
                    const auto erase = replacement.size() + detected - p - removed;
                    for (std::size_t i = 0; i < erase; ++i) corrected.key("Shift+BackSpace");
                    corrected.type(raw.substr(p));
                    if (!corrected.settle()) throw std::runtime_error("typo correction did not settle");
                    const bool preview_restored = corrected.visible() == clean_preview;
                    corrected.key("Return");
                    const bool restored = preview_restored && corrected.document == clean_commit &&
                                          corrected.harness->composition_empty();
                    const auto name = lag == 0 ? "immediate" : "delayed";
                    row[name] = {{"restored_clean", restored}, {"exact_target", corrected.document == expected},
                        {"actual", u16_to_utf8(corrected.document)}, {"damaged_preview", u16_to_utf8(damaged_preview)},
                        {"backspaces", erase}, {"repair_key", "Shift+BackSpace"},
                        {"extra_keys", corrected.key_count - clean.key_count}};
                    if (corrected.preview) row[name]["live_model"] = {{"requests", corrected.preview->stats.requests},
                        {"applied", corrected.preview->stats.applied}, {"stale", corrected.preview->stats.stale},
                        {"failures", corrected.preview->stats.failures}};
                    if (!restored) row[name]["trace"] = corrected.events;
                }
                rows.push_back(std::move(row));
            }
        }
    }
    return rows;
}

Json long_composition(const Json& articles, BopomofoKeyboardLayout layout, const FallbackEngine& fallback) {
    std::u16string seed;
    for (const auto& input : articles) {
        if (input.at("category") != "chinese") continue;
        for (const auto& clause : input.at("clauses")) {
            for (const auto& unit : units_for(clause.at("parts"), layout, fallback)) seed += unit.raw;
        }
    }
    Driver driver(layout, {});
    Json rows = Json::array();
    std::size_t written = 0;
    for (const std::size_t length : {128U, 256U, 512U, 768U}) {
        std::vector<long long> window;
        while (written < length) {
            driver.type(std::u16string(1, seed[written++ % seed.size()]));
            window.push_back(driver.latency.back());
        }
        rows.push_back({{"layout", bopomofo_keyboard_layout_name(layout)},
            {"raw_keys", length}, {"p50_us", percentile(window, 50)}, {"p95_us", percentile(window, 95)},
            {"max_us", percentile(window, 100)}, {"commits", driver.harness->commits().size()}});
    }
    return rows;
}

Json clause_recovery(const Json& articles, BopomofoKeyboardLayout layout, const std::string& socket,
                     const FallbackEngine& fallback, std::size_t model_paths = 0, bool production_model = false) {
    Json rows = Json::array();
    for (const auto& input : articles) {
        std::u16string context;
        std::size_t index = 0;
        for (const auto& clause : input.at("clauses")) {
            Driver driver(layout, socket, true, false, model_paths, &fallback, production_model);
            // An optimistic reference: preceding clauses are already correct.
            // This is independent from the full document's observed output.
            driver.document = context;
            driver.cursor = context.size();
            driver.pause(0);
            auto target = context;
            for (const auto& unit : units_for(clause.at("parts"), layout, fallback)) {
                driver.type(unit.raw);
                target += unit.expected;
            }
            if (!driver.settle()) throw std::runtime_error("clause prediction timeout");
            const auto before = driver.visible();
            bool found = before == target;
            Json candidates = Json::array();
            const auto baseline_keys = driver.key_count;
            if (!found) {
                driver.key("Down");
                const auto displayed = driver.harness->candidates();
                for (std::size_t candidate = 0; candidate < displayed.size(); ++candidate) {
                    auto tentative = driver.document.substr(0, driver.cursor);
                    const auto* session = driver.harness->session();
                    if (session->mixed_decision.active()) {
                        tentative += session->buffer.commit_text();
                        tentative += utf8_to_u16(displayed[candidate]);
                    } else {
                        const auto at = session->buffer.candidate_target(CandidateTarget::BeforeCursor);
                        const auto& segments = session->buffer.segments();
                        for (std::size_t i = 0; i < segments.size(); ++i) {
                            tentative += at && i == *at ? utf8_to_u16(displayed[candidate]) : segments[i].rendered_text();
                        }
                    }
                    tentative += driver.document.substr(driver.cursor);
                    candidates.push_back(u16_to_utf8(tentative));
                    if (tentative == target) {
                        driver.key(Key(static_cast<char32_t>(U'1' + candidate)), "candidate-selection");
                        if (!driver.settle()) throw std::runtime_error("candidate prediction timeout");
                        found = true;
                        break;
                    }
                }
                if (!found && driver.harness->has_candidates()) driver.key("Escape");
            }
            driver.key("Return");
            rows.push_back({{"id", input.at("id").get<std::string>() + ":" + std::to_string(index++)},
                {"layout", bopomofo_keyboard_layout_name(layout)},
                {"model_paths", model_paths},
                {"production_model", production_model},
                {"expected", u16_to_utf8(target)}, {"before", u16_to_utf8(before)},
                {"already_correct", before == target}, {"candidate_target_found", found},
                {"repaired", found && driver.document == target}, {"actual", u16_to_utf8(driver.document)},
                {"extra_keys_excluding_return", driver.key_count - baseline_keys - 1}, {"candidates_examined", candidates}});
            context = target + utf8_to_u16(clause.at("punctuation").get<std::string>());
        }
    }
    return rows;
}

bool same_structure(const MixedPath& left, const MixedPath& right) {
    if (left.segments.size() != right.segments.size()) return false;
    for (std::size_t i = 0; i < left.segments.size(); ++i) {
        const auto& a = left.segments[i];
        const auto& b = right.segments[i];
        if (a.kind != b.kind || a.begin != b.begin || a.end != b.end || a.reading != b.reading ||
            a.raw != b.raw || a.consumed_boundary != b.consumed_boundary) return false;
    }
    return true;
}

struct BoundedRefinement {
    std::vector<MixedPath> paths;
    std::size_t requests = 0;
    std::size_t failures = 0;
    std::optional<std::size_t> source_row;
};

std::u16string segment_text(const MixedSegment& segment) {
    if (segment.consumed_boundary) return {};
    if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
        return utf8_to_u16(char32_to_utf8(segment.candidates.front()));
    }
    return segment.raw;
}

// Offline explicit-refresh primitive. No expected label is available here.
// This is a separate hypothetical operation, not normal candidate selection.
BoundedRefinement refine_first_visible(const MixedDecodeResult& result,
                                      const std::vector<MixedCandidateEntry>& entries,
                                      const std::u16string& context, const std::string& socket,
                                      const FallbackEngine& fallback, int context_length) {
    BoundedRefinement refined;
    std::ifstream token_file(std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path() / "tokens/bpmf.json");
    if (!token_file) throw std::runtime_error("missing bounded-refinement token table");
    const auto tokens = Json::parse(token_file);
    const auto supported = [&](const MixedSegment& segment) {
        return segment.kind == MixedSegmentKind::Bopomofo &&
               tokens.contains("<" + u16_to_utf8(segment.reading) + ">");
    };
    const auto source = std::ranges::find_if(entries, [&](const auto& entry) {
        return entry.path_index != 0 && entry.char_index == 0 &&
               std::ranges::any_of(result.paths.at(entry.path_index).segments, supported);
    });
    if (source == entries.end()) return refined;
    refined.source_row = static_cast<std::size_t>(std::distance(entries.begin(), source));
    auto path = result.paths.at(source->path_index);
    const auto first = std::ranges::find_if(path.segments, supported);
    auto last = first;
    while (last != path.segments.end() && (supported(*last) || last->consumed_boundary)) ++last;
    std::u16string prefix = context;
    for (auto it = path.segments.begin(); it != first; ++it) prefix += segment_text(*it);
    std::vector<protocol::PaddingEntry> padding;
    for (auto it = first; it != last; ++it) {
        if (!it->consumed_boundary) padding.push_back({false, it->reading, 0});
    }
    const auto reserved = 2 + padding.size() * 2;
    const auto limit = static_cast<std::size_t>(std::max(0, context_length));
    prefix = utf16_tail(prefix, limit > reserved ? limit - reserved : 0);
    ServiceTransportOptions options;
    options.socket_path = socket;
    options.auto_start = false;
    ServiceTransport transport(options);
    const auto exchange = [](const std::function<void(ServiceTransport::Callback)>& send) {
        auto promise = std::make_shared<std::promise<protocol::Message>>();
        auto future = promise->get_future();
        send([promise](auto response) { promise->set_value(std::move(response)); });
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) throw std::runtime_error("bounded refresh timeout");
        return future.get();
    };
    const auto opened = exchange([&](auto callback) { transport.open_session(std::move(callback)); });
    const auto* session = std::get_if<protocol::OpenSessionResponse>(&opened);
    if (!session) { refined.failures = 1; return refined; }
    ++refined.requests;
    const auto response = exchange([&](auto callback) {
        transport.predict(session->session_id, 1, 1, prefix, padding, std::move(callback));
    });
    const auto* prediction = std::get_if<protocol::Prediction>(&response);
    const bool valid = prediction && prediction->session_id == session->session_id && prediction->request_id == 1 &&
                       prediction->buffer_revision == 1 && prediction->candidates.size() == padding.size();
    if (valid) {
        std::size_t index = 0;
        for (auto it = first; it != last; ++it) {
            if (it->consumed_boundary) continue;
            const auto allowed = fallback.lookup(it->reading);
            const auto& candidates = prediction->candidates[index++];
            if (candidates.empty() || std::ranges::any_of(candidates, [&](auto value) {
                    return std::ranges::find(allowed, value) == allowed.end();
                })) { refined.failures = 1; break; }
            it->candidates = candidates;
            for (const auto value : allowed) {
                if (std::ranges::find(it->candidates, value) == it->candidates.end()) it->candidates.push_back(value);
            }
        }
        if (refined.failures == 0) {
            path.rendered.clear();
            for (const auto& segment : path.segments) path.rendered += segment_text(segment);
            refined.paths.push_back(std::move(path));
        }
    } else {
        refined.failures = 1;
    }
    const auto closed = exchange([&](auto callback) { transport.close_session(session->session_id, std::move(callback)); });
    if (!std::holds_alternative<protocol::CloseSessionResponse>(closed)) throw std::runtime_error("bounded refresh close failed");
    return refined;
}

// Counterfactual, end-of-clause candidate enrichment. The real production
// preview is the control; model work happens only in a shadow harness. The
// insertion rule does not consult the expected text.
Json pause_candidates(const Json& articles, BopomofoKeyboardLayout layout, const std::string& socket,
                      const FallbackEngine& fallback, bool explicit_one = false) {
    Json rows = Json::array();
    const MixedInputDecoder decoder([&](auto reading) { return fallback.lookup(reading); },
                                    [&](auto word) { return fallback.latin_frequency(word); });
    for (const auto& input : articles) {
        std::u16string context;
        std::size_t index = 0;
        for (const auto& clause : input.at("clauses")) {
            Driver driver(layout, socket, true, false, 0, &fallback, true);
            driver.document = context;
            driver.cursor = context.size();
            driver.pause(0);
            std::u16string raw;
            std::u16string expected;
            for (const auto& unit : units_for(clause.at("parts"), layout, fallback)) {
                raw += unit.raw;
                expected += input.value("ascii_intent", false) ? unit.raw : unit.expected;
            }
            driver.type(raw);
            if (!driver.settle()) throw std::runtime_error("pause control did not settle");
            const auto before = driver.visible();
            auto* session = driver.harness->session();
            Json row{{"id", input.at("id").get<std::string>() + ":" + std::to_string(index++)},
                     {"layout", bopomofo_keyboard_layout_name(layout)},
                     {"raw", u16_to_utf8(raw)}, {"context", u16_to_utf8(context)},
                     {"root_group", input.value("root_group", input.at("id").get<std::string>())},
                     {"category", input.at("category")},
                     {"ascii_intent", input.value("ascii_intent", false)},
                     {"expected", u16_to_utf8(context + expected)}, {"before", u16_to_utf8(before)},
                     {"applicable", session->mixed_decision.active()}, {"extra_requests", 0},
                     {"explicit_request", explicit_one && input.value("explicit_request", true)},
                     {"automatic_preview_exact", before == context + expected},
                     {"preview_preserved", true}, {"raw_in_homepage", true}, {"commit_matches_selected", true},
                     {"added_paths", 0}, {"model_failures", 0}, {"wait_us", 0}};
            if (session->mixed_decision.active()) {
                const auto original = session->mixed_decision;
                const auto old_entries = decoder.expand_candidates(original.result, 9, original.preview_path);
                if (old_entries.size() < 2) {
                    // Engine deliberately has no panel for an unambiguous
                    // single path. Such a tail needs no candidate enrichment.
                    row["applicable"] = false;
                    row["skip_reason"] = "single-path-no-panel";
                    row["baseline_target_in_homepage"] = before == context + expected;
                    row["enriched_target_in_homepage"] = before == context + expected;
                    rows.push_back(std::move(row));
                    context += expected + utf8_to_u16(clause.at("punctuation").get<std::string>());
                    continue;
                }
                std::vector<MixedPath> refined;
                auto start = Clock::now();
                if (explicit_one) {
                    if (input.value("explicit_request", true)) {
                        const auto work = refine_first_visible(original.result, old_entries,
                            driver.document.substr(0, driver.cursor) + session->buffer.rendered_prefix_before_caret(),
                            socket, fallback, driver.harness->config().context_length);
                        refined = work.paths;
                        row["extra_requests"] = work.requests;
                        row["model_failures"] = work.failures;
                        if (work.source_row) row["requested_homepage_row"] = *work.source_row + 1;
                    }
                } else {
                    Driver shadow(layout, socket);
                    shadow.document = context;
                    shadow.cursor = context.size();
                    shadow.pause(0);
                    shadow.type(raw);
                    if (!shadow.settle()) throw std::runtime_error("shadow control did not settle");
                    // Share a captured candidate snapshot, not callbacks,
                    // transport state or the production session itself.
                    shadow.harness->session()->mixed_decision = original;
                    start = Clock::now();
                    shadow.preview = std::make_unique<LiveMixedModelPreview>(*shadow.harness, fallback, socket, 4);
                    shadow.preview->on_key();
                    if (!shadow.settle()) throw std::runtime_error("shadow candidate model did not settle");
                    refined = shadow.harness->session()->mixed_decision.result.paths;
                    row["extra_requests"] = shadow.preview->stats.requests;
                    row["model_failures"] = shadow.preview->stats.failures;
                }
                row["wait_us"] = microseconds(start);
                std::vector<MixedPath> merged;
                std::set<std::u16string> originals;
                for (const auto& path : original.result.paths) originals.insert(path.rendered);
                auto seen = originals;
                std::size_t preview = 0;
                for (std::size_t source = 0; source < original.result.paths.size(); ++source) {
                    const auto& path = original.result.paths[source];
                    if (source == original.preview_path) preview = merged.size();
                    merged.push_back(path);
                    if (source == 0) continue;  // exact raw is never modeled
                    for (const auto& candidate : refined) {
                        if (same_structure(path, candidate) && seen.insert(candidate.rendered).second) {
                            merged.push_back(candidate);
                            row["added_paths"] = row.at("added_paths").get<std::size_t>() + 1;
                        }
                    }
                }
                session->mixed_decision.result.paths = std::move(merged);
                session->mixed_decision.preview_path = preview;
                session->mixed_decision.result.best_path = preview;
                const auto new_entries = decoder.expand_candidates(session->mixed_decision.result, 9, preview);
                const auto contains = [](const auto& entries, std::u16string_view text) {
                    return std::ranges::any_of(entries, [&](const auto& entry) { return entry.text == text; });
                };
                // Shifted capitals/symbols may already have committed part of
                // this clause. Compare the full document, including these
                // observed commits, rather than assuming it is still context.
                const auto buffered = session->buffer.commit_text();
                const auto fixed_prefix = driver.document.substr(0, driver.cursor) + buffered;
                const auto fixed_suffix = driver.document.substr(driver.cursor);
                const auto reachable = [&](const auto& entries) {
                    return before == context + expected || std::ranges::any_of(entries, [&](const auto& entry) {
                        return fixed_prefix + entry.text + fixed_suffix == context + expected;
                    });
                };
                row["baseline_target_in_homepage"] = reachable(old_entries);
                row["enriched_target_in_homepage"] = reachable(new_entries);
                row["raw_in_homepage"] = contains(new_entries, original.result.raw);
                row["preview_preserved"] = session->mixed_decision.result.paths[preview].rendered ==
                                           original.result.paths[original.preview_path].rendered;
                Json old_text = Json::array();
                Json new_text = Json::array();
                for (const auto& entry : old_entries) old_text.push_back(u16_to_utf8(entry.text));
                for (const auto& entry : new_entries) new_text.push_back(u16_to_utf8(entry.text));
                row["baseline_homepage"] = old_text;
                row["enriched_homepage"] = new_text;
                driver.key("Down");
                const auto panel = driver.harness->candidates();
                if (panel != new_text.get<std::vector<std::string>>()) {
                    throw std::runtime_error("counterfactual panel differs from engine at " + row.at("id").get<std::string>() +
                        ": expected=" + new_text.dump() + ", displayed=" + Json(panel).dump());
                }
                const auto at = std::ranges::find_if(panel, [&](const auto& value) {
                    return fixed_prefix + utf8_to_u16(value) + fixed_suffix == context + expected;
                });
                // Even if the labeled target is absent, select the raw row to
                // verify the candidate mapping and exact commit invariant.
                const auto selected = at == panel.end() ? std::ranges::find(panel, u16_to_utf8(original.result.raw)) : at;
                if (selected == panel.end()) throw std::runtime_error("raw candidate disappeared");
                const auto selected_text = *selected;
                driver.key(Key(static_cast<char32_t>(U'1' + std::distance(panel.begin(), selected))), "pause-candidate-selection");
                const auto shown = driver.visible();
                driver.key("Return");
                if (!driver.settle()) throw std::runtime_error("pause candidate commit did not settle");
                row["selected"] = selected_text;
                row["actual"] = u16_to_utf8(driver.document);
                row["commit_matches_selected"] = driver.document == fixed_prefix + utf8_to_u16(selected_text) + fixed_suffix &&
                                                 shown == driver.document;
            } else {
                row["baseline_target_in_homepage"] = before == context + expected;
                row["enriched_target_in_homepage"] = before == context + expected;
            }
            rows.push_back(std::move(row));
            context += expected + utf8_to_u16(clause.at("punctuation").get<std::string>());
        }
    }
    return rows;
}

Json punctuation_timing(const Json& articles, BopomofoKeyboardLayout layout, const std::string& socket,
                        const FallbackEngine& fallback, int cadence) {
    const auto units = units_for(articles.at(0).at("clauses").at(0).at("parts"), layout, fallback);
    Json rows = Json::array();
    for (const bool smart : {true, false}) {
        for (const bool wait_before : {false, true}) {
            Driver driver(layout, socket, smart);
            std::u16string expected;
            for (const auto& unit : units) { driver.type(unit.raw, cadence); expected += unit.expected; }
            if (wait_before && !driver.settle()) throw std::runtime_error("punctuation prediction timeout");
            const auto before = driver.visible();
            const bool pending = driver.harness->session()->prediction.pending;
            driver.key("Control+,");
            driver.pause(250);
            if (!driver.settle()) throw std::runtime_error("punctuation prediction timeout");
            const auto after = driver.visible();
            rows.push_back({{"layout", bopomofo_keyboard_layout_name(layout)},
                {"smart", smart}, {"wait_before_punctuation", wait_before}, {"pending_before_punctuation", pending},
                {"before", u16_to_utf8(before)}, {"after", u16_to_utf8(after)},
                {"before_edits", distance(expected, before)}, {"after_edits", distance(expected + u"，", after)},
                {"requests", driver.requests()}});
        }
    }
    return rows;
}

Json preview_timing(const Json& cases, BopomofoKeyboardLayout layout, const std::string& socket,
                    const FallbackEngine& fallback, bool production_model = false) {
    Json rows = Json::array();
    for (const auto& input : cases) {
        const auto units = units_for(input.at("parts"), layout, fallback);
        for (const std::size_t paths : {0U, 1U}) {
            for (const int cadence : {0, 20, 60}) {
                for (const int wait : {0, 120}) {
                    Driver driver(layout, socket, true, false, production_model ? 0 : paths, &fallback,
                                  production_model && paths == 1);
                    auto expected = utf8_to_u16(input.value("initial_context", ""));
                    driver.document = expected;
                    driver.cursor = expected.size();
                    driver.pause(0);
                    for (const auto& unit : units) { driver.type(unit.raw, cadence); expected += unit.expected; }
                    if (wait != 0) driver.pause(wait);
                    const auto shown = driver.visible();
                    driver.key("Return");
                    if (!driver.settle()) throw std::runtime_error("preview timing did not settle");
                    Json row{{"id", input.at("id")}, {"layout", bopomofo_keyboard_layout_name(layout)},
                        {"model_paths", paths}, {"cadence_ms", cadence}, {"wait_before_return_ms", wait},
                        {"expected", u16_to_utf8(expected)}, {"shown_before_return", u16_to_utf8(shown)},
                        {"actual", u16_to_utf8(driver.document)}, {"exact", expected == driver.document},
                        {"commit_matches_preview", shown == driver.document}, {"key_count", driver.key_count}};
                    row["production_model"] = production_model && paths == 1;
                    if (driver.preview) row["live_model"] = {{"requests", driver.preview->stats.requests},
                        {"applied", driver.preview->stats.applied}, {"stale", driver.preview->stats.stale},
                        {"failures", driver.preview->stats.failures}};
                    rows.push_back(std::move(row));
                }
            }
        }
    }
    return rows;
}

Json load_cases(const std::filesystem::path& path, unsigned depth = 0) {
    if (depth >= 16) throw std::runtime_error("fixture inheritance is cyclic or too deep");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read cases: " + path.string());
    auto local = Json::parse(input);
    if (!local.contains("extends")) return local;
    auto inherited = load_cases(path.parent_path() / local.at("extends").get<std::string>(), depth + 1);
    local.erase("extends");
    for (const auto& [key, value] : local.items()) {
        if (key == "articles" || key == "edit_workflows" || key == "timing_cases") {
            if (!inherited.contains(key)) inherited[key] = Json::array();
            for (const auto& item : value) inherited[key].push_back(item);
        } else {
            inherited[key] = value;
        }
    }
    return inherited;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr, "usage: typing_usability_probe SERVICE MODEL CASES_JSON REPORT_JSON [CADENCE_MS=60] "
                             "[focused|repaired-only|model-comparison|model-candidates|model-timing|model-typos|"
                              "production-comparison|production-layouts|production-timing|production-typos|pause-candidates|explicit-one]\n");
        return 1;
    }
    try {
        const std::vector<std::string> args(argv, argv + argc);
        const int cadence = argc >= 6 ? std::stoi(args[5]) : 60;
        const bool focused = argc == 7 && args[6] == "focused";
        const bool repaired_only = argc == 7 && args[6] == "repaired-only";
        const bool candidates_only = argc == 7 && args[6] == "model-candidates";
        const bool model_comparison = argc == 7 && (args[6] == "model-comparison" || candidates_only);
        const bool model_timing = argc == 7 && args[6] == "model-timing";
        const bool model_typos = argc == 7 && args[6] == "model-typos";
        const bool production_layouts = argc == 7 && args[6] == "production-layouts";
        const bool production_comparison = production_layouts || (argc == 7 && args[6] == "production-comparison");
        const bool production_timing = argc == 7 && args[6] == "production-timing";
        const bool production_typos = argc == 7 && args[6] == "production-typos";
        const bool pause_candidate_experiment = argc == 7 && args[6] == "pause-candidates";
        const bool explicit_one = argc == 7 && args[6] == "explicit-one";
        if (argc == 7 && !focused && !repaired_only && !model_comparison && !model_timing && !model_typos &&
            !production_comparison && !production_timing && !production_typos && !pause_candidate_experiment && !explicit_one) throw std::runtime_error("unknown experiment mode");
        if (cadence < 0 || cadence > 1000) throw std::runtime_error("invalid cadence");
        const auto cases = load_cases(args[3]);
        const FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
        auto layouts = std::vector{BopomofoKeyboardLayout::Standard, BopomofoKeyboardLayout::Hsu};
        if (production_layouts) {
            layouts.insert(layouts.end(), {BopomofoKeyboardLayout::Ibm, BopomofoKeyboardLayout::Et,
                BopomofoKeyboardLayout::GinYieh, BopomofoKeyboardLayout::Et26, BopomofoKeyboardLayout::DachenCp26});
        }
        for (const auto layout : layouts) {
            for (const auto& item : cases.at("articles")) {
                for (const auto& clause : item.at("clauses")) (void)units_for(clause.at("parts"), layout, fallback);
            }
        }
        Server server(args[1], args[2]);
        Json report{{"service", args[1]}, {"model", args[2]}, {"cold_us", server.cold_us},
            {"fixtures", args[3]},
            {"experiment", argc == 7 ? args[6] : "usability"},
            {"preview_policy", production_comparison || production_timing || production_typos ?
                                    "production-single-path-v1" : explicit_one ? "explicit-first-visible-one-run-v1" : pause_candidate_experiment ?
                                    "shadow-pause-four-append-originals-v1" : "best-first-token-aware-v3"},
            {"articles", Json::array()}, {"workflows", Json::array()}, {"typos", Json::array()},
            {"long_composition", Json::array()}, {"clause_recovery", Json::array()}, {"punctuation_timing", Json::array()},
            {"preview_timing", Json::array()}};
        for (const auto layout : layouts) {
            if (pause_candidate_experiment || explicit_one) {
                for (auto& row : pause_candidates(cases.at("articles"), layout, server.socket, fallback, explicit_one)) report["clause_recovery"].push_back(std::move(row));
                continue;
            }
            if (production_comparison) {
                for (const bool enabled : {false, true}) {
                    for (const auto& item : cases.at("articles")) {
                        auto row = article(item, layout, server.socket, fallback, enabled ? "production" : "baseline", cadence);
                        std::printf("%s %s %s CER=%.3f\n", row.at("layout").get<std::string>().c_str(),
                            item.at("id").get<std::string>().c_str(), enabled ? "production" : "baseline", row.at("cer").get<double>());
                        std::fflush(stdout);
                        report["articles"].push_back(std::move(row));
                    }
                    for (auto& row : clause_recovery(cases.at("articles"), layout, server.socket, fallback, 0, enabled)) report["clause_recovery"].push_back(std::move(row));
                    for (auto& row : workflows(cases.at("edit_workflows"), layout, server.socket, fallback, 0, enabled)) report["workflows"].push_back(std::move(row));
                }
                continue;
            }
            if (production_timing) {
                for (auto& row : preview_timing(cases.at("timing_cases"), layout, server.socket, fallback, true)) report["preview_timing"].push_back(std::move(row));
                continue;
            }
            if (production_typos) {
                for (auto& row : typos(cases.at("articles"), layout, fallback, server.socket, 0, true)) report["typos"].push_back(std::move(row));
                continue;
            }
            if (model_typos) {
                for (auto& row : typos(cases.at("articles"), layout, fallback, server.socket, 1)) report["typos"].push_back(std::move(row));
                continue;
            }
            if (model_timing) {
                for (auto& row : preview_timing(cases.at("timing_cases"), layout, server.socket, fallback)) report["preview_timing"].push_back(std::move(row));
                continue;
            }
            if (model_comparison) {
                const std::vector<std::size_t> path_counts = candidates_only ? std::vector<std::size_t>{4} : std::vector<std::size_t>{0, 1, 4};
                for (const auto paths : path_counts) {
                    for (const auto& item : cases.at("articles")) {
                        const auto mode = paths == 0 ? "baseline" : paths == 1 ? "live-best" : "live-candidates";
                        auto row = article(item, layout, server.socket, fallback, mode, cadence, paths);
                        std::printf("[%s/%s] %s CER=%.3f prefix=%zu/%zu\n", row.at("layout").get<std::string>().c_str(),
                            mode, item.at("id").get<std::string>().c_str(), row.at("cer").get<double>(),
                            row.at("unit_prefix_exact").get<std::size_t>(), row.at("units").get<std::size_t>());
                        std::fflush(stdout);
                        report["articles"].push_back(std::move(row));
                    }
                    for (auto& row : clause_recovery(cases.at("articles"), layout, server.socket, fallback, paths)) report["clause_recovery"].push_back(std::move(row));
                    for (auto& row : workflows(cases.at("edit_workflows"), layout, server.socket, fallback, paths)) report["workflows"].push_back(std::move(row));
                }
                continue;
            }
            if (!focused && !repaired_only) {
                for (const auto& item : cases.at("articles")) {
                    for (const std::string mode : {"normal-sentence", "burst-paragraph", "offline-sentence"}) {
                        auto row = article(item, layout, mode == "offline-sentence" ? "" : server.socket,
                                           fallback, mode, mode == "normal-sentence" ? cadence : 0);
                        std::printf("article %s %s %s CER=%.3f requests=%llu\n", row.at("layout").get<std::string>().c_str(),
                            item.at("id").get<std::string>().c_str(), mode.c_str(), row.at("cer").get<double>(),
                            static_cast<unsigned long long>(row.at("model_requests").get<std::uint64_t>()));
                        std::fflush(stdout);
                        report["articles"].push_back(std::move(row));
                    }
                    if (item.at("category") == "chinese") {
                        report["articles"].push_back(article(item, layout, server.socket, fallback, "traditional", cadence));
                    }
                }
                for (auto& row : workflows(cases.at("edit_workflows"), layout, server.socket, fallback)) report["workflows"].push_back(std::move(row));
                for (auto& row : typos(cases.at("articles"), layout, fallback)) report["typos"].push_back(std::move(row));
                for (auto& row : long_composition(cases.at("articles"), layout, fallback)) report["long_composition"].push_back(std::move(row));
            }
            if (!focused) {
                for (const auto& item : cases.at("articles")) {
                    report["articles"].push_back(repaired_article(item, layout, server.socket, fallback, cadence));
                }
            }
            if (!repaired_only) {
                for (auto& row : clause_recovery(cases.at("articles"), layout, server.socket, fallback)) report["clause_recovery"].push_back(std::move(row));
                for (auto& row : punctuation_timing(cases.at("articles"), layout, server.socket, fallback, cadence)) report["punctuation_timing"].push_back(std::move(row));
            }
        }
        std::ofstream output(args[4]);
        if (!output) throw std::runtime_error("cannot create report");
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write report");
        std::printf("report=%s articles=%zu workflows=%zu typos=%zu\n", args[4].c_str(), report["articles"].size(),
                    report["workflows"].size(), report["typos"].size());
        return 0;
    } catch (const Failure& error) {
        std::fprintf(stderr, "raw-key invariant failed: %s\n", error.message.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "simulation failed: %s\n", error.what());
    }
    return 1;
}

#include "raw_key_harness.hpp"
#include "engine/fallback_engine.hpp"
#include "text/utf.hpp"
#include "ipc/unix_socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {

protocol::Message receive(const UnixSocketConnection& connection) {
    auto bytes = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, bytes.data(), sizeof(length));
    const auto payload = connection.recv_exact(length);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return protocol::decode(bytes);
}

// Deliberately delay a real transport response until the test releases it.
// A bounded wait also makes unwinding a failed assertion safe.
class ScriptService {
public:
    explicit ScriptService(bool hold = false, int invalid = 0, bool delay_alternatives = false, bool hold_open = false,
                           bool singleton_words = false, bool structured_words = false)
        : hold_(hold || delay_alternatives), invalid_(invalid), delay_alternatives_(delay_alternatives),
          hold_open_(hold_open), singleton_words_(singleton_words), structured_words_(structured_words) {
        static std::size_t serial = 0;
        socket = std::filesystem::temp_directory_path() /
                 ("mixed-live-" + std::to_string(getpid()) + "-" + std::to_string(++serial) + ".sock");
        server_.bind_listen(socket);
        worker_ = std::thread([this] { serve(); });
    }
    ~ScriptService() {
        release();
        if (calls.load() == 0 && !connected.load()) {
            try { (void)UnixSocketClient{}.connect(socket); } catch (...) {}
        }
        if (worker_.joinable()) worker_.join();
    }
    void release() {
        { std::lock_guard<std::mutex> lock(mutex_); hold_ = false; }
        condition_.notify_all();
    }
    void release_one() {
        { std::lock_guard<std::mutex> lock(mutex_); released_calls_ = calls.load(); }
        condition_.notify_all();
    }
    void hold() {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_ = true;
        released_calls_ = calls.load();
    }
    std::vector<std::u16string> contexts() {
        std::lock_guard<std::mutex> lock(mutex_);
        return contexts_;
    }
    std::vector<protocol::SessionId> prediction_sessions() {
        std::lock_guard<std::mutex> lock(mutex_);
        return prediction_sessions_;
    }
    std::filesystem::path socket;
    std::atomic<std::size_t> calls{0};
    std::atomic<std::size_t> opens{0};
    std::atomic<std::size_t> closes{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> ok{true};
private:
    void serve() {
        std::size_t sessions_remaining = 0;
        try {
            auto connection = server_.accept_one();
            connected = true;
            protocol::SessionId session{};
            session[0] = 0x78;
            protocol::ServiceEpoch epoch{};
            epoch[0] = 0x22;
            const FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
            for (;;) {
                const auto message = receive(connection);
                if (std::holds_alternative<protocol::StatusRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::Message{
                        protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
                } else if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                    ++session[0];
                    ++sessions_remaining;
                    ++opens;
                    if (hold_open_) {
                        std::unique_lock<std::mutex> lock(mutex_);
                        (void)condition_.wait_for(lock, std::chrono::seconds(3), [&] { return !hold_; });
                    }
                    connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));
                } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        contexts_.push_back(request->context);
                        prediction_sessions_.push_back(request->session_id);
                        ++calls;
                        (void)condition_.wait_for(lock, std::chrono::seconds(3), [&] {
                            return !hold_ || released_calls_ >= calls.load() ||
                                   (delay_alternatives_ && calls.load() == 1);
                        });
                    }
                    protocol::Prediction response{request->session_id, request->request_id, request->buffer_revision, {}};
                    for (const auto& entry : request->padding) {
                        auto candidates = fallback.lookup(entry.bopomofo());
                        if (entry.bopomofo() == u"ㄋㄧˇ") candidates = {U'擬', U'你'};
                        if (entry.bopomofo() == u"ㄏㄠˇ") candidates = {U'好'};
                        if (singleton_words_) {
                            if (entry.bopomofo() == u"ㄨ ") candidates = {U'巫'};
                            if (entry.bopomofo() == u"ㄕ ") candidates = {U'師'};
                            if (entry.bopomofo() == u"ㄙㄢ ") candidates = {U'三'};
                        }
                        if (structured_words_) {
                            if (entry.bopomofo() == u"ㄒㄧㄣ ") candidates = {U'新'};
                            if (entry.bopomofo() == u"ㄉㄜ˙") candidates = {U'的'};
                            if (entry.bopomofo() == u"ㄐㄧˋ") candidates = {U'寄'};
                            if (entry.bopomofo() == u"ㄍㄟˇ") candidates = {U'給'};
                            if (entry.bopomofo() == u"ㄊㄨㄥˊ") candidates = {U'同'};
                            if (entry.bopomofo() == u"ㄕˋ") candidates = {U'事'};
                            if (entry.bopomofo() == u"ㄌㄧˇ") candidates = {U'裡'};
                        }
                        if (entry.chosen()) candidates = {entry.chosen_char()};
                        if (invalid_ == 2) candidates = {U'龍'};
                        response.candidates.push_back(std::move(candidates));
                    }
                    if (invalid_ == 1) ++response.request_id;
                    if (invalid_ == 3 || (invalid_ == 4 && calls.load() == 1)) {
                        connection.send_all(protocol::encode(protocol::Message{protocol::Error{
                            invalid_ == 4 ? protocol::ErrorCode::UnknownSession : protocol::ErrorCode::ProtocolError,
                            request->session_id, request->request_id,
                            request->buffer_revision, "scripted unavailable model"}}));
                    } else {
                        connection.send_all(protocol::encode(protocol::Message{response}));
                    }
                } else if (const auto* close = std::get_if<protocol::CloseSessionRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Message{protocol::CloseSessionResponse{close->session_id}}));
                    if (sessions_remaining != 0) --sessions_remaining;
                    ++closes;
                } else if (const auto* record = std::get_if<protocol::RecordCommitRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Message{protocol::RecordCommitResponse{record->event_id, true}}));
                } else if (const auto* discard = std::get_if<protocol::DiscardCommitRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Message{protocol::DiscardCommitResponse{discard->event_id, true}}));
                } else { ok = false; break; }
            }
        } catch (...) { if (sessions_remaining != 0) ok = false; }
    }
    UnixSocketServer server_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool hold_ = false;
    int invalid_ = 0;
    bool delay_alternatives_ = false;
    bool hold_open_ = false;
    bool singleton_words_ = false;
    bool structured_words_ = false;
    std::size_t released_calls_ = 0;
    std::vector<std::u16string> contexts_;
    std::vector<protocol::SessionId> prediction_sessions_;
};

HarnessOptions smart_options() {
    HarnessOptions result;
    result.config.smart_english = true;
    // Production buffer predictions are unavailable in these focused tests;
    // only the experimental adapter connects to the scripted service.
    result.socket_path = "/nonexistent/live-mixed-model-test.sock";
    return result;
}

void wait_request(Harness& harness, ScriptService& service);

RAWKEY_SUITE("deferred commit waits for the latest buffer and mixed model result", deferred_model_commit) {
    for (const bool smart : {false, true}) {
        for (const std::string submit : {"Return", "KP_Enter", "Shift+KP_Enter", "Shift+space"}) {
            ScriptService service(true);
            HarnessOptions options;
            options.config.smart_english = smart;
            options.config.smart_model_preview = smart;
            options.socket_path = service.socket.string();
            Harness harness(options);
            harness.type("su3");
            wait_request(harness, service);
            // The first request describes only the first syllable. Submission
            // must also wait for the dirty/newest query, not this stale reply.
            harness.type("cl3");
            const auto shown = harness.preedit();
            RAWKEY_ASSERT(harness.key_accepted(submit));
            RAWKEY_ASSERT(harness.commits().empty());
            RAWKEY_ASSERT(!harness.composition_empty());
            RAWKEY_ASSERT(!harness.update_ready());
            RAWKEY_ASSERT(harness.preedit() == shown);
            RAWKEY_ASSERT(harness.key_accepted(submit));
            RAWKEY_ASSERT(harness.commits().empty());
            service.release_one();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (service.calls.load() < 2 && std::chrono::steady_clock::now() < deadline) {
                (void)harness.pump_until([&] { return service.calls.load() >= 2; }, std::chrono::milliseconds(10));
            }
            RAWKEY_ASSERT(service.calls.load() >= 2);
            RAWKEY_ASSERT(harness.commits().empty());
            service.release();
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
            RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{
                submit == "Shift+space" ? "擬好 " : "擬好"});
            RAWKEY_ASSERT(harness.update_ready());
        }
    }
}

RAWKEY_SUITE("deferred commit waits while the model session is opening", deferred_model_open) {
    for (const bool smart : {false, true}) {
        ScriptService service(true, 0, false, true);
        HarnessOptions options;
        options.config.smart_english = smart;
        options.config.smart_model_preview = smart;
        options.socket_path = service.socket.string();
        Harness harness(options);
        harness.type("su3");
        RAWKEY_ASSERT(harness.pump_until([&] { return service.opens.load() != 0; }));
        RAWKEY_ASSERT(service.calls.load() == 0);
        harness.key("Return");
        RAWKEY_ASSERT(harness.commits().empty());
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
        RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{"擬"});
    }
}

RAWKEY_SUITE("deferred commit stays with its original input context", deferred_model_context) {
    ScriptService service(true);
    HarnessOptions options;
    options.socket_path = service.socket.string();
    Harness harness(options);
    harness.type("su3");
    wait_request(harness, service);
    harness.key("Return");
    RAWKEY_ASSERT(harness.commits().empty());
    harness.use_context(2);
    harness.type("cl3");
    service.release();
    RAWKEY_ASSERT(harness.pump_until([&] {
        return harness.host().commits().size() == 1 && !harness.session()->prediction.pending;
    }));
    const auto commits = harness.host().commits();
    RAWKEY_ASSERT(commits.front().first == 1 && commits.front().second == u"擬");
    RAWKEY_ASSERT(harness.preedit() == "好");
    harness.key("Return");
    RAWKEY_ASSERT(harness.host().commits().back().first == 2);
    RAWKEY_ASSERT(harness.last_commit() == "好");
    harness.use_context(1);
    RAWKEY_ASSERT(harness.composition_empty());
}

RAWKEY_SUITE("deferred commit falls back once when the model reports failure", deferred_model_failure) {
    for (const bool smart : {false, true}) {
        ScriptService service(true, 3);
        HarnessOptions options;
        options.config.smart_english = smart;
        options.config.smart_model_preview = smart;
        options.socket_path = service.socket.string();
        Harness harness(options);
        harness.type("su3");
        wait_request(harness, service);
        const auto fallback = harness.preedit();
        harness.key("Return");
        RAWKEY_ASSERT(harness.commits().empty());
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.composition_empty(); }));
        RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{fallback});
    }
}

RAWKEY_SUITE("deferred commit is cancelled by edits reset detach and settings", deferred_model_cancellation) {
    for (const std::string action : {"edit", "escape", "reset", "detach", "settings", "transport"}) {
        ScriptService service(true);
        auto options = smart_options();
        options.config.smart_model_preview = true;
        options.socket_path = service.socket.string();
        Harness harness(options);
        harness.type("su3");
        wait_request(harness, service);
        harness.key("Return");
        RAWKEY_ASSERT(harness.commits().empty());
        if (action == "edit") harness.type("cl3");
        else if (action == "escape") harness.key("Escape");
        else if (action == "reset") harness.reset();
        else if (action == "detach") { harness.detach(); harness.use_context(1); }
        else if (action == "settings") harness.set_config("SmartModelPreview", "False");
        else {
            ServiceTransportOptions transport;
            transport.socket_path = "/nonexistent/deferred-commit-test.sock";
            transport.auto_start = false;
            harness.engine().set_transport_options(transport);
        }
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        RAWKEY_ASSERT(harness.commits().empty());
        RAWKEY_ASSERT(!harness.session()->deferred_commit);
        if (action == "edit") {
            RAWKEY_ASSERT(harness.preedit() == "擬好");
            harness.key("Return");
            RAWKEY_ASSERT(harness.last_commit() == "擬好");
        }
    }
}

RAWKEY_SUITE("reattached native handle cannot adopt a previous lifetime open reply", reused_handle_open_reply) {
    for (const std::string boundary : {"detach", "focus", "reset"}) {
        ScriptService service(true, 0, false, true);
        HarnessOptions options;
        options.socket_path = service.socket.string();
        Harness harness(options);
        harness.type("su3");
        RAWKEY_ASSERT(harness.pump_until([&] { return service.opens.load() == 1; }, std::chrono::seconds(1)));
        RAWKEY_ASSERT(!harness.session()->prediction.session_open());
        RAWKEY_ASSERT(service.calls.load() == 0);
        if (boundary == "detach") harness.detach();
        else if (boundary == "focus") harness.focus_out();
        else harness.reset();
        harness.use_context(1);  // Reused handle or generation-invalidated same object.
        harness.type("cl3");
        RAWKEY_ASSERT(harness.preedit() == "好");
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] {
            return service.opens.load() == 2 && !harness.session()->prediction.pending;
        }));
        const auto sessions = service.prediction_sessions();
        RAWKEY_ASSERT(!sessions.empty());
        for (const auto& session : sessions) RAWKEY_ASSERT(session[0] == 0x7a);
        RAWKEY_ASSERT(harness.session()->prediction.session_id[0] == 0x7a);
        RAWKEY_ASSERT(harness.pump_until([&] { return service.closes.load() == 1; }));
        RAWKEY_ASSERT(harness.preedit() == "好");
        if (boundary == "focus") RAWKEY_ASSERT(harness.last_commit() == "你");
        else RAWKEY_ASSERT(harness.commits().empty());
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "好");
    }
}

RAWKEY_SUITE("production mixed model across keyboard layouts", production_layout_models) {
    // Independent physical keys; the service sees canonical readings, not a
    // layout-specific tokenizer or a remapped English identifier.
    const std::pair<std::string, std::string> layouts[]{
        {"standard", "su3cl3"}, {"hsu", "nefhwf"}, {"ibm", "7a,-;,"},
        {"et", "ne3hz3"}, {"ginyieh", "d-avla"}, {"et26", "nejhzj"},
        {"dachen_cp26", "surclr"},
    };
    for (const auto& [name, keys] : layouts) {
        ScriptService service;
        auto value = smart_options();
        value.config.keyboard_layout = name;
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.type(keys);
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        RAWKEY_ASSERT(service.calls.load() != 0);
        RAWKEY_ASSERT(harness.preedit() == "擬好");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "擬好");
        RAWKEY_ASSERT(service.ok.load());
    }
    for (const auto& [name, keys] : layouts) {
        ScriptService service(true);
        auto value = smart_options();
        value.config.keyboard_layout = name;
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.type(keys);
        wait_request(harness, service);
        harness.key("Down");
        const auto panel = harness.candidates();
        const auto shown = harness.preedit();
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        RAWKEY_ASSERT(harness.candidates() == panel);
        RAWKEY_ASSERT(harness.preedit() == shown);
        harness.choose_text(keys);
        harness.expect_commit(keys);
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production leading first tone gang reaches the model before a following word", production_leading_gang) {
    for (const auto& [layout, keys] : std::array{
        std::pair{"standard", "e; "}, std::pair{"hsu", "gk "},
        std::pair{"ibm", "9v "}, std::pair{"et", "v0 "},
        std::pair{"ginyieh", "r; "}, std::pair{"et26", "vt "},
        std::pair{"dachen_cp26", "ell "},
    }) {
        ScriptService service;
        auto value = smart_options();
        value.config.keyboard_layout = layout;
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.type(keys);
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        RAWKEY_ASSERT(service.calls.load() != 0);
        RAWKEY_ASSERT(harness.preedit() == "剛");
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
        harness.expect_commit("剛");
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production leading punctuation readings reach the model and respect explicit choices", production_leading_symbols) {
    constexpr std::array keys{
        std::array{"standard", "d; ", "w; ", "d/ ", "t. ", "y; "},
        std::array{"hsu", "kk ", "tk ", "kl ", "vo ", "zk "},
        std::array{"ibm", "0v ", "6v ", "0b ", "tz ", "iv "},
        std::array{"et", "k0 ", "t0 ", "k- ", ".y ", ";0 "},
        std::array{"ginyieh", "f; ", "e; ", "f/ ", "y. ", "u; "},
        std::array{"et26", "kt ", "tt ", "kl ", "yp ", "qt "},
        std::array{"dachen_cp26", "dll ", "wwll ", "dn ", "ttmm ", "yll "},
    };
    constexpr std::array words{"康", "湯", "坑", "抽", "髒"};
    for (const auto& row : keys) {
        for (size_t index = 0; index < words.size(); ++index) {
            for (const auto route : {"model", "raw", "manual", "immediate-enter"}) {
                ScriptService service(std::string_view(route) != "model");
                auto value = smart_options();
                value.config = config_from_json({{"smart_english", true}});
                value.config.keyboard_layout = row[0];
                value.socket_path = service.socket.string();
                Harness harness(value);
                const auto body = std::string(row[index + 1]).substr(0, std::string_view(row[index + 1]).size() - 1);
                harness.type(body);
                RAWKEY_ASSERT(harness.preedit() == body);
                RAWKEY_ASSERT(harness.pending_model_requests() == 0);
                harness.key("space");
                RAWKEY_ASSERT(harness.preedit() == words[index]);
                // ScriptService's counter changes on its worker, without a
                // host post until the held reply is released. Drain host work
                // in short bounded slices so each route need not wait for the
                // entire request deadline before observing that counter.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (service.calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                    (void)harness.pump_until([&] { return service.calls.load() != 0; }, std::chrono::milliseconds(10));
                }
                RAWKEY_ASSERT(service.calls.load() != 0);
                std::string expected = words[index];
                if (std::string_view(route) == "model") {
                    RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
                    RAWKEY_ASSERT(harness.preedit() == expected);
                } else if (std::string_view(route) == "immediate-enter") {
                    harness.expect_commit(expected);
                } else {
                    harness.key("Down");
                    expected = std::string_view(route) == "raw" ? row[index + 1] : words[index];
                    harness.choose_text(expected);
                    RAWKEY_ASSERT(harness.preedit() == expected);
                }
                service.release();
                RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
                if (std::string_view(route) == "immediate-enter") {
                    RAWKEY_ASSERT(harness.composition_empty());
                    RAWKEY_ASSERT(harness.last_commit() == expected);
                } else {
                    RAWKEY_ASSERT(harness.preedit() == expected);
                    harness.expect_commit(expected);
                }
                RAWKEY_ASSERT(service.ok.load());
            }
        }
    }
}

RAWKEY_SUITE("production Hsu recovered letter tones reach the model without losing explicit choices", production_hsu_completion) {
    for (const auto& [keys, text] : std::array{
        std::pair{"mif", "買"}, std::pair{"gof", "狗"}, std::pair{"hif", "海"},
        std::pair{"myf", "馬"}, std::pair{"mef", "米"}, std::pair{"kof", "口"},
        std::pair{"mof", "某"}, std::pair{"bej", "必"}, std::pair{"leof", "柳"},
        std::pair{"ij", "愛"}, std::pair{"vd", "持"}, std::pair{"veof", "糗"},
        std::pair{"naf", "餒"}, std::pair{"vf", "尺"}, std::pair{"goj", "夠"},
        std::pair{"emf", "眼"}, std::pair{"emj", "驗"}, std::pair{"xhj", "握"},
        std::pair{"xk ", "汪"}, std::pair{"xa ", "威"}}) {
        for (const auto route : {"model", "raw", "manual", "immediate-enter"}) {
            ScriptService service(std::string_view(route) != "model");
            auto value = smart_options();
            value.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
            value.socket_path = service.socket.string();
            Harness harness(value);
            const std::string raw(keys);
            harness.type(raw.substr(0, raw.size() - 1));
            RAWKEY_ASSERT(harness.preedit() == raw.substr(0, raw.size() - 1));
            RAWKEY_ASSERT(harness.pending_model_requests() == 0);
            harness.type(raw.substr(raw.size() - 1));
            RAWKEY_ASSERT(harness.preedit() == text);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (service.calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                (void)harness.pump_until([&] { return service.calls.load() != 0; }, std::chrono::milliseconds(10));
            }
            RAWKEY_ASSERT(service.calls.load() != 0);
            std::string expected(text);
            if (std::string_view(route) == "immediate-enter") {
                harness.expect_commit(expected);
            } else if (std::string_view(route) != "model") {
                harness.key("Down");
                expected = std::string_view(route) == "raw" ? raw : text;
                harness.choose_text(expected);
            }
            service.release();
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
            if (std::string_view(route) == "immediate-enter") {
                RAWKEY_ASSERT(harness.composition_empty());
                RAWKEY_ASSERT(harness.last_commit() == expected);
            } else {
                RAWKEY_ASSERT(harness.preedit() == expected);
                harness.expect_commit(expected);
            }
            RAWKEY_ASSERT(service.ok.load());
        }
    }
}

RAWKEY_SUITE("production Hsu phrase boundary repair rejects late responses after explicit choices", production_hsu_phrase_boundaries) {
    for (const auto& [keys, text] : std::array{
        std::pair{"dlfedceyj", "等一下"}, std::pair{"kofdijlefeofleldvemd", "口袋李有零錢"},
        std::pair{"uljvimdyfki dkfmj", "用vim打開檔案"}}) {
        std::string modeled;
        for (const auto route : {"model", "raw", "manual", "immediate-enter"}) {
            ScriptService service(true);
            auto options = smart_options();
            options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
            options.socket_path = service.socket.string();
            Harness harness(options);
            harness.type(keys);
            if (harness.preedit() != text) throw Failure{std::string(keys) + " expected " + text + ", got " + harness.preedit()};
            const auto readings = [&] {
                const auto& decision = harness.session()->mixed_decision;
                std::vector<std::u16string> value;
                for (const auto& segment : decision.result.paths.at(decision.preview_path).segments) {
                    if (segment.kind == MixedSegmentKind::Bopomofo) value.push_back(segment.reading);
                }
                return value;
            };
            const auto before = readings();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (service.calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                (void)harness.pump_until([&] { return service.calls.load() != 0; }, std::chrono::milliseconds(10));
            }
            RAWKEY_ASSERT(service.calls.load() != 0);
            std::string expected(text);
            if (std::string_view(route) == "immediate-enter") {
                expected = modeled;
                harness.expect_commit(expected);
                RAWKEY_ASSERT(harness.commits().empty());
            } else if (std::string_view(route) != "model") {
                harness.key("Down");
                expected = std::string_view(route) == "raw" ? keys : text;
                harness.choose_text(expected);
            }
            service.release();
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
            if (std::string_view(route) == "immediate-enter") {
                RAWKEY_ASSERT(harness.composition_empty());
                RAWKEY_ASSERT(harness.last_commit() == expected);
            } else {
                if (std::string_view(route) == "model") {
                    RAWKEY_ASSERT(readings() == before);
                    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
                    expected = harness.preedit();
                    modeled = expected;
                } else RAWKEY_ASSERT(harness.preedit() == expected);
                harness.expect_commit(expected);
            }
            RAWKEY_ASSERT(service.ok.load());
        }
    }
}

RAWKEY_SUITE("production Hsu structured repair choices survive late model replies", production_hsu_structured_boundaries) {
    for (const auto& [keys, repaired] : std::array{
        std::pair{"cem byfreport.csvjejgaftxldcj", "先把report.csv寄給同事"},
        std::pair{"cen dgsssh_keyfkjzijjgjlef", "心的ssh_key放在這裡"}}) {
        for (const auto route : {"repair", "raw", "panel", "immediate-enter"}) {
            ScriptService service(true);
            auto options = smart_options();
            options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
            options.socket_path = service.socket.string();
            Harness harness(options);
            harness.type(keys);
            const auto automatic = harness.preedit();
            RAWKEY_ASSERT(!harness.session()->mixed_decision.result.paths.at(
                harness.session()->mixed_decision.preview_path).boundary_alternative);
            RAWKEY_ASSERT(harness.pump_until([&] { return service.calls.load() != 0; }));
            std::string expected;
            if (std::string_view(route) == "immediate-enter") {
                expected = automatic;
                harness.expect_commit(expected);
            } else {
                harness.key("Down");
                RAWKEY_ASSERT(harness.candidate(1) == keys);
                RAWKEY_ASSERT(harness.candidate(2) == repaired);
                if (std::string_view(route) != "panel") {
                    expected = std::string_view(route) == "raw" ? keys : repaired;
                    harness.choose_text(expected);
                }
            }
            service.release();
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
            if (std::string_view(route) == "immediate-enter") {
                RAWKEY_ASSERT(harness.composition_empty());
                RAWKEY_ASSERT(harness.last_commit() == expected);
            } else {
                if (std::string_view(route) == "panel") {
                    RAWKEY_ASSERT(harness.has_candidates());
                    RAWKEY_ASSERT(harness.preedit() == automatic);
                    RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
                    expected = harness.candidate(2);
                    RAWKEY_ASSERT(!expected.empty() && expected != keys);
                    harness.choose_text(expected);
                }
                RAWKEY_ASSERT(harness.preedit() == expected);
                harness.expect_commit(expected);
            }
            RAWKEY_ASSERT(service.ok.load());
        }
    }
}

RAWKEY_SUITE("production boundary candidate model improves homophones without replacing raw or fallback choices", production_boundary_candidate_model) {
    for (const auto& [keys, fallback, refined, suffix_context] : std::array{
        std::tuple{"cem byfreport.csvjejgaftxldcj", "先把report.csv寄給同事", "先把report.csv寄給同事", "先把report.csv"},
        std::tuple{"cen dgsssh_keyfkjzijjgjlef", "心的ssh_key放在這裡", "新的ssh_key放在這裡", "新的ssh_key"}}) {
        for (const auto route : {"refine", "navigate", "manual", "raw", "escape", "enter", "focus"}) {
            ScriptService service(false, 0, false, false, false, true);
            auto options = smart_options();
            options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
            options.socket_path = service.socket.string();
            Harness harness(options);
            harness.type(keys);
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
            const auto automatic = harness.preedit();
            const auto before_requests = harness.pending_model_requests();
            const auto before_calls = service.calls.load();
            service.hold();
            harness.key("Down");
            RAWKEY_ASSERT(harness.candidate(2) == fallback);
            // The external server counter does not wake FakeHost's queue.
            // Use short pumps so cancellation really precedes the held reply,
            // rather than waiting for the mock's three-second safety timeout.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (service.calls.load() <= before_calls && std::chrono::steady_clock::now() < deadline) {
                (void)harness.pump_until([&] { return service.calls.load() > before_calls; }, std::chrono::milliseconds(10));
            }
            RAWKEY_ASSERT(service.calls.load() > before_calls);
            RAWKEY_ASSERT(harness.pending_model_requests() == before_requests + 1);
            if (std::string_view(route) == "refine") {
                service.release();
                RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
                RAWKEY_ASSERT(harness.preedit() == automatic);
                RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
                RAWKEY_ASSERT(harness.candidate(1) == keys);
                RAWKEY_ASSERT(harness.candidate(2) == fallback);
                const size_t refined_row = std::string_view(fallback) == refined ? 2 : 3;
                RAWKEY_ASSERT(harness.candidate(refined_row) == refined);
                const auto choices = harness.candidates();
                RAWKEY_ASSERT(std::ranges::find(choices, fallback) != choices.end());
                RAWKEY_ASSERT(harness.pending_model_requests() == before_requests + 2);
                RAWKEY_ASSERT(service.contexts().back() == utf8_to_u16(suffix_context));
                RAWKEY_ASSERT(std::ranges::count_if(harness.session()->mixed_decision.result.paths, [](const auto& path) {
                    return path.boundary_model_refined;
                }) <= 1);
                harness.key("Escape");
                RAWKEY_ASSERT(harness.preedit() == automatic);
                harness.key("Down");
                RAWKEY_ASSERT(harness.candidate(2) == fallback);
                RAWKEY_ASSERT(harness.candidate(refined_row) == refined);
                RAWKEY_ASSERT(harness.pending_model_requests() == before_requests + 2);
                harness.choose_text(refined);
                harness.expect_commit(refined);
            } else {
                std::string expected(automatic);
                if (std::string_view(route) == "navigate") {
                    harness.key("Down");
                    harness.key("Down");
                    RAWKEY_ASSERT(harness.cursor_index() == 2);
                } else if (std::string_view(route) == "manual" || std::string_view(route) == "raw") {
                    expected = std::string_view(route) == "manual" ? fallback : keys;
                    harness.choose_text(expected);
                } else if (std::string_view(route) == "escape") harness.key("Escape");
                else if (std::string_view(route) == "enter") {
                    const auto start = std::chrono::steady_clock::now();
                    harness.expect_commit(expected);
                    RAWKEY_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
                }
                else harness.expect_focus_out_commit(expected);
                service.release();
                RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
                if (std::string_view(route) == "navigate") {
                    RAWKEY_ASSERT(harness.cursor_index() == 2);
                    RAWKEY_ASSERT(harness.candidate(2) == fallback);
                    harness.key(" ");
                    expected = fallback;
                }
                if (std::string_view(route) == "enter" || std::string_view(route) == "focus") {
                    RAWKEY_ASSERT(harness.last_commit() == expected);
                    RAWKEY_ASSERT(harness.composition_empty());
                } else {
                    RAWKEY_ASSERT(harness.preedit() == expected);
                    harness.expect_commit(expected);
                }
            }
            RAWKEY_ASSERT(service.ok.load());
        }
    }
}

RAWKEY_SUITE("production boundary candidate skips inference when its extra row cannot be displayed", production_boundary_candidate_short_panel) {
    for (const int page_size : {2, 3}) {
        ScriptService service;
        auto options = smart_options();
        options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"},
                                           {"candidate_page_size", page_size}});
        options.socket_path = service.socket.string();
        Harness harness(options);
        const std::string keys = "cen dgsssh_keyfkjzijjgjlef";
        harness.type(keys);
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        const auto requests = harness.pending_model_requests();
        harness.key("Down");
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        RAWKEY_ASSERT(harness.pending_model_requests() == requests);
        RAWKEY_ASSERT(harness.candidate(1) == keys);
        if (page_size == 3) {
            RAWKEY_ASSERT(harness.candidate(2) == "心的ssh_key放在這裡");
            harness.choose_text("心的ssh_key放在這裡");
            harness.expect_commit("心的ssh_key放在這裡");
        } else {
            harness.choose_text(keys);
            harness.expect_commit(keys);
        }
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production boundary candidate errors preserve fallback and do not loop", production_boundary_candidate_errors) {
    for (const int invalid : {1, 2, 3}) {
        ScriptService service(false, invalid);
        auto options = smart_options();
        options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
        options.socket_path = service.socket.string();
        Harness harness(options);
        harness.type("cen dgsssh_keyfkjzijjgjlef");
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        const auto before = harness.preedit();
        const auto requests = harness.pending_model_requests();
        harness.key("Down");
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        RAWKEY_ASSERT(harness.preedit() == before);
        RAWKEY_ASSERT(harness.candidate(2) == "心的ssh_key放在這裡");
        RAWKEY_ASSERT(harness.pending_model_requests() == requests + 1);
        harness.key("Escape");
        harness.key("Down");
        RAWKEY_ASSERT(harness.pending_model_requests() == requests + 1);
        harness.choose_text("心的ssh_key放在這裡");
        harness.expect_commit("心的ssh_key放在這裡");
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production boundary candidate model keeps middle context left only and can refine a raw winner", production_boundary_candidate_context) {
    for (const bool middle : {false, true}) {
        ScriptService service(false, 0, false, false, false, true);
        auto options = smart_options();
        options.config = config_from_json({{"smart_english", true}, {"keyboard_layout", "hsu"}});
        options.socket_path = service.socket.string();
        Harness harness(options);
        if (middle) {
            harness.type("nefhwf");
            harness.key("Down");
            harness.choose_text("你好");
            harness.key("Left");
        }
        const std::string keys = middle ? "cen dgsssh_keyfkjzijjgjlef" : "velfbyfreport.csvfkjzijjgjlef";
        const std::string target = middle ? "新的ssh_key放在這裡" : "請把report.csv放在這裡";
        harness.type(keys);
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        if (!middle) RAWKEY_ASSERT(harness.session()->mixed_decision.preview_path == 0);
        const auto automatic = harness.preedit();
        const auto requests = harness.pending_model_requests();
        harness.key("Down");
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
        RAWKEY_ASSERT(harness.preedit() == automatic);
        RAWKEY_ASSERT(harness.session()->pending_token.raw == utf8_to_u16(keys));
        RAWKEY_ASSERT(harness.pending_model_requests() == requests + 2);
        RAWKEY_ASSERT(service.contexts().back() == utf8_to_u16(middle ? "你新的ssh_key" : "請把report.csv"));
        const auto choices = harness.candidates();
        RAWKEY_ASSERT(std::ranges::find(choices, target) != choices.end());
        harness.choose_text(target);
        harness.expect_commit(middle ? "你" + target + "好" : target);
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production mixed model preview", production_mixed_model_preview) {
    const auto options = [](const ScriptService& service) {
        auto value = smart_options();
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        return value;
    };
    const auto settled = [](Harness& harness) {
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }, std::chrono::seconds(5)));
    };
    {
        ScriptService service;
        auto value = options(service);
        std::u16string training_answer;
        value.on_training_commit = [&](const auto& sample, auto) { training_answer = sample.answer; };
        Harness harness(value);
        harness.type("su3"); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "擬");
        const auto calls = service.calls.load();
        harness.type("c"); settled(harness);
        RAWKEY_ASSERT(harness.preedit().starts_with("擬"));
        RAWKEY_ASSERT(service.calls.load() == calls);
        harness.key("BackSpace"); settled(harness);
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "擬");
        RAWKEY_ASSERT(training_answer == u"擬");
    }
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.type("su3"); wait_request(harness, service);
        harness.key("Shift+BackSpace"); harness.type("4");
        const auto corrected = harness.preedit();
        service.release(); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == corrected);
        RAWKEY_ASSERT(harness.preedit() != "擬");
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == corrected);
    }
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.type("su3"); wait_request(harness, service);
        harness.key("Down");
        const auto panel = harness.candidates();
        const auto shown = harness.preedit();
        service.release(); settled(harness);
        RAWKEY_ASSERT(harness.candidates() == panel);
        RAWKEY_ASSERT(harness.preedit() == shown);
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == shown);
    }
    for (const std::string action : {"Return", "Escape", "focus", "reset", "detach"}) {
        ScriptService service(true);
        Harness harness(options(service));
        harness.type("su3"); wait_request(harness, service);
        if (action == "focus") harness.focus_out();
        else if (action == "reset") harness.reset();
        else if (action == "detach") harness.detach();
        else {
            harness.key(action);
            if (action == "Escape") harness.key("Escape");
        }
        const auto commits = harness.commits();
        service.release();
        (void)harness.pump_until([] { return false; }, std::chrono::milliseconds(100));
        if (action == "Return") RAWKEY_ASSERT(harness.commits() == std::vector<std::string>{"擬"});
        else RAWKEY_ASSERT(harness.commits() == commits);
        RAWKEY_ASSERT(harness.composition_empty());
    }
    {
        ScriptService service(true, 0, false, true);
        Harness harness(options(service));
        harness.type("su3");
        RAWKEY_ASSERT(harness.pump_until([&] { return service.opens.load() == 1; }, std::chrono::seconds(2)));
        harness.detach();  // The service has not returned the new session ID.
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] { return service.closes.load() == 1; }, std::chrono::seconds(2)));
        RAWKEY_ASSERT(service.calls.load() == 0);
    }
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.set_config("SelectionKeys", "數字鍵");
        harness.type("su3"); wait_request(harness, service);
        harness.key("Down"); harness.choose_text("你");
        harness.type("hello");
        service.release(); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "你hello");
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "你hello");
    }
    for (const bool sensitive : {false, true}) {
        ScriptService service;
        Harness harness(options(service));
        harness.set_surrounding("history", 7, 7);
        harness.host().set_sensitive(sensitive);
        harness.type("hello");
        RAWKEY_ASSERT(service.calls.load() == 0);
        harness.type("su3"); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "hello擬");
        RAWKEY_ASSERT(service.contexts().back() == (sensitive ? u"hello" : u"historyhello"));
    }
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.set_surrounding("old", 3, 3);
        harness.type("su3"); wait_request(harness, service);
        harness.set_surrounding("new", 3, 3);
        harness.type("c");
        service.release(); settled(harness);
        RAWKEY_ASSERT(service.contexts().back() == u"new");
        RAWKEY_ASSERT(harness.preedit().starts_with("擬"));
    }
    for (const int invalid : {1, 2, 3}) {
        ScriptService service(false, invalid);
        Harness harness(options(service));
        harness.type("su3");
        const auto fallback = harness.preedit();
        settled(harness);
        RAWKEY_ASSERT(harness.preedit() == fallback);
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == fallback);
    }
    {
        ScriptService service(false, 4);
        Harness harness(options(service));
        harness.type("su3"); settled(harness);
        RAWKEY_ASSERT(service.calls.load() == 2);
        RAWKEY_ASSERT(harness.preedit() == "擬");
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "擬");
    }
    {
        ScriptService service;
        Harness harness(options(service));
        harness.set_config("BopomofoKeyboardLayout", "許氏");
        harness.type("nef"); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "擬");
        harness.type("arious"); settled(harness);
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "nefarious");
    }
    {
        ScriptService service;
        Harness harness(options(service));
        harness.type(",4su3"); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "誒擬");
        RAWKEY_ASSERT(service.contexts().back() == u"誒");
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == "誒擬");
    }
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.type("su3"); wait_request(harness, service);
        harness.set_config("SmartModelPreview", "False");
        const auto baseline = harness.preedit();
        service.release(); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == baseline);
        RAWKEY_ASSERT(!harness.config().smart_model_preview);
    }
    {
        auto value = smart_options();
        value.config.smart_model_preview = true;
        Harness harness(value);  // No service: retain and submit fallback.
        harness.type("su3");
        const auto fallback = harness.preedit();
        settled(harness);
        harness.key("Return");
        RAWKEY_ASSERT(harness.last_commit() == fallback);
    }
}

RAWKEY_SUITE("smart English automatically enables live model despite legacy preview setting", production_single_smart_switch) {
    ScriptService service;
    auto value = smart_options();
    value.config = config_from_json({{"smart_english", true}, {"smart_model_preview", false}});
    value.socket_path = service.socket.string();
    Harness harness(value);
    RAWKEY_ASSERT(harness.config().smart_model_preview);
    RAWKEY_ASSERT(!to_json(harness.config()).contains("smart_model_preview"));
    const auto settled = [&] {
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
    };
    harness.type("su3");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "擬");
    RAWKEY_ASSERT(harness.pending_model_requests() != 0);
    harness.expect_commit("擬");
    harness.set_config("SmartEnglish", "False");
    harness.type("su");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    RAWKEY_ASSERT(harness.pending_model_requests() == 0);
    harness.reset();
    harness.set_config("SmartEnglish", "True");
    harness.type("su3");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "擬");
    harness.expect_commit("擬");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("production mixed model Chinese deletion and reordered readings", production_edit_models) {
    const auto options = [](const ScriptService& service) {
        auto value = smart_options();
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        return value;
    };
    const auto settled = [](Harness& harness) {
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
    };
    {
        ScriptService service(true);
        Harness harness(options(service));
        harness.type("su3"); wait_request(harness, service);
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.composition_empty());
        service.release(); settled(harness);
        RAWKEY_ASSERT(harness.composition_empty());
        RAWKEY_ASSERT(harness.commits().empty());
    }
    {
        ScriptService service;
        Harness harness(options(service));
        harness.type("su3cl3"); settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "擬好");
        harness.key("BackSpace");
        RAWKEY_ASSERT(harness.preedit() == "擬");
        settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "擬");
        harness.expect_commit("擬");
    }
    for (const auto& [layout, keys] : std::vector<std::pair<std::string, std::string>>{
        {"standard", "us3"}, {"hsu", "enf"}, {"et26", "enj"}}) {
        ScriptService service;
        auto value = options(service);
        value.config.keyboard_layout = layout;
        Harness harness(value);
        harness.type(keys); settled(harness);
        // Compact inputs can also mean ㄧㄣˇ; they retain both interpretations.
        harness.key("Down");
        harness.choose_text("你");
        settled(harness);
        RAWKEY_ASSERT(harness.preedit() == "你");
        harness.expect_commit("你");
    }
}

RAWKEY_SUITE("production mixed model unresolved keys remain raw and late answers stay local", production_error_island_models) {
    ScriptService service(true);
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.socket_path = service.socket.string();
    Harness harness(value);
    harness.type("su3cl3");
    wait_request(harness, service);
    harness.type("sss3su3cl3");
    RAWKEY_ASSERT(harness.preedit() == "你好sss3你好");
    service.release();
    RAWKEY_ASSERT(harness.pump_until([&] {
        return harness.pending_model_idle() && !harness.session()->prediction.pending;
    }));
    const auto shown = harness.preedit();
    RAWKEY_ASSERT(shown.find("sss3") != std::string::npos);
    RAWKEY_ASSERT(shown.starts_with("擬好"));
    const auto& decision = harness.session()->mixed_decision;
    RAWKEY_ASSERT(std::ranges::any_of(decision.result.paths.at(decision.preview_path).segments, [](const auto& segment) {
        return segment.kind == MixedSegmentKind::BopomofoUnresolved && segment.raw == u"sss3" &&
               segment.reading.empty() && segment.candidates.empty();
    }));
    harness.expect_commit(shown);
}

RAWKEY_SUITE("production smart cursor model uses only the prefix and preserves the suffix", production_cursor_models) {
    for (const auto action : {"settle", "select", "commit", "undo"}) {
        ScriptService service(true);
        auto value = smart_options();
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.set_surrounding("history", 7, 7);
        harness.type("su3cl3");
        wait_request(harness, service);
        harness.key("Down");
        harness.choose_text("你好");
        harness.key("Left");
        harness.type("hellosu3cl3");
        RAWKEY_ASSERT(harness.preedit() == "你hello你好好");
        if (std::string_view(action) == "select") {
            harness.key("Down");
            harness.choose_text("hello你好");
        } else if (std::string_view(action) == "commit") {
            harness.expect_commit("你hello擬好好");
        } else if (std::string_view(action) == "undo") {
            for (int i = 0; i < 6; ++i) harness.key("Shift+BackSpace");
            RAWKEY_ASSERT(harness.preedit() == "你hello好");
        }
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        if (std::string_view(action) == "settle") {
            RAWKEY_ASSERT(harness.preedit() == "你hello擬好好");
            const auto contexts = service.contexts();
            RAWKEY_ASSERT(std::ranges::find(contexts, u"history你hello") != contexts.end());
            RAWKEY_ASSERT(std::ranges::find(contexts, u"history你好hello") == contexts.end());
            harness.key("Right");
            RAWKEY_ASSERT(harness.session()->pending_token.empty());
            RAWKEY_ASSERT(harness.preedit() == "你hello擬好好");
            harness.expect_commit("你hello擬好好");
        } else if (std::string_view(action) == "select") {
            RAWKEY_ASSERT(harness.preedit() == "你hello你好好");
            harness.expect_commit("你hello你好好");
        } else if (std::string_view(action) == "commit") {
            RAWKEY_ASSERT(harness.composition_empty());
            RAWKEY_ASSERT(harness.last_commit() == "你hello擬好好");
        } else {
            RAWKEY_ASSERT(harness.preedit() == "你hello好");
            harness.expect_commit("你hello好");
        }
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production smart cursor service errors preserve settled word choices", production_cursor_service_failure) {
    ScriptService service(false, 3);
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.socket_path = service.socket.string();
    Harness harness(value);
    const auto settled = [&] {
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
    };
    harness.type("xu/4j94vu04y94appao6u.3wj61ul ");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有圖標");
    harness.key("Left");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有圖標");
    harness.key("Left");
    harness.type("hellosu3cl3");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有hello你好圖標");
    harness.key("Right");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "另外現在app沒有hello你好圖標");
    harness.expect_commit("另外現在app沒有hello你好圖標");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("production smart cursor late replies cannot cross a focus boundary", production_cursor_focus_boundary) {
    ScriptService service(true);
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.socket_path = service.socket.string();
    Harness harness(value);
    harness.set_surrounding("你🙂", 3, 3);
    harness.type("su3cl3");
    wait_request(harness, service);
    harness.key("Left");
    harness.type("hellosu3cl3");
    harness.expect_focus_out_commit("你hello你好好");
    const auto original = harness.host().commits();
    RAWKEY_ASSERT(original.size() == 1 && original.front().first == 1);
    harness.use_context(2);
    harness.set_surrounding("", 0, 0);
    harness.type("world");
    service.release();
    RAWKEY_ASSERT(harness.pump_until([&] {
        const auto* old = harness.engine().session(1);
        return harness.pending_model_idle() && harness.engine().pending_model_idle(1) &&
               old != nullptr && !old->prediction.pending;
    }));
    RAWKEY_ASSERT(harness.preedit() == "world");
    RAWKEY_ASSERT(harness.context_text().empty());
    RAWKEY_ASSERT(harness.host().commits() == original);
    harness.expect_commit("world");
    const auto commits = harness.host().commits();
    RAWKEY_ASSERT(commits.size() == 2 && commits.back().first == 2);
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("production isolated tones remain raw and do not request phantom readings", production_orphan_tones) {
    ScriptService service;
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.config.keyboard_layout = "ibm";
    value.socket_path = service.socket.string();
    Harness harness(value);
    harness.type(".");
    RAWKEY_ASSERT(harness.preedit() == ".");
    RAWKEY_ASSERT(harness.pending_model_requests() == 0);
    RAWKEY_ASSERT(!harness.session()->buffer.has_unfinished_reading());
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.composition_empty());
    harness.type("7a,");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
    RAWKEY_ASSERT(harness.preedit() == "擬");
    RAWKEY_ASSERT(harness.pending_model_requests() != 0);
    harness.expect_commit("擬");
}

RAWKEY_SUITE("internal preview comparison survives keyboard layout changes", production_preview_comparison_control) {
    for (const auto& [layout, keys] : std::array{
        std::pair{"standard", "su3cl3"}, std::pair{"hsu", "nefhwf"}}) {
        for (const bool enabled : {false, true}) {
            ScriptService service;
            auto value = smart_options();
            value.config.smart_model_preview = enabled;
            value.socket_path = service.socket.string();
            Harness harness(value);
            harness.set_config("BopomofoKeyboardLayout", layout);
            RAWKEY_ASSERT(harness.config().smart_model_preview == enabled);
            harness.type(keys);
            RAWKEY_ASSERT(harness.pump_until([&] { return harness.pending_model_idle(); }));
            RAWKEY_ASSERT(harness.preedit() == (enabled ? "擬好" : "你好"));
            RAWKEY_ASSERT((harness.pending_model_requests() != 0) == enabled);
        }
    }
}

RAWKEY_SUITE("production mixed model long errors preserve prefix and reject late replacement", production_long_error_models) {
    for (const bool choose_raw : {false, true}) {
        ScriptService service(true);
        auto value = smart_options();
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.type("su3cl3");
        wait_request(harness, service);
        const auto wrong = std::string(24, 's');
        harness.type(wrong);
        RAWKEY_ASSERT(harness.preedit() == "你好" + wrong);
        if (choose_raw) {
            harness.key("Down");
            harness.choose_text("su3cl3" + wrong);
        }
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        if (choose_raw) {
            harness.expect_commit("su3cl3" + wrong);
        } else {
            RAWKEY_ASSERT(harness.preedit() == "擬好" + wrong);
            for (size_t i = 0; i < wrong.size(); ++i) harness.key("BackSpace");
            RAWKEY_ASSERT(harness.preedit() == "擬好");
            harness.expect_commit("擬好");
        }
        RAWKEY_ASSERT(service.ok.load());
    }
}

RAWKEY_SUITE("production mixed model keeps lexical islands and valid interjections", production_lexical_island_models) {
    ScriptService service(true);
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.socket_path = service.socket.string();
    Harness harness(value);
    harness.type("su3cl3");
    wait_request(harness, service);
    harness.type("appao6o4");
    RAWKEY_ASSERT(harness.preedit() == "你好app沒欸");
    service.release();
    RAWKEY_ASSERT(harness.pump_until([&] {
        return harness.pending_model_idle() && !harness.session()->prediction.pending;
    }));
    const auto& decision = harness.session()->mixed_decision;
    const auto& segments = decision.result.paths.at(decision.preview_path).segments;
    RAWKEY_ASSERT(std::ranges::any_of(segments, [](const auto& segment) { return segment.reading == u"ㄇㄟˊ"; }));
    RAWKEY_ASSERT(std::ranges::any_of(segments, [](const auto& segment) { return segment.reading == u"ㄟˋ"; }));
    RAWKEY_ASSERT(std::ranges::none_of(segments, [](const auto& segment) {
        return segment.kind == MixedSegmentKind::BopomofoUnresolved;
    }));
    const auto shown = harness.preedit();
    RAWKEY_ASSERT(shown.starts_with("擬好app"));
    RAWKEY_ASSERT(shown.find("ao6") == std::string::npos && shown.find("o4") == std::string::npos);
    harness.expect_commit(shown);
}

RAWKEY_SUITE("production model preserves explicit rare singleton choices", production_singleton_manual_choices) {
    ScriptService service;
    auto value = smart_options();
    value.config.smart_model_preview = true;
    value.socket_path = service.socket.string();
    Harness harness(value);
    const auto settled = [&] {
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
    };
    harness.type("j ");
    settled();
    harness.key("Down");
    harness.choose_text("巫");
    settled();
    harness.type("g ");
    settled();
    harness.key("Down");
    harness.choose_text("師");
    settled();
    harness.type("n0 ");
    settled();
    RAWKEY_ASSERT(harness.preedit() == "巫師三");
    RAWKEY_ASSERT(service.calls.load() != 0);
    harness.expect_commit("巫師三");
    RAWKEY_ASSERT(harness.last_commit() == "巫師三");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("production lexical reading repair rejects stale replies and preserves raw choices", production_singleton_repair) {
    for (const std::string action : {"settle", "Return", "raw", "BackSpace", "panel"}) {
        ScriptService service(true, 0, false, false, true);
        auto value = smart_options();
        value.config.smart_model_preview = true;
        value.socket_path = service.socket.string();
        Harness harness(value);
        harness.type("j g n0 ");
        RAWKEY_ASSERT(harness.preedit() == "巫師三");
        wait_request(harness, service);
        std::vector<std::string> panel;
        if (action == "Return") harness.expect_commit("巫師三");
        if (action == "raw") {
            harness.key("Down");
            harness.choose_text("j g n0 ");
        }
        if (action == "BackSpace") harness.key("BackSpace");
        if (action == "panel") { harness.key("Down"); panel = harness.candidates(); }
        service.release();
        RAWKEY_ASSERT(harness.pump_until([&] {
            return harness.pending_model_idle() && !harness.session()->prediction.pending;
        }));
        if (action == "Return") {
            RAWKEY_ASSERT(harness.composition_empty());
            RAWKEY_ASSERT(harness.last_commit() == "巫師三");
        } else if (action == "raw") {
            RAWKEY_ASSERT(harness.preedit() == "j g n0 ");
            harness.expect_commit("j g n0 ");
        } else if (action == "BackSpace") {
            RAWKEY_ASSERT(harness.preedit() == "巫師");
            harness.expect_commit("巫師");
        } else {
            RAWKEY_ASSERT(harness.preedit() == "巫師三");
            if (action == "panel") RAWKEY_ASSERT(harness.candidates() == panel);
            harness.expect_commit("巫師三");
        }
        RAWKEY_ASSERT(service.ok.load());
    }
}

void wait_request(Harness& harness, ScriptService& service) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (service.calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        (void)harness.pump_until([&] { return service.calls.load() != 0; }, std::chrono::milliseconds(10));
    }
    RAWKEY_ASSERT(service.calls.load() != 0);
}

}  // namespace

RAWKEY_SUITE("stale buffer prediction retains preview", stale_buffer_retains_preview) {
    ScriptService service(false, 0, true);
    auto value = smart_options();
    value.config.smart_english = false;
    value.socket_path = service.socket.string();
    Harness harness(value);
    harness.type("su3");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.preedit() == "擬"; }));
    harness.type("cl3");
    RAWKEY_ASSERT(harness.pump_until([&] { return service.calls.load() >= 2; }));
    harness.key("Control+,");
    RAWKEY_ASSERT(harness.preedit() == "擬好，");
    service.release_one();
    RAWKEY_ASSERT(harness.pump_until([&] { return !harness.session()->prediction.pending; }));
    // Punctuation changed the buffer revision. Its stale response must leave
    // the already-refined first character intact without requesting new keys.
    RAWKEY_ASSERT(harness.preedit() == "擬好，");
    service.release();
    RAWKEY_ASSERT(harness.pump_until([&] { return !harness.session()->prediction.pending; }));
    harness.key("Return");
    RAWKEY_ASSERT(harness.last_commit() == "擬好，");
}

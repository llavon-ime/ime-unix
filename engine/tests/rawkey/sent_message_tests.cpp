#include "raw_key_harness.hpp"

#include "engine/fallback_engine.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>
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

// Answers predictions from the Bopomofo table and records the context of every
// request, so a test can see what the engine would have sent the model.
class RecordingService {
public:
    RecordingService() {
        static std::size_t serial = 0;
        socket = std::filesystem::temp_directory_path() /
                 ("llavon-ime-sent-message-" + std::to_string(getpid()) + "-" + std::to_string(++serial) + ".sock");
        server_.bind_listen(socket);
        worker_ = std::thread([this] { serve(); });
    }
    ~RecordingService() {
        if (calls.load() == 0) {
            try { (void)UnixSocketClient{}.connect(socket); } catch (...) {}
        }
        if (worker_.joinable()) worker_.join();
    }

    std::vector<std::u16string> contexts() {
        std::lock_guard<std::mutex> lock(mutex_);
        return contexts_;
    }

    std::filesystem::path socket;
    std::atomic<std::size_t> calls{0};
    std::atomic<bool> ok{true};

private:
    void serve() {
        try {
            auto connection = server_.accept_one();
            protocol::ServiceEpoch epoch{};
            epoch[0] = 0x41;
            protocol::SessionId session{};
            session[0] = 0x11;
            const FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
            for (;;) {
                const auto message = receive(connection);
                if (std::holds_alternative<protocol::StatusRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::Message{
                        protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
                } else if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                    connection.send_all(protocol::encode(
                        protocol::Message{protocol::OpenSessionResponse{session, epoch}}));
                } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        contexts_.push_back(request->context);
                    }
                    ++calls;
                    protocol::Prediction response{request->session_id, request->request_id,
                                                  request->buffer_revision, {}};
                    for (const auto& entry : request->padding) {
                        auto candidates = fallback.lookup(entry.bopomofo());
                        if (entry.chosen()) candidates = {entry.chosen_char()};
                        response.candidates.push_back(std::move(candidates));
                    }
                    connection.send_all(protocol::encode(protocol::Message{response}));
                } else if (const auto* close = std::get_if<protocol::CloseSessionRequest>(&message)) {
                    connection.send_all(protocol::encode(
                        protocol::Message{protocol::CloseSessionResponse{close->session_id}}));
                } else if (const auto* record = std::get_if<protocol::RecordCommitRequest>(&message)) {
                    connection.send_all(protocol::encode(
                        protocol::Message{protocol::RecordCommitResponse{record->event_id, true}}));
                } else if (const auto* discard = std::get_if<protocol::DiscardCommitRequest>(&message)) {
                    connection.send_all(protocol::encode(
                        protocol::Message{protocol::DiscardCommitResponse{discard->event_id, true}}));
                } else {
                    ok = false;
                    break;
                }
            }
        } catch (...) {
            // The harness closes the socket when it shuts down.
        }
    }

    UnixSocketServer server_;
    std::thread worker_;
    std::mutex mutex_;
    std::vector<std::u16string> contexts_;
};

void settle(Harness& harness) {
    RAWKEY_ASSERT(harness.pump_until([&] {
        return !harness.session()->prediction.pending && harness.pending_model_idle();
    }));
}

HarnessOptions options_for(const RecordingService& service) {
    HarnessOptions options;
    options.socket_path = service.socket.string();
    return options;
}

// Types one reading and returns the context of its final prediction request.
std::u16string predict_and_get_context(Harness& harness, RecordingService& service, std::string_view keys) {
    const auto before = service.calls.load();
    harness.type(keys);
    settle(harness);
    RAWKEY_ASSERT(service.calls.load() > before);
    const auto contexts = service.contexts();
    RAWKEY_ASSERT(!contexts.empty());
    return contexts.back();
}

}  // namespace

RAWKEY_SUITE("a sent message stands in for the cleared field", sent_message_context) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);

    // Send 「我」. The chat client clears the field afterwards.
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    harness.set_surrounding("", 0, 0);
    RAWKEY_ASSERT(harness.context_text().empty());

    // The reply follows the message on a new line, not as its continuation.
    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3") == u"我\n");
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("sent messages stay in front of the message being written", sent_message_while_writing) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);

    // Send 「我」, then commit 「你」 into the cleared field.
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    harness.set_surrounding("", 0, 0);
    harness.type("su3");
    settle(harness);
    harness.expect_commit("你");
    harness.set_surrounding("你", 1, 1);

    // The field only holds 「你」; the sent message still comes first.
    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3") == u"我\n你");
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("several sent messages accumulate", sent_message_accumulates) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    harness.set_surrounding("", 0, 0);
    // Seeing the cleared field is what turns the commit into a sent message.
    harness.type("su3");
    settle(harness);
    harness.expect_commit("你");
    harness.set_surrounding("", 0, 0);

    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3") == u"我\n你\n");
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("a field that keeps its text gets no history", sent_message_document) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    // A document keeps the commit; repeating it as history would duplicate it.
    harness.set_surrounding("我", 1, 1);
    harness.type("su3");
    settle(harness);
    harness.expect_commit("你");
    harness.set_surrounding("我你", 2, 2);

    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3") == u"我你");
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("a sent message does not follow the user to another input context", sent_message_other_context) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");

    // Another field in the same program must not inherit the message.
    harness.use_context(2);
    harness.set_surrounding("", 0, 0);
    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3").empty());
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("an expired sent message is not context", sent_message_expired) {
    RecordingService service;
    auto options = options_for(service);
    options.sent_message_context_window = std::chrono::seconds{0};
    Harness harness(options);
    harness.set_surrounding("", 0, 0);
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    harness.set_surrounding("", 0, 0);

    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3").empty());
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

RAWKEY_SUITE("a withdrawn sent message is not context", sent_message_withdrawn) {
    RecordingService service;
    Harness harness(options_for(service));
    harness.set_surrounding("", 0, 0);
    harness.type("ji3");
    settle(harness);
    harness.expect_commit("我");
    // The application inserts the committed text; Backspace withdraws it.
    harness.set_surrounding("我", 1, 1);
    harness.key("BackSpace");
    RAWKEY_ASSERT(harness.last_commit() == "我");
    harness.set_surrounding("", 0, 0);

    RAWKEY_ASSERT(predict_and_get_context(harness, service, "ji3").empty());
    harness.key("Escape");
    RAWKEY_ASSERT(service.ok.load());
}

#include "raw_key_harness.hpp"

#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {

protocol::ByteVector receive_frame(const UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

}  // namespace

// A scripted prediction service: the harness points the transport at this
// socket, types a key sequence, and the model answer must reach the candidate
// list. This is how tests exercise the service path without a real model.
RAWKEY_SUITE("prediction integration", prediction_integration) {
    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("llavon-ime-rawkey-prediction-" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket_path);

    UnixSocketServer server;
    server.bind_listen(socket_path);
    std::atomic<bool> server_ok{true};
    std::atomic<int> server_stage{0};
    std::thread server_thread([&] {
        try {
            auto connection = server.accept_one();
            server_stage = 1;
            protocol::ServiceEpoch epoch{};
            epoch[0] = 0x53;
            protocol::SessionId session_id{};
            session_id[0] = 0x27;

            const auto status = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{
                protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open)) server_ok = false;
            connection.send_all(protocol::encode(
                protocol::Message{protocol::OpenSessionResponse{session_id, epoch}}));

            const auto predict_message = protocol::decode(receive_frame(connection));
            const auto* predict = std::get_if<protocol::PredictRequest>(&predict_message);
            if (predict == nullptr) {
                server_ok = false;
                return;
            }
            if (predict->context != u"history") {
                server_ok = false;
                return;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::Prediction{session_id, predict->request_id, predict->buffer_revision, {{U'擬'}}}}));
            server_stage = 2;

            const auto close_message = protocol::decode(receive_frame(connection));
            const auto* close = std::get_if<protocol::CloseSessionRequest>(&close_message);
            if (close == nullptr || close->session_id != session_id) {
                server_ok = false;
                return;
            }
            connection.send_all(
                protocol::encode(protocol::Message{protocol::CloseSessionResponse{session_id}}));
            server_stage = 3;
        } catch (const std::exception&) {
            server_ok = false;
        } catch (...) {
            server_ok = false;
        }
    });

    bool prediction_visible = false;
    {
        HarnessOptions options;
        options.socket_path = socket_path.string();
        Harness harness(options);
        harness.set_config("SmartEnglish", "False");
        harness.set_surrounding("history", 7, 7);
        harness.type("su3");
        prediction_visible = harness.pump_until(
            [&] {
                auto* session = harness.session();
                if (session == nullptr) return false;
                const auto* candidates = session->buffer.segment_candidates(0);
                return candidates != nullptr && !candidates->empty() && candidates->front() == U'擬';
            },
            std::chrono::seconds(10));
        // Detaching closes the prediction session; wait for the service to see
        // it before the transport is torn down.
        harness.detach();
        RAWKEY_ASSERT(harness.pump_until([&] { return server_stage.load() == 3; },
                                         std::chrono::seconds(2)));
    }

    if (server_stage.load() == 0) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket_path);
        } catch (...) {
        }
    }
    server_thread.join();
    std::error_code error;
    std::filesystem::remove(socket_path, error);

    RAWKEY_ASSERT(prediction_visible);
    RAWKEY_ASSERT(server_ok.load());
    RAWKEY_ASSERT(server_stage.load() == 3);
}

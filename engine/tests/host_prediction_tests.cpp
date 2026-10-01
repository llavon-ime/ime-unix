#include "test_suites.h"

#ifndef _WIN32

#include "fake_host.hpp"

#include "host/engine.hpp"
#include "input/input_key.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"
#include "text/utf.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace llavon::ime;
using llavon::ime::test::FakeHost;

[[noreturn]] void fail(const char* step) {
    std::fprintf(stderr, "host prediction test failed: %s\n", step);
    std::exit(EXIT_FAILURE);
}

protocol::ByteVector receive_frame(const UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

std::filesystem::path test_path(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("llavon-ime-host-prediction-" + std::string(name) + "-" + std::to_string(getpid()) + ".sock");
}

EngineOptions test_options(const std::filesystem::path& socket_path,
                           const std::filesystem::path& overrides_path) {
    EngineOptions options;
    options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    options.phrase_overrides_path = overrides_path;
    options.config = default_config();
    options.config.smart_english = false;
    options.enable_accessibility = false;
    options.transport.socket_path = socket_path;
    options.transport.auto_start = false;
    return options;
}

void type(Engine& engine, ContextId context, std::u16string_view text) {
    for (const char16_t ch : text) {
        InputKey key;
        key.sym = ch;
        (void)engine.key_event(context, key);
    }
}

void set_surrounding(FakeHost& host, std::u16string text) {
    HostContext surrounding;
    surrounding.valid = true;
    surrounding.cursor = text.size();
    surrounding.anchor = text.size();
    surrounding.text = std::move(text);
    host.set_surrounding(std::move(surrounding));
}

void unblock_accept(const std::filesystem::path& socket_path) {
    try {
        UnixSocketClient client;
        (void)client.connect(socket_path);
    } catch (...) {
    }
}

// Models a failed prediction followed by a dirty retry and a successful
// prediction, then the session close on detach.
bool test_prediction_retry_and_close() {
    const auto socket_path = test_path("retry");
    const auto overrides_path = socket_path.parent_path() / (socket_path.stem().string() + "-overrides.txt");
    std::error_code error;
    std::filesystem::remove(socket_path, error);

    UnixSocketServer server;
    server.bind_listen(socket_path);

    protocol::ServiceEpoch epoch{};
    epoch[0] = 0x42;
    protocol::SessionId session_id{};
    session_id[0] = 0x19;
    std::atomic<bool> server_ok = true;
    std::atomic<bool> close_received = false;
    std::mutex close_mutex;
    std::condition_variable close_condition;

    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            const auto status = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status)) server_ok = false;
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open)) server_ok = false;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{session_id, epoch}}));

            const auto first_message = protocol::decode(receive_frame(connection));
            const auto* first = std::get_if<protocol::PredictRequest>(&first_message);
            if (first == nullptr || first->request_id != 1 || first->padding.size() != 2) {
                server_ok = false;
                return;
            }
            if (first->context != u"history") server_ok = false;
            protocol::Error model_error;
            model_error.code = protocol::ErrorCode::ModelError;
            model_error.session_id = session_id;
            model_error.request_id = first->request_id;
            model_error.buffer_revision = first->buffer_revision;
            connection.send_all(protocol::encode(protocol::Message{model_error}));

            const auto second_message = protocol::decode(receive_frame(connection));
            const auto* second = std::get_if<protocol::PredictRequest>(&second_message);
            if (second == nullptr || second->request_id != 2 || second->padding.size() != 2) {
                server_ok = false;
                return;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::Prediction{session_id, second->request_id, second->buffer_revision, {{U'擬'}, {U'你'}}}}));

            const auto close_message = protocol::decode(receive_frame(connection));
            const auto* close = std::get_if<protocol::CloseSessionRequest>(&close_message);
            if (close == nullptr || close->session_id != session_id) {
                server_ok = false;
                return;
            }
            connection.send_all(
                protocol::encode(protocol::Message{protocol::CloseSessionResponse{session_id}}));
            {
                std::lock_guard lock(close_mutex);
                close_received = true;
            }
            close_condition.notify_all();
        } catch (...) {
            server_ok = false;
        }
    });

    bool passed = false;
    {
        FakeHost host;
        set_surrounding(host, u"history");
        Engine engine(test_options(socket_path, overrides_path), host);
        const ContextId context = 21;
        engine.attach(context);
        type(engine, context, u"su3su3");

        passed = host.pump_until([&]() {
            const auto* session = engine.session(context);
            return session != nullptr && !session->prediction.pending &&
                   session->prediction.next_request_id >= 3;
        });
        if (passed) {
            const auto* session = engine.session(context);
            const auto* first_candidates = session == nullptr ? nullptr : session->buffer.segment_candidates(0);
            const auto* second_candidates = session == nullptr ? nullptr : session->buffer.segment_candidates(1);
            passed = first_candidates != nullptr && !first_candidates->empty() &&
                     first_candidates->front() == U'擬' && second_candidates != nullptr &&
                     !second_candidates->empty() && second_candidates->front() == U'你';
        }
        engine.detach(context);
        {
            std::unique_lock lock(close_mutex);
            passed = passed &&
                     close_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return close_received.load(); });
        }
    }

    if (!close_received) unblock_accept(socket_path);
    server_thread.join();
    std::filesystem::remove(socket_path, error);
    std::filesystem::remove(overrides_path, error);
    return passed && server_ok;
}

// A prediction that reports UnknownSession must clear the session and open a
// replacement on the next request.
bool test_unknown_session_reopens() {
    const auto socket_path = test_path("unknown-session");
    const auto overrides_path = socket_path.parent_path() / (socket_path.stem().string() + "-overrides.txt");
    std::error_code error;
    std::filesystem::remove(socket_path, error);

    UnixSocketServer server;
    server.bind_listen(socket_path);

    protocol::ServiceEpoch epoch{};
    protocol::SessionId first_id{};
    first_id[0] = 0x11;
    protocol::SessionId second_id{};
    second_id[0] = 0x22;
    std::atomic<bool> server_ok = true;
    std::atomic<bool> close_received = false;
    std::mutex close_mutex;
    std::condition_variable close_condition;

    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            (void)protocol::decode(receive_frame(connection));
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto first_open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(first_open)) server_ok = false;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{first_id, epoch}}));

            const auto first_message = protocol::decode(receive_frame(connection));
            const auto* first = std::get_if<protocol::PredictRequest>(&first_message);
            if (first == nullptr || first->request_id != 1 || first->padding.size() != 1) {
                server_ok = false;
                return;
            }
            protocol::Error unknown;
            unknown.code = protocol::ErrorCode::UnknownSession;
            unknown.session_id = first_id;
            unknown.request_id = first->request_id;
            unknown.buffer_revision = first->buffer_revision;
            connection.send_all(protocol::encode(protocol::Message{unknown}));

            const auto second_open = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(second_open)) server_ok = false;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{second_id, epoch}}));

            const auto second_message = protocol::decode(receive_frame(connection));
            const auto* second = std::get_if<protocol::PredictRequest>(&second_message);
            if (second == nullptr || second->request_id != 2 || second->padding.size() != 2 ||
                second->session_id != second_id) {
                server_ok = false;
                return;
            }
            connection.send_all(protocol::encode(protocol::Message{
                protocol::Prediction{second_id, second->request_id, second->buffer_revision, {{U'擬'}, {U'你'}}}}));

            const auto close_message = protocol::decode(receive_frame(connection));
            const auto* close = std::get_if<protocol::CloseSessionRequest>(&close_message);
            if (close == nullptr || close->session_id != second_id) {
                server_ok = false;
                return;
            }
            connection.send_all(
                protocol::encode(protocol::Message{protocol::CloseSessionResponse{second_id}}));
            {
                std::lock_guard lock(close_mutex);
                close_received = true;
            }
            close_condition.notify_all();
        } catch (...) {
            server_ok = false;
        }
    });

    bool passed = false;
    {
        FakeHost host;
        Engine engine(test_options(socket_path, overrides_path), host);
        const ContextId context = 31;
        engine.attach(context);

        type(engine, context, u"su3");
        passed = host.pump_until([&]() {
            const auto* session = engine.session(context);
            return session != nullptr && !session->prediction.session_open() &&
                   !session->prediction.pending && session->prediction.next_request_id >= 2;
        });
        if (passed) {
            type(engine, context, u"cl3");
            passed = host.pump_until([&]() {
                const auto* session = engine.session(context);
                return session != nullptr && session->prediction.session_open() &&
                       session->prediction.session_id == second_id &&
                       !session->prediction.pending && session->prediction.next_request_id >= 3;
            });
        }
        if (passed) {
            const auto* session = engine.session(context);
            const auto* candidates = session == nullptr ? nullptr : session->buffer.segment_candidates(0);
            passed = session != nullptr && candidates != nullptr && !candidates->empty() &&
                     candidates->front() == U'擬';
        }
        engine.detach(context);
        {
            std::unique_lock lock(close_mutex);
            passed = passed &&
                     close_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return close_received.load(); });
        }
    }

    if (!close_received) unblock_accept(socket_path);
    server_thread.join();
    std::filesystem::remove(socket_path, error);
    std::filesystem::remove(overrides_path, error);
    return passed && server_ok;
}

// Destroying the engine while a request is in flight must not run callbacks
// into the dead engine and must unblock the service connection.
bool test_destroy_with_inflight_request() {
    const auto socket_path = test_path("lifetime");
    const auto overrides_path = socket_path.parent_path() / (socket_path.stem().string() + "-overrides.txt");
    std::error_code error;
    std::filesystem::remove(socket_path, error);

    UnixSocketServer server;
    server.bind_listen(socket_path);

    std::atomic<bool> server_done = false;
    std::mutex mutex;
    std::condition_variable condition;

    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            (void)protocol::decode(receive_frame(connection));
            protocol::ServiceEpoch epoch{};
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
            (void)protocol::decode(receive_frame(connection));
            protocol::SessionId session_id{};
            session_id[0] = 0x33;
            connection.send_all(
                protocol::encode(protocol::Message{protocol::OpenSessionResponse{session_id, epoch}}));
            // Hold the prediction open until the client disconnects.
            (void)connection.recv_exact(1);
        } catch (...) {
        }
        {
            std::lock_guard lock(mutex);
            server_done = true;
        }
        condition.notify_all();
    });

    {
        FakeHost host;
        Engine engine(test_options(socket_path, overrides_path), host);
        const ContextId context = 51;
        engine.attach(context);
        type(engine, context, u"su3");
        // Let the open-session response arrive; the prediction stays in flight
        // while the engine is destroyed at the end of this scope.
        (void)host.pump_until([&]() { return host.redraw_count() > 0; }, std::chrono::milliseconds(200));
    }

    {
        std::unique_lock lock(mutex);
        (void)condition.wait_for(lock, std::chrono::seconds(2), [&]() { return server_done.load(); });
    }
    if (!server_done) unblock_accept(socket_path);
    server_thread.join();
    std::filesystem::remove(socket_path, error);
    std::filesystem::remove(overrides_path, error);
    return server_done;
}

}  // namespace

int run_host_prediction_tests() {
    if (!test_prediction_retry_and_close()) fail("retry and close");
    if (!test_unknown_session_reopens()) fail("unknown session");
    if (!test_destroy_with_inflight_request()) fail("destroy with inflight");
    return EXIT_SUCCESS;
}

#else

int run_host_prediction_tests() { return EXIT_SUCCESS; }

#endif

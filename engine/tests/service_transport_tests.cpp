#include "test_suites.h"

#ifndef _WIN32

#include "engine/service_transport.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

llavon::ime::protocol::ByteVector receive_frame(const llavon::ime::UnixSocketConnection& connection) {
    auto header = connection.recv_exact(4);
    std::uint32_t length = 0;
    std::memcpy(&length, header.data(), sizeof(length));
    auto payload = connection.recv_exact(length);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

}  // namespace

int run_service_transport_tests() {
    using namespace llavon::ime;
    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("llavon-ime-transport-test-" + std::to_string(getpid()) + ".sock");
    std::error_code error;
    std::filesystem::remove(socket_path, error);
    UnixSocketServer server;
    server.bind_listen(socket_path);

    protocol::ServiceEpoch epoch{};
    epoch[0] = 0x42;
    protocol::SessionId session{};
    session[0] = 0x19;
    bool server_ok = true;
    std::thread server_thread([&]() {
        try {
            auto connection = server.accept_one();
            const auto status_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status_request)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));

            const auto open_request = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::OpenSessionRequest>(open_request)) server_ok = false;
            connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));

            const auto predict_request = protocol::decode(receive_frame(connection));
            const auto* request = std::get_if<protocol::PredictRequest>(&predict_request);
            if (request == nullptr || request->session_id != session || request->request_id != 1) {
                server_ok = false;
            } else {
                connection.send_all(protocol::encode(protocol::Message{protocol::Prediction{session, 1, request->buffer_revision, {{U'你'}}}}));
            }

            auto shutdown_connection = server.accept_one();
            const auto shutdown_request = protocol::decode(receive_frame(shutdown_connection));
            if (!std::holds_alternative<protocol::ShutdownRequest>(shutdown_request)) server_ok = false;
        } catch (...) {
            server_ok = false;
        }
    });

    ServiceTransportOptions options;
    options.socket_path = socket_path;
    ServiceTransport transport(options);
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    std::vector<protocol::Message> responses;
    transport.open_session([&](protocol::Message response) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        if (!callback_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return responses.size() >= 1; })) {
            transport.stop();
            server_thread.join();
            return EXIT_FAILURE;
        }
    }
    if (!std::holds_alternative<protocol::OpenSessionResponse>(responses.front())) {
        transport.stop();
        server_thread.join();
        return EXIT_FAILURE;
    }
    transport.predict(session, 1, 7, {}, {{false, u"ㄋㄧˇ", 0}}, [&](protocol::Message response) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        responses.push_back(std::move(response));
        callback_condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        if (!callback_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return responses.size() >= 2; })) {
            transport.stop();
            server_thread.join();
            return EXIT_FAILURE;
        }
    }
    transport.stop();
    server_thread.join();
    if (!server_ok || !std::holds_alternative<protocol::Prediction>(responses.back())) return EXIT_FAILURE;

    // stop() may race with the initial status handshake. Once stopping is set,
    // the worker must not publish the newly connected socket and block waiting
    // for an open-session response.
    const auto delayed_path = std::filesystem::temp_directory_path() /
                              ("llavon-ime-transport-stop-test-" + std::to_string(getpid()) + ".sock");
    std::filesystem::remove(delayed_path, error);
    UnixSocketServer delayed_server;
    delayed_server.bind_listen(delayed_path);
    std::mutex delayed_mutex;
    std::condition_variable delayed_condition;
    bool status_received = false;
    bool release_status = false;
    bool open_seen = false;
    bool shutdown_callback = false;
    std::thread delayed_server_thread([&]() {
        try {
            auto connection = delayed_server.accept_one();
            const auto status_message = protocol::decode(receive_frame(connection));
            if (!std::holds_alternative<protocol::StatusRequest>(status_message)) server_ok = false;
            {
                std::unique_lock<std::mutex> lock(delayed_mutex);
                status_received = true;
                delayed_condition.notify_one();
                delayed_condition.wait(lock, [&]() { return release_status; });
            }
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
            try {
                const auto next = protocol::decode(receive_frame(connection));
                open_seen = std::holds_alternative<protocol::OpenSessionRequest>(next);
            } catch (...) {
                // The fixed worker closes immediately after the handshake.
            }
        } catch (...) {
            server_ok = false;
        }
    });

    ServiceTransportOptions delayed_options;
    delayed_options.socket_path = delayed_path;
    delayed_options.auto_start = false;
    ServiceTransport delayed_transport(delayed_options);
    delayed_transport.open_session([&](protocol::Message response) {
        const auto* value = std::get_if<protocol::Error>(&response);
        shutdown_callback = value != nullptr && value->code == protocol::ErrorCode::ServiceShuttingDown;
    });
    {
        std::unique_lock<std::mutex> lock(delayed_mutex);
        if (!delayed_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return status_received; })) {
            delayed_transport.stop();
            delayed_server_thread.join();
            return EXIT_FAILURE;
        }
    }
    std::thread release_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lock(delayed_mutex);
            release_status = true;
        }
        delayed_condition.notify_one();
    });
    delayed_transport.stop();
    release_thread.join();
    delayed_server_thread.join();
    std::filesystem::remove(delayed_path, error);
    if (open_seen || !shutdown_callback) return EXIT_FAILURE;

    // reconfigure() restarts the transport against a second service without
    // rebuilding the engine: the restarted worker serves the new socket and
    // the previous service is asked to shut down.
    const auto first_path = std::filesystem::temp_directory_path() /
                            ("llavon-ime-transport-first-" + std::to_string(getpid()) + ".sock");
    const auto second_path = std::filesystem::temp_directory_path() /
                             ("llavon-ime-transport-second-" + std::to_string(getpid()) + ".sock");
    std::filesystem::remove(first_path, error);
    std::filesystem::remove(second_path, error);
    UnixSocketServer first_server;
    first_server.bind_listen(first_path);
    UnixSocketServer second_server;
    second_server.bind_listen(second_path);
    bool first_shutdown = false;
    bool second_open = false;
    bool second_shutdown = false;
    std::thread first_thread([&]() {
        try {
            auto connection = first_server.accept_one();
            (void)receive_frame(connection);
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
            (void)receive_frame(connection);
            connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));
        } catch (...) {
        }
        try {
            auto shutdown_connection = first_server.accept_one();
            first_shutdown = std::holds_alternative<protocol::ShutdownRequest>(
                protocol::decode(receive_frame(shutdown_connection)));
            std::filesystem::remove(first_path, error);
        } catch (...) {
        }
    });
    std::thread second_thread([&]() {
        try {
            auto connection = second_server.accept_one();
            (void)receive_frame(connection);
            connection.send_all(protocol::encode(
                protocol::Message{protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}}));
            second_open = std::holds_alternative<protocol::OpenSessionRequest>(
                protocol::decode(receive_frame(connection)));
            connection.send_all(protocol::encode(protocol::Message{protocol::OpenSessionResponse{session, epoch}}));
        } catch (...) {
        }
        try {
            auto shutdown_connection = second_server.accept_one();
            second_shutdown = std::holds_alternative<protocol::ShutdownRequest>(
                protocol::decode(receive_frame(shutdown_connection)));
            std::filesystem::remove(second_path, error);
        } catch (...) {
        }
    });

    ServiceTransportOptions first_options;
    first_options.socket_path = first_path;
    first_options.service_path = "/nonexistent/llavon-ime-unix-service";
    ServiceTransport restarted(first_options);
    std::mutex restart_mutex;
    std::condition_variable restart_condition;
    std::vector<protocol::Message> restart_responses;
    const auto restart_callback = [&](protocol::Message response) {
        std::lock_guard<std::mutex> lock(restart_mutex);
        restart_responses.push_back(std::move(response));
        restart_condition.notify_one();
    };
    const auto wait_for_restart_response = [&](std::size_t count) {
        std::unique_lock<std::mutex> lock(restart_mutex);
        return restart_condition.wait_for(lock, std::chrono::seconds(2),
                                          [&]() { return restart_responses.size() >= count; });
    };

    restarted.open_session(restart_callback);
    if (!wait_for_restart_response(1)) {
        restarted.stop();
        first_thread.join();
        second_thread.join();
        return EXIT_FAILURE;
    }
    ServiceTransportOptions second_options;
    second_options.socket_path = second_path;
    second_options.service_path = "/nonexistent/llavon-ime-unix-service";
    restarted.reconfigure(second_options);
    restarted.open_session(restart_callback);
    if (!wait_for_restart_response(2)) {
        restarted.stop();
        first_thread.join();
        second_thread.join();
        return EXIT_FAILURE;
    }
    restarted.stop();
    first_thread.join();
    second_thread.join();
    if (!first_shutdown || !second_open || !second_shutdown) return EXIT_FAILURE;
    if (!std::holds_alternative<protocol::OpenSessionResponse>(restart_responses.back())) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

#else

int run_service_transport_tests() { return EXIT_SUCCESS; }

#endif

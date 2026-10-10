#include "test_suites.h"
#include "scripted_service.hpp"
#include "engine/service_transport.hpp"

#include <future>

int run_transport_resilience_tests() {
    using namespace llavon::ime;
    using test::ScriptedService;
    using namespace std::chrono_literals;
    bool ok = true;
    // A peer that never finishes the epoch handshake cannot hold destruction.
    {
        std::promise<void> accepted;
        auto received = accepted.get_future();
        ScriptedService server([&](int fd, int) {
            (void)ScriptedService::receive(fd);
            accepted.set_value();
            ScriptedService::await_disconnect(fd);
        });
        ServiceTransportOptions options;
        options.socket_path = server.path(); options.auto_start = false;
        options.handshake_timeout = 80ms;
        ServiceTransport transport(options);
        std::promise<protocol::Message> result;
        auto future = result.get_future();
        transport.open_session([&](protocol::Message message) { result.set_value(std::move(message)); });
        ok = ok && received.wait_for(1s) == std::future_status::ready;
        ok = ok && future.wait_for(1s) == std::future_status::ready;
        if (future.wait_for(0ms) == std::future_status::ready) ok = ok && std::holds_alternative<protocol::Error>(future.get());
        transport.stop();
        ok = ok && server.ok();
    }
    // One absolute deadline covers a header and a body that never arrives.
    // The next request reconnects, rather than consuming the old partial frame.
    {
        ScriptedService server([](int fd, int index) {
            ScriptedService::handshake(fd, true);
            (void)ScriptedService::receive(fd);
            if (index == 0) {
                const std::array<std::uint8_t, 4> partial{34, 0, 0, 0};
                (void)socket_io::write_all(fd, partial, std::chrono::steady_clock::now() + 1s);
                ScriptedService::await_disconnect(fd);
            } else {
                protocol::SessionId id{}; id[0] = 1;
                ScriptedService::send(fd, protocol::OpenSessionResponse{id, {}});
            }
        });
        ServiceTransportOptions options;
        options.socket_path = server.path(); options.auto_start = false; options.request_timeout = 80ms;
        ServiceTransport transport(options);
        for (int i = 0; i < 2; ++i) {
            std::promise<protocol::Message> result; auto future = result.get_future();
            transport.open_session([&](protocol::Message message) { result.set_value(std::move(message)); });
            if (future.wait_for(1s) != std::future_status::ready) { transport.stop(); ok = false; break; }
            const auto message = future.get();
            ok = ok && (i == 0 ? std::holds_alternative<protocol::Error>(message)
                               : std::holds_alternative<protocol::OpenSessionResponse>(message));
        }
        transport.stop();
        ok = ok && server.ok();
    }
    // Queue rejection is bounded and callbacks execute outside the queue lock.
    {
        std::promise<void> accepted; auto received = accepted.get_future();
        ScriptedService server([&](int fd, int) {
            ScriptedService::handshake(fd);
            (void)ScriptedService::receive(fd);
            accepted.set_value();
            ScriptedService::await_disconnect(fd);
        });
        ServiceTransportOptions options;
        options.socket_path = server.path(); options.auto_start = false; options.max_pending_requests = 1;
        ServiceTransport transport(options);
        transport.open_session({});
        ok = ok && received.wait_for(1s) == std::future_status::ready;
        transport.status(std::nullopt, {});
        std::promise<protocol::Message> result; auto future = result.get_future();
        transport.status(std::nullopt, [&](protocol::Message message) {
            (void)transport.connected();
            result.set_value(std::move(message));
        });
        ok = ok && future.wait_for(1s) == std::future_status::ready;
        if (future.wait_for(0ms) == std::future_status::ready) {
            const auto message = future.get();
            const auto* error = std::get_if<protocol::Error>(&message);
            ok = ok && error && error->code == protocol::ErrorCode::ResourceExhausted;
        }
        const auto before = std::chrono::steady_clock::now();
        transport.stop();
        ok = ok && std::chrono::steady_clock::now() - before < 1s;
    }
    // Queue policy: only replace predictions for the same session. A queued
    // commit and its correction cancel as a pair; later work remains ordered.
    {
        std::promise<void> accepted, release;
        auto received = accepted.get_future(); auto gate = release.get_future().share();
        ScriptedService server([&](int fd, int) {
            ScriptedService::handshake(fd);
            (void)ScriptedService::receive(fd);
            accepted.set_value();
            if (gate.wait_for(2s) != std::future_status::ready) throw std::runtime_error("queue gate");
            protocol::SessionId id{}; id[0] = 1;
            ScriptedService::send(fd, protocol::OpenSessionResponse{id, {}});
            const auto request = std::get<protocol::PredictRequest>(ScriptedService::receive(fd));
            if (request.request_id != 2) throw std::runtime_error("obsolete prediction executed");
            ScriptedService::send(fd, protocol::Prediction{request.session_id, 2, 2, {{U'你'}}});
            ScriptedService::await_disconnect(fd);
        });
        ServiceTransportOptions options; options.socket_path = server.path(); options.auto_start = false;
        ServiceTransport transport(options);
        transport.open_session({});
        const bool ready = received.wait_for(1s) == std::future_status::ready;
        ok = ok && ready;
        std::promise<protocol::Message> old_result, new_result;
        auto old_future = old_result.get_future(); auto new_future = new_result.get_future();
        if (ready) {
            protocol::SessionId id{}; id[0] = 1;
            transport.predict(id, 1, 1, {}, {}, [&](protocol::Message message) { old_result.set_value(std::move(message)); });
            protocol::RecordCommitRequest record; record.event_id[0] = 4;
            transport.record_commit(record); transport.discard_commit(record.event_id);
            transport.predict(id, 2, 2, {}, {}, [&](protocol::Message message) { new_result.set_value(std::move(message)); });
        }
        release.set_value();
        if (ready && old_future.wait_for(1s) == std::future_status::ready && new_future.wait_for(1s) == std::future_status::ready) {
            ok = ok && std::holds_alternative<protocol::Error>(old_future.get()) &&
                std::holds_alternative<protocol::Prediction>(new_future.get());
        } else ok = false;
        transport.stop();
        ok = ok && server.ok();
    }
    // Cold loading gets its own budget; the same delay after warm-up times out.
    {
        ScriptedService server([](int fd, int) {
            ScriptedService::handshake(fd, false);
            for (int i = 0; i < 2; ++i) {
                const auto request = std::get<protocol::PredictRequest>(ScriptedService::receive(fd));
                std::this_thread::sleep_for(180ms);
                if (i == 0) ScriptedService::send(fd, protocol::Prediction{
                    request.session_id, request.request_id, request.buffer_revision, {{U'你'}}});
                else ScriptedService::await_disconnect(fd);
            }
        });
        ServiceTransportOptions options;
        options.socket_path = server.path(); options.auto_start = false;
        options.request_timeout = 60ms; options.cold_prediction_timeout = 600ms;
        ServiceTransport transport(options);
        protocol::SessionId id{}; id[0] = 1;
        for (std::uint64_t i = 1; i <= 2; ++i) {
            std::promise<protocol::Message> result; auto future = result.get_future();
            transport.predict(id, i, i, {}, {}, [&](protocol::Message message) { result.set_value(std::move(message)); });
            if (future.wait_for(1s) != std::future_status::ready) { transport.stop(); ok = false; break; }
            const auto message = future.get();
            ok = ok && (i == 1 ? std::holds_alternative<protocol::Prediction>(message) : std::holds_alternative<protocol::Error>(message));
        }
        transport.stop();
    }
    // A stale error is no more trustworthy than a stale prediction. Reject it
    // instead of resetting the caller's session.
    {
        ScriptedService server([](int fd, int) {
            ScriptedService::handshake(fd);
            const auto request = std::get<protocol::PredictRequest>(ScriptedService::receive(fd));
            ScriptedService::send(fd, protocol::Error{protocol::ErrorCode::UnknownSession,
                request.session_id, request.request_id + 1, request.buffer_revision, "stale"});
            ScriptedService::await_disconnect(fd);
        });
        ServiceTransportOptions options; options.socket_path = server.path(); options.auto_start = false;
        ServiceTransport transport(options);
        std::promise<protocol::Message> result; auto future = result.get_future();
        transport.predict({}, 1, 2, {}, {}, [&](protocol::Message message) { result.set_value(std::move(message)); });
        if (future.wait_for(1s) == std::future_status::ready) {
            const auto message = future.get(); const auto* error = std::get_if<protocol::Error>(&message);
            ok = ok && error && error->code == protocol::ErrorCode::ProtocolError &&
                error->request_id == 1 && error->buffer_revision == 2;
        } else ok = false;
        transport.stop();
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#pragma once

#include "ipc/socket_io.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"
#include "util/unique_fd.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace llavon::ime::test {

// A bounded fake service: test failure cannot strand a blocking accept/recv.
class ScriptedService final {
public:
    explicit ScriptedService(std::function<void(int, int)> handler) {
        auto pattern = (std::filesystem::temp_directory_path() / "llavon-scripted-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("cannot create scripted service directory");
        root_ = pattern;
        path_ = root_ / "ime.sock";
        listener_.bind_listen(path_);
        thread_ = std::jthread([this, serve = std::move(handler)](std::stop_token stop) {
            int index = 0;
            while (!stop.stop_requested()) {
                pollfd descriptor{listener_.native_handle(), POLLIN, 0};
                if (::poll(&descriptor, 1, 20) <= 0) continue;
                const UniqueFd fd(::accept(listener_.native_handle(), nullptr, nullptr));
                if (!fd.valid()) continue;
                if (::fcntl(fd.get(), F_SETFL, O_NONBLOCK) != 0) { ok_ = false; return; }
#ifdef SO_NOSIGPIPE
                const int enabled = 1;
                (void)::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
                try { serve(fd.get(), index++); } catch (...) { ok_ = false; }
            }
        });
    }
    ~ScriptedService() {
        thread_.request_stop();
        thread_.join();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    ScriptedService(const ScriptedService&) = delete;
    ScriptedService& operator=(const ScriptedService&) = delete;
    const std::filesystem::path& path() const { return path_; }
    bool ok() const { return ok_.load(); }

    static protocol::Message receive(int fd) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        std::array<std::uint8_t, 4> header{};
        if (!socket_io::read_all(fd, header, deadline)) throw std::runtime_error("scripted frame header");
        const auto size = static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8U) |
            (static_cast<std::uint32_t>(header[2]) << 16U) | (static_cast<std::uint32_t>(header[3]) << 24U);
        if (size > protocol::kMaxFramePayloadBytes) throw std::runtime_error("scripted oversized frame");
        protocol::ByteVector bytes(header.begin(), header.end());
        bytes.resize(4U + size);
        if (!socket_io::read_all(fd, std::span(bytes).subspan(4), deadline)) throw std::runtime_error("scripted frame body");
        return protocol::decode(bytes);
    }
    static void send(int fd, const protocol::Message& message) {
        if (!socket_io::write_all(fd, protocol::encode(message), std::chrono::steady_clock::now() + std::chrono::seconds(2)))
            throw std::runtime_error("scripted send");
    }
    static void handshake(int fd, bool loaded = false) {
        if (!std::holds_alternative<protocol::StatusRequest>(receive(fd))) throw std::runtime_error("missing status");
        protocol::ServiceEpoch epoch{};
        epoch[0] = 5;
        send(fd, protocol::StatusResponse{epoch, false, loaded, 0, 8, std::nullopt});
    }
    static void await_disconnect(int fd) {
        std::array<std::uint8_t, 1> byte{};
        if (socket_io::read_all(fd, byte, std::chrono::steady_clock::now() + std::chrono::seconds(2)) || errno == ETIMEDOUT)
            throw std::runtime_error("expected disconnect");
    }

private:
    std::filesystem::path root_, path_;
    UnixSocketServer listener_;
    std::atomic_bool ok_{true};
    std::jthread thread_;
};

}  // namespace llavon::ime::test

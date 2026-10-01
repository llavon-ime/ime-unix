#include "ipc/unix_socket.hpp"
#include "util/unique_fd.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace llavon::ime {

namespace {

sockaddr_un make_address(const std::filesystem::path& path) {
    const auto text = path.string();
    if (text.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("Unix socket path is too long: " + text);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, text.c_str(), sizeof(address.sun_path) - 1);
    return address;
}

[[noreturn]] void throw_errno(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}

void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
#endif
}

bool socket_accepts_connections(const std::filesystem::path& path) {
    try {
        auto connection = UnixSocketClient{}.connect(path);
        return connection.valid();
    } catch (const std::system_error& error) {
        const int value = error.code().value();
        if (value == ECONNREFUSED || value == ENOENT) return false;
        throw;
    }
}

}  // namespace

UnixSocketConnection::UnixSocketConnection(int fd) : fd_(fd) {
    suppress_sigpipe(fd_);
}

UnixSocketConnection::~UnixSocketConnection() {
    close();
}

UnixSocketConnection::UnixSocketConnection(UnixSocketConnection&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UnixSocketConnection& UnixSocketConnection::operator=(UnixSocketConnection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

bool UnixSocketConnection::valid() const noexcept {
    return fd_ >= 0;
}

void UnixSocketConnection::send_all(std::span<const std::uint8_t> bytes) const {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t rc = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw_errno("send failed");
        }
        if (rc == 0) throw std::runtime_error("send returned zero bytes");
        sent += static_cast<size_t>(rc);
    }
}

std::vector<std::uint8_t> UnixSocketConnection::recv_exact(size_t size) const {
    std::vector<std::uint8_t> bytes(size);
    size_t received = 0;
    while (received < size) {
        const ssize_t rc = ::recv(fd_, bytes.data() + received, size - received, 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw_errno("recv failed");
        }
        if (rc == 0) throw std::runtime_error("socket closed before receiving expected bytes");
        received += static_cast<size_t>(rc);
    }
    return bytes;
}

void UnixSocketConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

UnixSocketConnection UnixSocketClient::connect(const std::filesystem::path& path) const {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) throw_errno("socket failed");

    const auto address = make_address(path);
    while (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno == EINTR) continue;
        const int saved_errno = errno;
        throw std::system_error(saved_errno, std::generic_category(), "connect failed");
    }
    return UnixSocketConnection(fd.release());
}

UnixSocketServer::~UnixSocketServer() {
    if (fd_ >= 0) ::close(fd_);
    if (!path_.empty()) {
        std::error_code ec;
        if (std::filesystem::symlink_status(path_, ec).type() == std::filesystem::file_type::socket) {
            std::filesystem::remove(path_, ec);
        }
    }
}

void UnixSocketServer::bind_listen(const std::filesystem::path& path) {
    if (fd_ >= 0) throw std::runtime_error("server already bound");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    if (std::filesystem::exists(path)) {
        if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::socket) {
            throw std::runtime_error("refusing to replace non-socket path: " + path.string());
        }
        if (socket_accepts_connections(path)) {
            throw std::runtime_error("Unix socket is already in use: " + path.string());
        }
        std::filesystem::remove(path);
    }

    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) throw_errno("socket failed");

    const auto address = make_address(path);
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const int saved_errno = errno;
        throw std::system_error(saved_errno, std::generic_category(), "bind failed");
    }

    if (::listen(fd.get(), 16) < 0) {
        const int saved_errno = errno;
        std::filesystem::remove(path);
        throw std::system_error(saved_errno, std::generic_category(), "listen failed");
    }

    path_ = path;
    fd_ = fd.release();
}

UnixSocketConnection UnixSocketServer::accept_one() const {
    int fd = -1;
    do {
        fd = ::accept(fd_, nullptr, nullptr);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) throw_errno("accept failed");
    return UnixSocketConnection(fd);
}

int UnixSocketServer::native_handle() const noexcept {
    return fd_;
}

}  // namespace llavon::ime

#include "ipc/socket_io.hpp"
#include "util/unique_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace llavon::ime::socket_io {
namespace {

bool ready(int fd, short events, Deadline deadline) {
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) { errno = ETIMEDOUT; return false; }
        const auto timeout = static_cast<int>(std::min(remaining.count(),
            static_cast<decltype(remaining.count())>(std::numeric_limits<int>::max())));
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) { errno = ETIMEDOUT; return false; }
        if (result < 0) return false;
        if ((descriptor.revents & POLLNVAL) != 0) { errno = EBADF; return false; }
        // Let recv/send report EOF or the actual socket error on HUP/ERR.
        return true;
    }
}

}  // namespace

int connect(const std::filesystem::path& path, Deadline deadline) {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) return -1;
    if (::fcntl(fd.get(), F_SETFL, O_NONBLOCK) < 0 || ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.size() >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    std::ranges::copy(name, std::begin(address.sun_path));
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1)) != 0) {
        // Linux returns EAGAIN for a full AF_UNIX backlog, not EINPROGRESS.
        // Retry on the next connection attempt rather than treating it as connected.
        if (errno != EINPROGRESS) return -1;
        if (!ready(fd.get(), POLLOUT, deadline)) return -1;
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0) return -1;
        if (error != 0) { errno = error; return -1; }
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return -1;
#endif
    return fd.release();
}

bool write_all(int fd, std::span<const std::uint8_t> bytes, Deadline deadline) {
    while (!bytes.empty()) {
        if (!ready(fd, POLLOUT, deadline)) return false;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto count = ::send(fd, bytes.data(), bytes.size(), flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) return false;
        if (count == 0) { errno = EPIPE; return false; }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

bool read_all(int fd, std::span<std::uint8_t> bytes, Deadline deadline) {
    while (!bytes.empty()) {
        if (!ready(fd, POLLIN, deadline)) return false;
        const auto count = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) return false;
        if (count == 0) { errno = ECONNRESET; return false; }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

}  // namespace llavon::ime::socket_io

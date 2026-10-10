#include "unix_socket_server.hpp"

#include "../pipe/protocol.hpp"
#include "../../../engine/src/util/unique_fd.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <queue>
#include <random>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#ifdef __APPLE__
#include <sys/types.h>
#endif

namespace ime::unix_service {

using llavon::ime::UniqueFd;

namespace {

std::uint32_t read_u32_le(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool read_all(int fd, std::span<std::uint8_t> destination) {
    auto* bytes = destination.data();
    const auto size = destination.size();
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count == 0) return false;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

void write_all(int fd, std::span<const std::uint8_t> source) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto* bytes = source.data();
    const auto size = source.size();
    std::size_t offset = 0;
    while (offset < size) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) throw std::runtime_error("Unix socket write timed out");
        pollfd descriptor{fd, POLLOUT, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) throw std::runtime_error("Unix socket write timed out");
        if (ready < 0) throw std::system_error(errno, std::generic_category(), "poll Unix socket write");
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto count = ::send(fd, bytes + offset, size - offset, flags);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::system_error(errno, std::generic_category(), "send on Unix socket");
        }
        if (count == 0) throw std::runtime_error("Unix socket closed while sending");
        offset += static_cast<std::size_t>(count);
    }
}

bool owned_by_current_user(const struct stat& status) {
    return status.st_uid == ::getuid();
}

struct stat lstat_or_throw(const std::filesystem::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        throw std::system_error(errno, std::generic_category(), "lstat " + path.string());
    }
    return status;
}

void require_private_directory(const std::filesystem::path& path, bool create) {
    if (create) {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) throw std::system_error(error.value(), std::generic_category(), "create runtime directory");
    }
    const auto status = lstat_or_throw(path);
    if (!S_ISDIR(status.st_mode) || !owned_by_current_user(status)) {
        throw std::runtime_error("runtime directory must be an owner-only directory: " + path.string());
    }
    if (::chmod(path.c_str(), S_IRWXU) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod runtime directory");
    }
}

bool socket_is_active(const std::filesystem::path& path) {
    const UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!probe.valid()) throw std::system_error(errno, std::generic_category(), "create Unix socket probe");
    if (::fcntl(probe.get(), F_SETFL, O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "make socket probe nonblocking");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto string_path = path.string();
    if (string_path.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("Unix socket path is too long: " + string_path);
    }
    std::memcpy(address.sun_path, string_path.c_str(), string_path.size() + 1);
    const int result = ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address),
                                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + string_path.size() + 1));
    const int saved_errno = errno;
    if (result == 0) return true;
    // A full listen backlog is still an active service, not a stale socket.
    if (saved_errno == EAGAIN || saved_errno == EINPROGRESS) return true;
    if (saved_errno == ECONNREFUSED || saved_errno == ENOENT || saved_errno == ECONNRESET) return false;
    throw std::system_error(saved_errno, std::generic_category(), "probe Unix socket");
}

void prepare_socket_path(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) throw std::runtime_error("Unix socket must have a parent directory");
    require_private_directory(parent, true);

    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) return;
        throw std::system_error(errno, std::generic_category(), "lstat Unix socket");
    }
    if (S_ISLNK(status.st_mode)) throw std::runtime_error("refusing symlink Unix socket path: " + path.string());
    if (!S_ISSOCK(status.st_mode)) throw std::runtime_error("refusing non-socket path: " + path.string());
    if (!owned_by_current_user(status)) throw std::runtime_error("refusing Unix socket owned by another user");
    if (socket_is_active(path)) throw std::runtime_error("Unix socket is already in use: " + path.string());
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "remove stale Unix socket");
    }
}

std::uint64_t peer_uid(int fd) {
#ifdef __linux__
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        throw std::system_error(errno, std::generic_category(), "get Unix peer credentials");
    }
    return static_cast<std::uint64_t>(credentials.uid);
#elif defined(__APPLE__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) {
        throw std::system_error(errno, std::generic_category(), "get Unix peer credentials");
    }
    return static_cast<std::uint64_t>(uid);
#else
    (void)fd;
    return static_cast<std::uint64_t>(::getuid());
#endif
}

}  // namespace

class UnixSocketServer::Connection final : public std::enable_shared_from_this<Connection> {
public:
    Connection(UnixSocketServer& server, int fd, std::uint64_t uid) : server_(server), fd_(fd), uid_(uid) {}

    ~Connection() {
        close();
        // The descriptor is stable while a reader or queued worker owns us.
        // Shutdown wakes recv; only the last owner closes it, avoiding fd reuse races.
        if (fd_ >= 0) ::close(fd_);
    }

    void run() {
        while (!closed()) {
            std::array<std::uint8_t, 4> header{};
            if (!read_all(fd(), header)) break;
            const auto payload_length = read_u32_le(header.data());
            if (payload_length > protocol::kMaxFramePayloadBytes) {
                send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "protocol frame is too large");
                break;
            }
            protocol::ByteVector frame(header.begin(), header.end());
            protocol::ByteVector payload(payload_length);
            if (!read_all(fd(), payload)) break;
            frame.insert(frame.end(), payload.begin(), payload.end());
            try {
                dispatch(protocol::decode(frame));
            } catch (const protocol::ProtocolError& error) {
                send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, error.what());
            } catch (const std::exception& error) {
                send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, error.what());
            }
        }
        close();
    }

    void close() noexcept {
        if (closed_.exchange(true, std::memory_order_acq_rel)) return;
        ::shutdown(fd_, SHUT_RDWR);
    }

    bool closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    std::uint64_t uid() const noexcept {
        return uid_;
    }

    void send(const protocol::Message& message) noexcept {
        try {
            const auto bytes = protocol::encode(message);
            std::lock_guard lock(write_mutex_);
            if (!closed()) write_all(fd_, bytes);
        } catch (...) {
            close();
        }
    }

private:
    void send_error(protocol::ErrorCode code, const protocol::SessionId& id, std::uint64_t request_id,
                    std::uint64_t revision, std::string message) noexcept {
        send(protocol::Message{protocol::Error{code, id, request_id, revision, std::move(message)}});
    }

    void dispatch(const protocol::Message& message) {
        std::visit(
            [this](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, protocol::OpenSessionRequest>) {
                    if (!server_.workers_->enqueue([self = shared_from_this()]() {
                            if (self->closed()) return;
                            const auto result = self->server_.sessions_->open_session(self->uid());
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_queue_error({}, 0, 0);
                    }
                } else if constexpr (std::is_same_v<T, protocol::PredictRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            if (self->closed()) return;
                            const auto result = self->server_.sessions_->predict(self->uid(), request);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_queue_error(request.session_id, request.request_id, request.buffer_revision);
                    }
                } else if constexpr (std::is_same_v<T, protocol::CloseSessionRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            const auto result = self->server_.sessions_->close_session(self->uid(), request.session_id);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_queue_error(request.session_id, 0, 0);
                    }
                } else if constexpr (std::is_same_v<T, protocol::StatusRequest>) {
                    const auto result = server_.sessions_->status(uid_, value.session_id);
                    std::visit([this](const auto& response) { send(protocol::Message{response}); }, result);
                } else if constexpr (std::is_same_v<T, protocol::RecordCommitRequest>) {
                    // A connection's Record/Discard stream is ordered independently
                    // of slow model jobs. Parallel workers must not reverse it.
                    const bool stored = server_.record_commit(value);
                    send(protocol::Message{protocol::RecordCommitResponse{value.event_id, stored}});
                } else if constexpr (std::is_same_v<T, protocol::DiscardCommitRequest>) {
                    const bool discarded = server_.discard_commit(value.event_id);
                    send(protocol::Message{protocol::DiscardCommitResponse{value.event_id, discarded}});
                } else if constexpr (std::is_same_v<T, protocol::ShutdownRequest>) {
                    send(protocol::Message{protocol::ShutdownResponse{true}});
                    server_.request_stop();
                } else {
                    send_error(protocol::ErrorCode::ProtocolError, {}, 0, 0, "unexpected response message from client");
                }
            },
            message);
    }

    int fd() const noexcept {
        return fd_;
    }

    void send_queue_error(const protocol::SessionId& id, std::uint64_t request_id, std::uint64_t revision) {
        const bool stopping = server_.stopping_.load(std::memory_order_acquire);
        send_error(stopping ? protocol::ErrorCode::ServiceShuttingDown : protocol::ErrorCode::ResourceExhausted,
                   id, request_id, revision, stopping ? "service is shutting down" : "service queue is full");
    }

    UnixSocketServer& server_;
    int fd_ = -1;
    std::atomic_bool closed_{false};
    std::uint64_t uid_ = 0;
    mutable std::mutex write_mutex_;
};

UnixSocketServer::UnixSocketServer(UnixServerOptions options) : options_(std::move(options)) {
    socket_path_ = options_.socket_path.value_or(default_socket_path());
    pid_path_ = options_.pid_path.value_or(socket_path_.parent_path() / "service.pid");
    if (socket_path_.is_relative()) socket_path_ = std::filesystem::absolute(socket_path_);
    if (pid_path_.is_relative()) pid_path_ = std::filesystem::absolute(pid_path_);
    runtime_ = std::make_shared<CoreRuntime>(options_.runtime);
}

bool UnixSocketServer::record_commit(const protocol::RecordCommitRequest& request) {
    std::lock_guard lock(commit_mutex_);
    if (!commits_) commits_ = std::make_unique<CommitStore>();
    return commits_->record(request);
}

bool UnixSocketServer::discard_commit(const protocol::SessionId& event_id) {
    std::lock_guard lock(commit_mutex_);
    if (!commits_) return false;
    return commits_->discard_staged(event_id);
}

void UnixSocketServer::settle_staged_commits(bool all) {
    std::lock_guard lock(commit_mutex_);
    if (!commits_) return;
    try {
        (void)commits_->flush_staged(all);
    } catch (const std::exception& error) {
        std::clog << "[SRV] could not write staged commits: " << error.what() << '\n';
    }
}

UnixSocketServer::~UnixSocketServer() {
    request_stop();
    close_connections();
    connections_.clear();
    if (workers_) workers_->shutdown();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    cleanup_endpoint();
}

const char* UnixSocketServer::name() const {
    return "unix-socket";
}

std::filesystem::path UnixSocketServer::default_socket_path() {
    if (const char* override_path = std::getenv("LLAVON_IME_UNIX_SOCKET_PATH");
        override_path && override_path[0] != '\0') {
        return override_path;
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = std::getenv("TMPDIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = "/tmp";
    return std::filesystem::path(runtime) / "llavon-ime" / "ime.sock";
}

std::filesystem::path UnixSocketServer::default_pid_path() {
    const auto socket = default_socket_path();
    return socket.parent_path() / "service.pid";
}

int UnixSocketServer::run() {
    runtime_->validate_configuration();
    sessions_ = std::make_unique<SessionManager>(runtime_, options_.limits);
    workers_ = std::make_unique<WorkerPool>(std::max<std::size_t>(2, options_.limits.max_concurrent_predictions + 1),
                                          options_.max_queued_requests);

    prepare_socket_path(socket_path_);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "create Unix listening socket");
    listen_fd_ = fd;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto string_path = socket_path_.string();
    if (string_path.size() >= sizeof(address.sun_path)) throw std::runtime_error("Unix socket path is too long");
    std::memcpy(address.sun_path, string_path.c_str(), string_path.size() + 1);
    const auto address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + string_path.size() + 1);
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        throw std::system_error(errno, std::generic_category(), "bind Unix socket");
    }
    endpoint_owned_ = true;
    if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod Unix socket");
    }
    if (::listen(listen_fd_, 32) != 0) throw std::system_error(errno, std::generic_category(), "listen Unix socket");

    if (!pid_path_.parent_path().empty()) require_private_directory(pid_path_.parent_path(), true);
    {
        struct stat status {};
        if (::lstat(pid_path_.c_str(), &status) == 0) {
            if (S_ISLNK(status.st_mode) || !S_ISREG(status.st_mode) || !owned_by_current_user(status)) {
                throw std::runtime_error("refusing unsafe service PID path: " + pid_path_.string());
            }
            if (::unlink(pid_path_.c_str()) != 0) throw std::system_error(errno, std::generic_category(), "remove stale PID file");
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "lstat service PID file");
        }
        std::ofstream pid(pid_path_);
        if (!pid) throw std::runtime_error("failed to create service PID file: " + pid_path_.string());
        pid << ::getpid() << '\n';
        if (!pid) throw std::runtime_error("failed to write service PID file: " + pid_path_.string());
        ::chmod(pid_path_.c_str(), S_IRUSR | S_IWUSR);
        pid_owned_ = true;
    }

    std::clog << "[SRV] listening on " << socket_path_ << " epoch=";
    for (const auto byte : sessions_->service_epoch()) std::clog << std::hex << static_cast<unsigned>(byte);
    std::clog << std::dec << '\n';

    while (!stopping_.load(std::memory_order_acquire)) {
        pollfd descriptor{listen_fd_, POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 250);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll Unix listening socket");
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) accept_connections();
        reap_connections();
        sessions_->reap();
        // A commit is only written once its correction window elapsed.
        settle_staged_commits(false);
        if (sessions_->should_idle_shutdown()) request_stop();
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    close_connections();
    {
        std::lock_guard lock(connections_mutex_);
        connections_.clear();
    }
    if (workers_) workers_->shutdown();
    // Nothing can arrive now, so settle whatever is still staged before the
    // store goes away.
    settle_staged_commits(true);
    if (sessions_) sessions_->shutdown();
    cleanup_endpoint();
    return 0;
}

void UnixSocketServer::request_stop() noexcept {
    stopping_.store(true, std::memory_order_release);
}

void UnixSocketServer::accept_connections() {
    sockaddr_un address {};
    socklen_t length = sizeof(address);
    UniqueFd accepted(::accept(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length));
    if (!accepted.valid()) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (stopping_.load(std::memory_order_acquire)) return;
        throw std::system_error(errno, std::generic_category(), "accept Unix socket");
    }
    const int connection_fd = accepted.get();
    if (::fcntl(connection_fd, F_SETFD, FD_CLOEXEC) != 0) return;
#ifdef SO_NOSIGPIPE
    // macOS has no MSG_NOSIGNAL; ask the socket itself not to raise SIGPIPE.
    const int enabled = 1;
    (void)::setsockopt(connection_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    const auto uid = peer_uid(connection_fd);
    if (uid != static_cast<std::uint64_t>(::getuid())) return;
    reap_connections();
    std::lock_guard lock(connections_mutex_);
    if (connections_.size() >= options_.max_connections) return;
    auto connection = std::make_shared<Connection>(*this, connection_fd, uid);
    (void)accepted.release();
    auto finished = std::make_shared<std::atomic_bool>(false);
    std::jthread thread([connection, finished]() {
        try { connection->run(); } catch (...) { connection->close(); }
        finished->store(true, std::memory_order_release);
    });
    connections_.push_back(ConnectionTask{std::move(connection), std::move(finished), std::move(thread)});
}

void UnixSocketServer::close_connections() noexcept {
    std::lock_guard lock(connections_mutex_);
    for (const auto& task : connections_) task.connection->close();
}

void UnixSocketServer::reap_connections() {
    std::lock_guard lock(connections_mutex_);
    std::erase_if(connections_, [](const ConnectionTask& task) {
        return task.finished->load(std::memory_order_acquire);
    });
}

void UnixSocketServer::cleanup_endpoint() noexcept {
    if (endpoint_owned_) {
        struct stat status {};
        if (::lstat(socket_path_.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) && owned_by_current_user(status)) {
            ::unlink(socket_path_.c_str());
        }
        endpoint_owned_ = false;
    }
    if (pid_owned_) {
        struct stat status {};
        if (::lstat(pid_path_.c_str(), &status) == 0 && S_ISREG(status.st_mode) && owned_by_current_user(status)) {
            ::unlink(pid_path_.c_str());
        }
        pid_owned_ = false;
    }
}

}  // namespace ime::unix_service

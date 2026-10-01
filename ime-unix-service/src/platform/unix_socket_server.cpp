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
    const auto* bytes = source.data();
    const auto size = source.size();
    std::size_t offset = 0;
    while (offset < size) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto count = ::send(fd, bytes + offset, size - offset, flags);
        if (count < 0) {
            if (errno == EINTR) continue;
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

class UnixSocketServer::WorkerPool {
public:
    explicit WorkerPool(std::size_t count) {
        count = std::max<std::size_t>(1, count);
        for (std::size_t i = 0; i < count; ++i)
            workers_.emplace_back([this](std::stop_token stop) { run(stop); });
    }

    ~WorkerPool() {
        shutdown();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    bool enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            queue_.push(std::move(task));
        }
        condition_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                // A second call still joins any threads that have not been joined.
            } else {
                stopping_ = true;
            }
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
    }

private:
    void run(std::stop_token stop) {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [this]() { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_ || stop.stop_requested()) return;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }
            try {
                task();
            } catch (...) {
                // A connection owns the error response.  Worker exceptions must never
                // terminate the service process.
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::queue<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;
};

class UnixSocketServer::Connection final : public std::enable_shared_from_this<Connection> {
public:
    Connection(UnixSocketServer& server, int fd, std::uint64_t uid) : server_(server), fd_(fd), uid_(uid) {}

    ~Connection() {
        close();
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
        std::lock_guard lock(write_mutex_);
        if (fd_ < 0) return;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    bool closed() const noexcept {
        std::lock_guard lock(write_mutex_);
        return fd_ < 0;
    }

    std::uint64_t uid() const noexcept {
        return uid_;
    }

    void send(const protocol::Message& message) noexcept {
        try {
            const auto bytes = protocol::encode(message);
            std::lock_guard lock(write_mutex_);
            if (fd_ >= 0) write_all(fd_, bytes);
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
                            const auto result = self->server_.sessions_->open_session(self->uid());
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::PredictRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            const auto result = self->server_.sessions_->predict(self->uid(), request);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                                   request.buffer_revision, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::CloseSessionRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            const auto result = self->server_.sessions_->close_session(self->uid(), request.session_id);
                            std::visit([&self](const auto& response) { self->send(protocol::Message{response}); }, result);
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, 0, 0,
                                   "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::StatusRequest>) {
                    const auto result = server_.sessions_->status(uid_, value.session_id);
                    std::visit([this](const auto& response) { send(protocol::Message{response}); }, result);
                } else if constexpr (std::is_same_v<T, protocol::RecordCommitRequest>) {
                    const auto request = value;
                    if (!server_.workers_->enqueue([self = shared_from_this(), request]() {
                            try {
                                const bool stored = self->server_.record_commit(request);
                                self->send(protocol::Message{protocol::RecordCommitResponse{request.event_id, stored}});
                            } catch (const std::exception& error) {
                                self->send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, error.what());
                            }
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");
                    }
                } else if constexpr (std::is_same_v<T, protocol::DiscardCommitRequest>) {
                    const auto event_id = value.event_id;
                    if (!server_.workers_->enqueue([self = shared_from_this(), event_id]() {
                            try {
                                const bool discarded = self->server_.discard_commit(event_id);
                                self->send(protocol::Message{protocol::DiscardCommitResponse{event_id, discarded}});
                            } catch (const std::exception& error) {
                                self->send_error(protocol::ErrorCode::InvalidArgument, {}, 0, 0, error.what());
                            }
                        })) {
                        send_error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");
                    }
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

    UnixSocketServer& server_;
    int fd_ = -1;
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
    for (auto& thread : connection_threads_) {
        if (thread.joinable()) thread.join();
    }
    connection_threads_.clear();
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
    workers_ = std::make_unique<WorkerPool>(std::max<std::size_t>(2, options_.limits.max_concurrent_predictions + 1));

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
        for (auto& thread : connection_threads_) {
            if (thread.joinable()) thread.join();
        }
        connection_threads_.clear();
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
#ifdef SO_NOSIGPIPE
    // macOS has no MSG_NOSIGNAL; ask the socket itself not to raise SIGPIPE.
    const int enabled = 1;
    (void)::setsockopt(connection_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    const auto uid = peer_uid(connection_fd);
    if (uid != static_cast<std::uint64_t>(::getuid())) return;
    auto connection = std::make_shared<Connection>(*this, connection_fd, uid);
    (void)accepted.release();
    std::lock_guard lock(connections_mutex_);
    connections_.push_back(connection);
    connection_threads_.emplace_back([connection]() { connection->run(); });
}

void UnixSocketServer::close_connections() noexcept {
    std::lock_guard lock(connections_mutex_);
    for (const auto& connection : connections_) connection->close();
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

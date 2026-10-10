#ifndef _WIN32

#include "service_transport.hpp"
#include "util/unique_fd.hpp"
#include "ipc/socket_io.hpp"
#include "config/config.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace llavon::ime {

namespace {

std::filesystem::path env_path(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') return value;
    return {};
}

void validate_limits(const ServiceTransportOptions& options) {
    if (options.max_pending_requests == 0 || options.max_pending_requests > 65536 ||
        options.connect_timeout.count() <= 0 || options.handshake_timeout.count() <= 0 ||
        options.request_timeout.count() <= 0 || options.cold_prediction_timeout.count() <= 0)
        throw std::invalid_argument("invalid transport limits");
}

bool launch_service(std::vector<std::string>& arguments) {
    // Build argv before fork: allocator locks may be owned by another thread.
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t launcher = ::fork();
    if (launcher < 0) return false;
    if (launcher == 0) {
        // The long-lived service must not leave a zombie child behind in an
        // input-method host after it exits or is repeatedly restarted.
        const pid_t service = ::fork();
        if (service < 0) _exit(127);
        if (service > 0) _exit(0);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    pid_t ended;
    do { ended = ::waitpid(launcher, &status, 0); } while (ended < 0 && errno == EINTR);
    return ended == launcher && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

protocol::ByteVector recv_frame(int fd, socket_io::Deadline deadline) {
    std::array<std::uint8_t, 4> header{};
    if (!socket_io::read_all(fd, header, deadline)) throw std::system_error(errno, std::generic_category(), "read service frame header");
    const auto length = static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8U) |
                        (static_cast<std::uint32_t>(header[2]) << 16U) | (static_cast<std::uint32_t>(header[3]) << 24U);
    if (length > protocol::kMaxFramePayloadBytes) throw protocol::ProtocolError("service frame is too large");
    protocol::ByteVector result(header.begin(), header.end());
    result.resize(result.size() + length);
    if (!socket_io::read_all(fd, std::span(result).subspan(4, length), deadline)) throw std::system_error(errno, std::generic_category(), "read service frame");
    return result;
}

}  // namespace

ServiceTransportOptions service_options_from_config(const Config& config, ServiceTransportOptions options,
                                                   bool preserve_model_path) {
    if (!preserve_model_path) options.model_path = config.model_path;
    options.context_length = static_cast<std::uint32_t>(std::max(1, config.context_length));
    options.threads = static_cast<std::uint32_t>(std::max(1, config.thread_count));
    options.gpu_layers = config.gpu_layers;
    options.idle_timeout_seconds = static_cast<std::uint32_t>(std::max(1, config.idle_timeout_seconds));
    return options;
}

bool same_service_runtime(const ServiceTransportOptions& left, const ServiceTransportOptions& right) {
    return left.model_path == right.model_path && left.context_length == right.context_length &&
           left.threads == right.threads && left.gpu_layers == right.gpu_layers &&
           left.idle_timeout_seconds == right.idle_timeout_seconds;
}

ServiceTransport::ServiceTransport(ServiceTransportOptions options) : options_(std::move(options)) {
    validate_limits(options_);
    if (options_.socket_path.empty()) options_.socket_path = default_socket_path();
    if (options_.service_path.empty()) options_.service_path = default_service_path();
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

ServiceTransport::~ServiceTransport() {
    stop();
}

void ServiceTransport::open_session(Callback callback) {
    enqueue(RequestKind::Open, protocol::Message{protocol::OpenSessionRequest{}}, std::move(callback));
}

void ServiceTransport::predict(const protocol::SessionId& session_id, std::uint64_t request_id,
                               std::uint64_t buffer_revision, std::u16string context,
                               std::vector<protocol::PaddingEntry> padding, Callback callback) {
    enqueue(RequestKind::Predict,
            protocol::Message{protocol::PredictRequest{session_id, request_id, buffer_revision, std::move(context), std::move(padding)}},
            std::move(callback));
}

void ServiceTransport::close_session(const protocol::SessionId& session_id, Callback callback) {
    enqueue(RequestKind::Close, protocol::Message{protocol::CloseSessionRequest{session_id}}, std::move(callback));
}

void ServiceTransport::status(std::optional<protocol::SessionId> session_id, Callback callback) {
    enqueue(RequestKind::Status, protocol::Message{protocol::StatusRequest{session_id}}, std::move(callback));
}

void ServiceTransport::shutdown(Callback callback) {
    enqueue(RequestKind::Shutdown, protocol::Message{protocol::ShutdownRequest{}}, std::move(callback));
}

void ServiceTransport::stop() {
    bool should_shutdown = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            // The join below is still needed when stop() is called twice.
        } else {
            stopping_ = true;
            should_shutdown = true;
        }
    }
    condition_.notify_all();
    // Wake a blocking recv without closing/reusing the descriptor underneath
    // the worker. The worker owns the final disconnect while it unwinds.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (socket_fd_ >= 0) (void)::shutdown(socket_fd_, SHUT_RDWR);
    }
    if (worker_.joinable()) worker_.join();
    disconnect();
    if (should_shutdown) shutdown_service();
}

bool ServiceTransport::connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

void ServiceTransport::reconfigure(ServiceTransportOptions options) {
    validate_limits(options);
    // stop() joins the worker, fails everything still queued, and shuts down
    // the service this transport owned. The worker is restarted with the new
    // options so a service may only spawn once a request arrives.
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        connected_ = false;
        socket_fd_ = -1;
        epoch_.reset();
        spawn_backoff_until_ = {};
    }
    if (options.socket_path.empty()) options.socket_path = default_socket_path();
    if (options.service_path.empty()) options.service_path = default_service_path();
    options_ = std::move(options);
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

std::optional<protocol::ServiceEpoch> ServiceTransport::service_epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

const ServiceTransportOptions& ServiceTransport::options() const noexcept {
    return options_;
}

std::filesystem::path ServiceTransport::default_socket_path() {
    if (auto value = env_path("LLAVON_IME_UNIX_SOCKET_PATH"); !value.empty()) return value;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = std::getenv("TMPDIR");
    if (runtime == nullptr || runtime[0] == '\0') runtime = "/tmp";
    return std::filesystem::path(runtime) / "llavon-ime" / "ime.sock";
}

std::filesystem::path ServiceTransport::default_service_path() {
    if (auto value = env_path("LLAVON_IME_UNIX_SERVICE_PATH"); !value.empty()) return value;
#ifdef LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH
    if (std::filesystem::exists(LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH)) {
        return LLAVON_IME_INSTALLED_UNIX_SERVICE_PATH;
    }
#endif
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const auto candidate =
            std::filesystem::path(home) / "Library" / "fcitx5" / "bin" / "llavon-ime-unix-service";
        if (std::filesystem::exists(candidate)) return candidate;
    }
#endif
    if (const char* path_env = std::getenv("PATH"); path_env != nullptr) {
        std::string_view paths(path_env);
        while (!paths.empty()) {
            const auto separator = paths.find(':');
            const auto part = paths.substr(0, separator);
            if (!part.empty()) {
                const auto candidate = std::filesystem::path(std::string(part)) / "llavon-ime-unix-service";
                if (std::filesystem::exists(candidate)) return candidate;
            }
            if (separator == std::string_view::npos) break;
            paths.remove_prefix(separator + 1);
        }
    }
    return "llavon-ime-unix-service";
}

void ServiceTransport::enqueue(RequestKind kind, protocol::Message message, Callback callback) {
    Callback rejected_callback;
    std::optional<protocol::Message> rejected_response;
    std::optional<Pending> superseded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_ && kind == RequestKind::Predict) {
            const auto id = std::get<protocol::PredictRequest>(message).session_id;
            const auto obsolete = std::ranges::find_if(queue_, [&id](const Pending& pending) {
                const auto* prediction = std::get_if<protocol::PredictRequest>(&pending.message);
                return prediction && prediction->session_id == id;
            });
            if (obsolete != queue_.end()) {
                superseded = std::move(*obsolete);
                queue_.erase(obsolete);
            }
        }
        if (!stopping_ && kind == RequestKind::DiscardCommit) {
            const auto event = std::get<protocol::DiscardCommitRequest>(message).event_id;
            const auto recorded = std::ranges::find_if(queue_, [&event](const Pending& pending) {
                const auto* record = std::get_if<protocol::RecordCommitRequest>(&pending.message);
                return record && record->event_id == event;
            });
            if (recorded != queue_.end()) {
                // Neither operation was sent. Drop both, not just the correction.
                queue_.erase(recorded);
                return;
            }
            if (std::ranges::any_of(queue_, [&event](const Pending& pending) {
                const auto* discard = std::get_if<protocol::DiscardCommitRequest>(&pending.message);
                return discard && discard->event_id == event;
            })) return;
        }
        if (stopping_) {
            if (callback) {
                rejected_callback = std::move(callback);
                rejected_response = protocol::Message{correlation_error(
                    message, protocol::ErrorCode::ServiceShuttingDown, "transport is stopped")};
            }
        } else if (queue_.size() >= (kind == RequestKind::DiscardCommit
                   ? options_.max_pending_requests * 2 + 1 : options_.max_pending_requests)) {
            // Keep accepted work in FIFO order, especially Record/Discard.
            // Collection is best effort; callers with callbacks get a correlated error.
            rejected_response = protocol::Message{correlation_error(
                message, protocol::ErrorCode::ResourceExhausted, "transport queue is full")};
            rejected_callback = std::move(callback);
        } else {
            queue_.push_back(Pending{kind, std::move(message), std::move(callback)});
        }
    }
    if (superseded) fail(std::move(*superseded), protocol::ErrorCode::ResourceExhausted, "prediction superseded before execution");
    if (rejected_callback) rejected_callback(std::move(*rejected_response));
    if (rejected_response) return;
    condition_.notify_one();
}

void ServiceTransport::record_commit(protocol::RecordCommitRequest request) {
    enqueue(RequestKind::RecordCommit, std::move(request), {});
}

void ServiceTransport::discard_commit(protocol::SessionId event_id) {
    enqueue(RequestKind::DiscardCommit, protocol::DiscardCommitRequest{event_id}, {});
}

void ServiceTransport::run(std::stop_token stop) {
    while (true) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, stop, [this]() { return stopping_ || !queue_.empty(); });
            // stop() has already shut down the socket to unblock recv. Fail
            // queued work below instead of reconnecting while draining it.
            if (stopping_ || stop.stop_requested()) break;
            pending = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            if (!ensure_connected()) throw std::system_error(ENOENT, std::generic_category(), "service is unavailable");
            bool stopped = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopped = stopping_;
            }
            if (stopped) {
                disconnect();
                fail(std::move(pending), protocol::ErrorCode::ServiceShuttingDown,
                     "transport is stopped");
                continue;
            }
            const auto bytes = protocol::encode(pending.message);
            const auto timeout = pending.kind == RequestKind::Predict && cold_prediction_
                ? options_.cold_prediction_timeout : options_.request_timeout;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            if (!socket_io::write_all(socket_fd_, bytes, deadline)) throw std::system_error(errno, std::generic_category(), "write service frame");
            const auto response = protocol::decode(recv_frame(socket_fd_, deadline));
            if (pending.kind == RequestKind::Predict) cold_prediction_ = false;
            if (!matches(pending.kind, pending.message, response)) {
                // A complete, decoded frame keeps the stream aligned. Reject
                // the stale result without applying it to another request.
                fail(std::move(pending), protocol::ErrorCode::ProtocolError, "service response correlation mismatch");
            } else {
                observe_response(response);
                if (pending.callback) pending.callback(response);
            }
        } catch (const protocol::ProtocolError& error) {
            disconnect();
            fail(std::move(pending), protocol::ErrorCode::ProtocolError, error.what());
        } catch (const std::exception& error) {
            disconnect();
            bool stopping = false;
            { std::lock_guard<std::mutex> lock(mutex_); stopping = stopping_; }
            fail(std::move(pending), stopping ? protocol::ErrorCode::ServiceShuttingDown : protocol::ErrorCode::ModelError, error.what());
        }
    }
    disconnect();
    std::deque<Pending> remaining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining.swap(queue_);
    }
    while (!remaining.empty()) {
        fail(std::move(remaining.front()), protocol::ErrorCode::ServiceShuttingDown, "transport is stopped");
        remaining.pop_front();
    }
}

bool ServiceTransport::ensure_connected() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_ && socket_fd_ >= 0) return true;
    }

    const auto connect = [this]() {
        return socket_io::connect(options_.socket_path, std::chrono::steady_clock::now() + options_.connect_timeout);
    };
    int fd = connect();
    const bool spawn_allowed = options_.auto_start && !options_.service_path.empty() &&
                               std::chrono::steady_clock::now() >= spawn_backoff_until_;
    if (fd < 0 && spawn_allowed) {
        std::vector<std::string> arguments;
        arguments.emplace_back(options_.service_path.string());
        arguments.emplace_back("--socket");
        arguments.emplace_back(options_.socket_path.string());
        if (!options_.model_path.empty()) { arguments.emplace_back("--model"); arguments.emplace_back(options_.model_path.string()); }
        if (!options_.tables_dir.empty()) { arguments.emplace_back("--tables"); arguments.emplace_back(options_.tables_dir.string()); }
        arguments.emplace_back("--context-length"); arguments.emplace_back(std::to_string(options_.context_length));
        arguments.emplace_back("--threads"); arguments.emplace_back(std::to_string(options_.threads));
        arguments.emplace_back("--gpu-layers"); arguments.emplace_back(options_.gpu_layers == -2 ? "auto" : std::to_string(options_.gpu_layers));
        arguments.emplace_back("--max-sessions"); arguments.emplace_back(std::to_string(options_.max_sessions));
        arguments.emplace_back("--max-idle-sessions"); arguments.emplace_back(std::to_string(options_.max_idle_sessions));
        arguments.emplace_back("--max-concurrent-predictions"); arguments.emplace_back(std::to_string(options_.max_concurrent_predictions));
        arguments.emplace_back("--idle-timeout"); arguments.emplace_back(std::to_string(options_.idle_timeout_seconds));
        if (launch_service(arguments)) {
            for (int attempt = 0; attempt < 40 && fd < 0; ++attempt) {
                { std::lock_guard<std::mutex> lock(mutex_); if (stopping_) return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                fd = connect();
            }
        }
    }
    if (fd < 0 && spawn_allowed) {
        spawn_backoff_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    if (fd < 0) return false;

    UniqueFd connection(fd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        // Publish the descriptor before the handshake so stop() can wake it.
        socket_fd_ = connection.release();
    }
    try {
        const auto deadline = std::chrono::steady_clock::now() + options_.handshake_timeout;
        const auto status_request = protocol::encode(protocol::Message{protocol::StatusRequest{std::nullopt}});
        if (!socket_io::write_all(fd, status_request, deadline)) throw std::runtime_error("failed to query service epoch");
        const auto response = protocol::decode(recv_frame(fd, deadline));
        const auto* status = std::get_if<protocol::StatusResponse>(&response);
        if (status == nullptr) throw protocol::ProtocolError("service epoch query returned an unexpected response");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (epoch_ && *epoch_ != status->service_epoch) {
                // Session IDs are process-local.  The frontend will receive UNKNOWN_SESSION
                // and lazily open a replacement on its next prediction.
            }
            epoch_ = status->service_epoch;
            if (stopping_) return false;
            connected_ = true;
            cold_prediction_ = !status->model_loaded;
        }
        return true;
    } catch (...) {
        disconnect();
        return false;
    }
}

void ServiceTransport::disconnect() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ >= 0) {
        ::shutdown(socket_fd_, SHUT_RDWR);
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_ = false;
}

void ServiceTransport::shutdown_service() noexcept {
    if (!options_.auto_start) return;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        const UniqueFd connection(socket_io::connect(options_.socket_path, deadline));
        if (!connection.valid()) return;
        const auto bytes = protocol::encode(protocol::Message{protocol::ShutdownRequest{}});
        (void)socket_io::write_all(connection.get(), bytes, deadline);
        ::shutdown(connection.get(), SHUT_RDWR);
    } catch (...) {
    }
    // The service unlinks its socket while exiting. Wait briefly for that so a
    // reconfigured transport does not connect to the process going away. This
    // must not probe with connect(): a peer that does not accept would block
    // the caller once its backlog is full.
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code error;
        if (!std::filesystem::exists(options_.socket_path, error) || error) return;
        ::usleep(50000);
    }
}

void ServiceTransport::fail(Pending pending, protocol::ErrorCode code, std::string message) {
    if (pending.callback) pending.callback(protocol::Message{correlation_error(pending.message, code, std::move(message))});
}

bool ServiceTransport::matches(RequestKind kind, const protocol::Message& request, const protocol::Message& response) {
    if (const auto* error = std::get_if<protocol::Error>(&response)) {
        if (const auto* sent = std::get_if<protocol::PredictRequest>(&request))
            return error->session_id == sent->session_id && error->request_id == sent->request_id &&
                   error->buffer_revision == sent->buffer_revision;
        if (const auto* sent = std::get_if<protocol::CloseSessionRequest>(&request))
            return error->session_id == sent->session_id;
        return true;
    }
    switch (kind) {
        case RequestKind::Open: return std::holds_alternative<protocol::OpenSessionResponse>(response);
        case RequestKind::Predict: {
            const auto* sent = std::get_if<protocol::PredictRequest>(&request);
            const auto* received = std::get_if<protocol::Prediction>(&response);
            return sent && received && sent->session_id == received->session_id && sent->request_id == received->request_id && sent->buffer_revision == received->buffer_revision;
        }
        case RequestKind::Close: {
            const auto* sent = std::get_if<protocol::CloseSessionRequest>(&request);
            const auto* received = std::get_if<protocol::CloseSessionResponse>(&response);
            return sent && received && sent->session_id == received->session_id;
        }
        case RequestKind::Status: return std::holds_alternative<protocol::StatusResponse>(response);
        case RequestKind::Shutdown: return std::holds_alternative<protocol::ShutdownResponse>(response);
        case RequestKind::RecordCommit: {
            const auto* sent = std::get_if<protocol::RecordCommitRequest>(&request);
            const auto* received = std::get_if<protocol::RecordCommitResponse>(&response);
            return sent && received && sent->event_id == received->event_id;
        }
        case RequestKind::DiscardCommit: {
            const auto* sent = std::get_if<protocol::DiscardCommitRequest>(&request);
            const auto* received = std::get_if<protocol::DiscardCommitResponse>(&response);
            return sent && received && sent->event_id == received->event_id;
        }
    }
    return false;
}

protocol::Error ServiceTransport::correlation_error(const protocol::Message& request, protocol::ErrorCode code, std::string message) {
    protocol::Error result;
    result.code = code;
    if (const auto* prediction = std::get_if<protocol::PredictRequest>(&request)) {
        result.session_id = prediction->session_id;
        result.request_id = prediction->request_id;
        result.buffer_revision = prediction->buffer_revision;
    } else if (const auto* close = std::get_if<protocol::CloseSessionRequest>(&request)) {
        result.session_id = close->session_id;
    }
    result.message = std::move(message);
    return result;
}

void ServiceTransport::observe_response(const protocol::Message& response) {
    if (const auto* value = std::get_if<protocol::StatusResponse>(&response)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch_ && *epoch_ != value->service_epoch) {
            // Keep the new epoch; callers validate their session IDs against the server response.
        }
        epoch_ = value->service_epoch;
    } else if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&response)) {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_ = opened->service_epoch;
    }
}

}  // namespace llavon::ime

#endif

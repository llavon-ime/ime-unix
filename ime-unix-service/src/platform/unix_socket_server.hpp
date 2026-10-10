#pragma once

#include "../engine/core_runtime.hpp"
#include "../session/session_manager.hpp"
#include "server_strategy.hpp"
#include "training/commit_store.hpp"
#include "worker_pool.hpp"

#include <filesystem>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ime::unix_service {

struct UnixServerOptions {
    RuntimeConfig runtime;
    SessionLimits limits;
    std::optional<std::filesystem::path> socket_path;
    std::optional<std::filesystem::path> pid_path;
    std::size_t max_connections = 128;
    std::size_t max_queued_requests = 128;
};

class UnixSocketServer final : public ServerStrategy {
public:
    explicit UnixSocketServer(UnixServerOptions options);
    ~UnixSocketServer() override;

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    const char* name() const override;
    int run() override;

    static std::filesystem::path default_socket_path();
    static std::filesystem::path default_pid_path();

private:
    class Connection;
    struct ConnectionTask {
        std::shared_ptr<Connection> connection;
        std::shared_ptr<std::atomic_bool> finished;
        std::jthread thread;
    };

    void request_stop() noexcept;
    void accept_connections();
    void close_connections() noexcept;
    void reap_connections();
    void cleanup_endpoint() noexcept;
    bool record_commit(const protocol::RecordCommitRequest& request);
    bool discard_commit(const protocol::SessionId& event_id);
    // Writes staged commits whose correction window elapsed, or all of them on
    // shutdown.
    void settle_staged_commits(bool all);

    UnixServerOptions options_;
    std::shared_ptr<CoreRuntime> runtime_;
    std::unique_ptr<SessionManager> sessions_;
    std::unique_ptr<WorkerPool> workers_;
    std::mutex commit_mutex_;
    std::unique_ptr<CommitStore> commits_;
    int listen_fd_ = -1;
    std::filesystem::path socket_path_;
    std::filesystem::path pid_path_;
    bool endpoint_owned_ = false;
    bool pid_owned_ = false;
    std::atomic_bool stopping_{false};

    std::mutex connections_mutex_;
    std::vector<ConnectionTask> connections_;
};

}  // namespace ime::unix_service

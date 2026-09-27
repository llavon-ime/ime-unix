#pragma once

#include "context/marker_transaction.hpp"
#include "memory/process_memory.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_set>

namespace llavon::ime::memory {
// All calls except post/discover/capture belong to the host thread.
class FocusProbe {
public:
    struct Hooks {
        std::function<HostContext(ContextId)> snapshot;
        std::function<bool(ContextId)> eligible;
        std::function<void(ContextId, std::u16string_view)> insert;
        std::function<void(ContextId, unsigned)> remove;
        std::function<void(std::function<void()>)> post;
        std::function<void(std::string)> status;
        std::function<Discovery(std::string_view, std::stop_token)> discover = memory::discover;
        std::function<Capture(const std::vector<Process>&, std::u16string_view, std::size_t,
                              std::u16string_view, std::stop_token)> capture = memory::capture;
    };
    explicit FocusProbe(Hooks hooks);
    ~FocusProbe();
    void focus(ContextId context, std::string program);
    void invalidate(ContextId context);
    void observe(ContextId context, const HostContext& fresh);
    void tick();
    // Queues keys only while editing. Replay(true) means restoration was
    // confirmed; replay(false) must forward the original key to its client.
    bool defer_key(ContextId context, std::function<void(bool)> replay);
    HostContext sample(ContextId context);
    bool active() const;
private:
    using Clock = std::chrono::steady_clock;
    using Ticket = MarkerTransaction::Ticket;
    void submit(std::function<void(std::stop_token)> job);
    void abort(std::string status);
    void drain(bool restored);
    bool usable(Ticket ticket) const;
    void scan(Ticket ticket);
    Hooks hooks_;
    MarkerTransaction transaction_;
    std::string program_;
    std::unordered_set<std::string> failed_programs_;
    bool waiting_snapshot_ = false;
    bool preparing_ = false;
    HostContext initial_;
    std::vector<Process> processes_;
    Clock::time_point deadline_{};
    std::vector<std::function<void(bool)>> deferred_keys_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::stop_source job_cancel_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::function<void(std::stop_token)> job_;
    std::stop_token job_token_;
    std::jthread worker_;
};
} // namespace llavon::ime::memory

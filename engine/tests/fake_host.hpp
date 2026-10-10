#pragma once

#include "host/host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace llavon::ime::test {

// Host test double: records commits and redraws, returns a configurable
// context, and queues post() bodies so tests can run them on the thread that
// owns the engine (mirroring a real host's main loop).
class FakeHost final : public Host {
public:
    ~FakeHost() override;
    void post(std::function<void()> body) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(body));
        condition_.notify_all();
    }

    void commit(ContextId context, std::u16string_view text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        commits_.emplace_back(context, std::u16string(text));
        condition_.notify_all();
    }

    void update_ui(ContextId context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++redraw_count_;
        last_redraw_context_ = context;
        condition_.notify_all();
    }

    HostContext surrounding_text(ContextId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return surrounding_;
    }

    bool is_sensitive(ContextId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sensitive_;
    }

    std::vector<int> probe_processes(ContextId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return probe_pids_;
    }

    int focused_probe_process(ContextId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focused_probe_pid_;
    }

    void set_focused_probe_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        focused_probe_pid_ = pid;
    }

    std::string program(ContextId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return program_;
    }

    void set_program(std::string program) {
        std::lock_guard<std::mutex> lock(mutex_);
        program_ = std::move(program);
    }

    void set_probe_pids(std::vector<int> pids) {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_pids_ = std::move(pids);
    }

    void set_surrounding(HostContext context) {
        std::lock_guard<std::mutex> lock(mutex_);
        surrounding_ = std::move(context);
    }

    void set_sensitive(bool sensitive) {
        std::lock_guard<std::mutex> lock(mutex_);
        sensitive_ = sensitive;
    }

    int redraw_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return redraw_count_;
    }

    ContextId last_redraw_context() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_redraw_context_;
    }

    std::vector<std::pair<ContextId, std::u16string>> commits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commits_;
    }

    bool has_pending_posts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty();
    }

    // Runs queued post() bodies on the calling thread until `predicate` holds
    // or the timeout elapses. The predicate is evaluated without holding the
    // internal lock.
    template <typename Predicate>
    bool pump_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (predicate()) return true;
            std::function<void()> body;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    if (!condition_.wait_until(lock, deadline, [&]() { return !queue_.empty(); })) {
                        // The predicate locks the same mutex through the
                        // accessors, so it must run without the lock held.
                        lock.unlock();
                        return predicate();
                    }
                }
                body = std::move(queue_.front());
                queue_.pop();
            }
            body();
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::pair<ContextId, std::u16string>> commits_;
    HostContext surrounding_;
    bool sensitive_ = false;
    std::vector<int> probe_pids_;
    int focused_probe_pid_ = 0;
    std::string program_;
    int redraw_count_ = 0;
    ContextId last_redraw_context_ = 0;
};

}  // namespace llavon::ime::test

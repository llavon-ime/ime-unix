#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ime::unix_service {

// Shared bounded executor. Accepted tasks drain on shutdown; overload never
// blocks a connection reader and never silently evicts earlier tasks.
class WorkerPool final {
public:
    WorkerPool(std::size_t count, std::size_t capacity) : capacity_(capacity) {
        try {
            for (std::size_t i = 0; i < std::max<std::size_t>(1, count); ++i)
                workers_.emplace_back([this]() { run(); });
        } catch (...) { shutdown(); throw; }
    }
    ~WorkerPool() { shutdown(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    bool enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || queue_.size() >= capacity_) return false;
            queue_.push(std::move(task));
        }
        condition_.notify_one();
        return true;
    }

    void shutdown() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        condition_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            try { task(); } catch (...) { /* One task cannot terminate the executor. */ }
        }
    }

    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;
};

}  // namespace ime::unix_service

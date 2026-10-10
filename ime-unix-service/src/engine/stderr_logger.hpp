#pragma once

#include <ime-core/logger.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace ime::unix_service {

// Thread-safe logger that serializes ime-core log messages onto a dedicated
// worker thread and writes them to std::clog.
//
// Messages are dropped unless the LLAVON_IME_DEBUG CMake option is enabled.
//
// Contract guarantees:
//   - log() never blocks the caller and never throws.
//   - A message factory is never evaluated on the calling thread.
//   - When the queue is full or the logger is shutting down, a message
//     factory is never evaluated at all (the moved-in factory is destroyed).
class StderrLogger final : public llavon::ime::core::Logger {
public:
    using llavon::ime::core::Logger::log;

    explicit StderrLogger(std::size_t max_queue = 256);
    ~StderrLogger() override;

    StderrLogger(const StderrLogger&) = delete;
    StderrLogger& operator=(const StderrLogger&) = delete;

    void log(std::string message) noexcept override;
    void log(MessageFactory make_message) noexcept override;

private:
    void worker_loop(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::variant<std::string, MessageFactory>> queue_;
    std::size_t max_queue_;
    std::jthread worker_;
};

}  // namespace ime::unix_service

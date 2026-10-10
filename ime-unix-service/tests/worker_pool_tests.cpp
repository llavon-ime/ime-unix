#include "platform/worker_pool.hpp"

#include <atomic>
#include <cstdlib>
#include <future>

int main() {
    using namespace std::chrono_literals;
    ime::unix_service::WorkerPool pool(1, 2);
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<int> completed{0};
    if (!pool.enqueue([&]() { entered.set_value(); gate.wait(); ++completed; })) return EXIT_FAILURE;
    if (entered_future.wait_for(2s) != std::future_status::ready) { release.set_value(); return EXIT_FAILURE; }
    const bool first = pool.enqueue([&]() { ++completed; });
    const bool second = pool.enqueue([]() { throw 1; });
    const bool overflow = pool.enqueue([&]() { completed = -100; });
    release.set_value();
    pool.shutdown();
    pool.shutdown();
    return first && second && !overflow && completed == 2 && !pool.enqueue([]() {}) ? EXIT_SUCCESS : EXIT_FAILURE;
}

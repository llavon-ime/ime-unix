#include "test_suites.h"

#ifndef _WIN32

#include "config/config.hpp"
#include "engine/service_transport.hpp"
#include "protocol/protocol.hpp"
#include "text/utf.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace llavon::ime;

class Waiter {
public:
    void push(protocol::Message message) {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(message));
        condition_.notify_all();
    }

    bool wait_for(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&]() { return messages_.size() >= count; });
    }

    protocol::Message at(size_t index) {
        std::lock_guard lock(mutex_);
        return messages_.at(index);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<protocol::Message> messages_;
};

void print_error(const protocol::Error& error) {
    std::printf("[real-service] error code=%u message=%s\n", static_cast<unsigned>(error.code),
                error.message.c_str());
}

bool candidates_non_empty(const protocol::Prediction& prediction, size_t index) {
    if (index >= prediction.candidates.size()) return false;
    if (prediction.candidates[index].empty()) return false;
    std::string printable;
    for (const char32_t candidate : prediction.candidates[index]) printable += char32_to_utf8(candidate);
    std::printf("[real-service] padding[%zu] candidates=%s\n", index, printable.c_str());
    return true;
}

}  // namespace

// Runs only when LLAVON_IME_REAL_SERVICE is set. The transport starts an
// isolated backend on the test socket, using the same model and runtime values
// as the configured frontend, then shuts down only that backend.
int run_real_service_tests() {
    const char* enabled = std::getenv("LLAVON_IME_REAL_SERVICE");
    if (enabled == nullptr || enabled[0] == '\0') return EXIT_SUCCESS;

    const auto config = load_config();
    ServiceTransportOptions options;
    if (const char* socket = std::getenv("LLAVON_IME_UNIX_SOCKET_PATH"); socket != nullptr && socket[0] != '\0') {
        options.socket_path = socket;
        // An externally supplied socket is owned by the caller (for example
        // release CI); never send that service a shutdown request.
        options.auto_start = false;
    } else {
        options.socket_path = std::filesystem::temp_directory_path() /
                              ("llavon-ime-real-service-" + std::to_string(getpid()) + ".sock");
    }
    options.model_path = config.model_path;
    options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path();
    options.context_length = static_cast<std::uint32_t>(config.context_length);
    options.threads = static_cast<std::uint32_t>(config.thread_count);
    options.gpu_layers = config.gpu_layers;
    options.idle_timeout_seconds = static_cast<std::uint32_t>(config.idle_timeout_seconds);
    ServiceTransport transport(options);

    Waiter waiter;
    transport.open_session([&waiter](protocol::Message response) { waiter.push(std::move(response)); });
    if (!waiter.wait_for(1, std::chrono::milliseconds(30000))) {
        std::printf("[real-service] no open-session response; is the service running?\n");
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto opened = waiter.at(0);
    if (const auto* error = std::get_if<protocol::Error>(&opened)) {
        print_error(*error);
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto* open_response = std::get_if<protocol::OpenSessionResponse>(&opened);
    if (open_response == nullptr) {
        std::printf("[real-service] unexpected open-session response\n");
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto session = open_response->session_id;
    std::printf("[real-service] session opened\n");

    // Context this IME never committed must still reach the model through
    // the real protocol path.
    const std::u16string context = u"今天天氣很好，我們一起";
    const std::vector<protocol::PaddingEntry> padding{{false, u"ㄕˋ", 0}, {false, u"ㄐㄧㄢˋ", 0}};
    transport.predict(session, 1, 1, context, padding,
                      [&waiter](protocol::Message response) { waiter.push(std::move(response)); });
    if (!waiter.wait_for(2, std::chrono::milliseconds(60000))) {
        std::printf("[real-service] no prediction response within 60s\n");
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto first = waiter.at(1);
    if (const auto* error = std::get_if<protocol::Error>(&first)) {
        print_error(*error);
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto* prediction = std::get_if<protocol::Prediction>(&first);
    if (prediction == nullptr || prediction->candidates.size() != padding.size() ||
        !candidates_non_empty(*prediction, 0) || !candidates_non_empty(*prediction, 1)) {
        std::printf("[real-service] prediction did not contain candidates for every padding entry\n");
        transport.stop();
        return EXIT_FAILURE;
    }

    // A second request with new context exercises the service's incremental
    // token cache alignment against the model.
    const std::u16string next_context = u"今天天氣很好，我們一起來寫程式";
    transport.predict(session, 2, 2, next_context, padding,
                      [&waiter](protocol::Message response) { waiter.push(std::move(response)); });
    if (!waiter.wait_for(3, std::chrono::milliseconds(60000))) {
        std::printf("[real-service] no second prediction within 60s\n");
        transport.stop();
        return EXIT_FAILURE;
    }
    const auto second = waiter.at(2);
    if (const auto* error = std::get_if<protocol::Error>(&second)) {
        print_error(*error);
        transport.stop();
        return EXIT_FAILURE;
    }
    if (const auto* next_prediction = std::get_if<protocol::Prediction>(&second);
        next_prediction == nullptr || !candidates_non_empty(*next_prediction, 0) ||
        !candidates_non_empty(*next_prediction, 1)) {
        std::printf("[real-service] second prediction did not contain candidates\n");
        transport.stop();
        return EXIT_FAILURE;
    }

    transport.close_session(session, [&waiter](protocol::Message response) { waiter.push(std::move(response)); });
    (void)waiter.wait_for(4, std::chrono::milliseconds(10000));
    transport.stop();
    std::printf("[real-service] real service test passed\n");
    return EXIT_SUCCESS;
}

#else

int run_real_service_tests() { return EXIT_SUCCESS; }

#endif

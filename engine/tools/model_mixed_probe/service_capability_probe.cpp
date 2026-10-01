// Observe the existing service API in isolation. This tool does not select
// mixed-input boundaries, modify the engine, or record training samples.
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {
using namespace llavon::ime;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct SocketDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                ("service-capability-" + std::to_string(getpid()));
    SocketDirectory() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("socket directory already exists");
    }
    ~SocketDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::vector<protocol::PaddingEntry> padding_for(const Json& query, const FallbackEngine& fallback) {
    std::vector<protocol::PaddingEntry> result;
    for (const auto& entry : query.at("padding")) {
        if (entry.contains("chosen")) {
            const auto character = entry.at("chosen").get<std::string>();
            const auto scalars = utf8_to_u32(character);
            if (scalars.size() != 1) throw std::runtime_error("chosen must contain one Unicode scalar");
            result.push_back({true, {}, scalars.front()});
        } else {
            auto reading = utf8_to_u16(entry.at("reading").get<std::string>());
            if (fallback.lookup(reading).empty()) throw std::runtime_error("reading has no fallback candidates");
            result.push_back({false, std::move(reading), 0});
        }
    }
    if (result.empty()) throw std::runtime_error("query padding is empty");
    return result;
}

class Observer {
public:
    explicit Observer(ServiceTransportOptions options) : transport_(std::move(options)) {}

    protocol::SessionId open() {
        return std::get<protocol::OpenSessionResponse>(exchange([&](auto callback) {
            transport_.open_session(std::move(callback));
        })).session_id;
    }
    void close(const protocol::SessionId& session) {
        const auto response = exchange([&](auto callback) {
            transport_.close_session(session, std::move(callback));
        });
        const auto* closed = std::get_if<protocol::CloseSessionResponse>(&response);
        if (!closed || closed->session_id != session || !closed->closed) throw std::runtime_error("session close failed");
    }
    Json predict(const protocol::SessionId& session, const Json& query,
                 const std::vector<protocol::PaddingEntry>& padding, const FallbackEngine& fallback) {
        const auto id = ++next_request_;
        const auto start = Clock::now();
        const auto response = exchange([&](auto callback) {
            transport_.predict(session, id, id, utf8_to_u16(query.at("context").get<std::string>()),
                               padding, std::move(callback));
        });
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        const auto* prediction = std::get_if<protocol::Prediction>(&response);
        if (!prediction || prediction->session_id != session || prediction->request_id != id ||
            prediction->buffer_revision != id || prediction->candidates.size() != padding.size()) {
            throw std::runtime_error("invalid prediction correlation/shape");
        }
        Json candidates = Json::array();
        Json unresolved = Json::array();
        std::string text;
        for (std::size_t i = 0; i < padding.size(); ++i) {
            const auto& values = prediction->candidates[i];
            if (values.empty()) throw std::runtime_error("prediction returned empty candidates");
            if (padding[i].chosen() && (values.size() != 1 || values.front() != padding[i].chosen_char())) {
                throw std::runtime_error("service failed to preserve chosen character");
            }
            const auto allowed = padding[i].chosen() ? values : fallback.lookup(padding[i].bopomofo());
            std::string encoded;
            for (const auto value : values) {
                if (!protocol::valid_scalar(value) || std::ranges::find(allowed, value) == allowed.end()) {
                    throw std::runtime_error("prediction returned a non-reading candidate");
                }
                encoded += char32_to_utf8(value);
            }
            candidates.push_back(encoded);
            if (!padding[i].chosen()) unresolved.push_back(encoded);
            text += char32_to_utf8(values.front());
        }
        return {{"id", query.at("id")}, {"text", text}, {"candidates", candidates},
                {"unresolved_candidates", unresolved}, {"latency_us", elapsed}};
    }

private:
    protocol::Message exchange(const std::function<void(ServiceTransport::Callback)>& send) {
        auto promise = std::make_shared<std::promise<protocol::Message>>();
        auto future = promise->get_future();
        send([promise](auto response) { promise->set_value(std::move(response)); });
        if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
            throw std::runtime_error("service response timeout");
        }
        auto response = future.get();
        if (const auto* error = std::get_if<protocol::Error>(&response)) {
            throw std::runtime_error("service: " + error->message);
        }
        return response;
    }
    ServiceTransport transport_;
    std::uint64_t next_request_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: service_capability_probe SERVICE MODEL QUERIES_JSON REPORT_JSON\n");
        return 1;
    }
    try {
        const std::vector<std::string> arguments(argv, argv + argc);
        std::ifstream input(arguments[3]);
        if (!input) throw std::runtime_error("cannot read queries");
        const auto queries = Json::parse(input).at("queries");
        if (!queries.is_array() || queries.empty()) throw std::runtime_error("queries must be a nonempty array");
        const FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
        std::vector<std::vector<protocol::PaddingEntry>> paddings;
        for (const auto& query : queries) paddings.push_back(padding_for(query, fallback));

        const SocketDirectory directory;
        ServiceTransportOptions options;
        options.socket_path = directory.path / "service.sock";
        options.service_path = arguments[1];
        options.model_path = arguments[2];
        options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path();
        options.idle_timeout_seconds = 120;
        Observer observer(options);
        auto session = observer.open();
        const auto cold = observer.predict(session, queries.at(0), paddings.at(0), fallback);
        Json rows = Json::array();
        // Reversing request history and using a fresh session distinguishes
        // token equivalence from accidental KV-cache/history contamination.
        for (const std::string phase : {"reused_forward", "reused_reverse", "fresh_forward"}) {
            for (std::size_t index = 0; index < queries.size(); ++index) {
                const auto at = phase == "reused_reverse" ? queries.size() - index - 1 : index;
                if (phase == "fresh_forward") session = observer.open();
                auto row = observer.predict(session, queries.at(at), paddings.at(at), fallback);
                row["phase"] = phase;
                rows.push_back(std::move(row));
                if (phase == "fresh_forward") observer.close(session);
            }
            if (phase == "reused_reverse") observer.close(session);
        }
        std::ofstream output(arguments[4]);
        if (!output) throw std::runtime_error("cannot create report");
        output << Json{{"service", arguments[1]}, {"model", arguments[2]},
                       {"cold_first_prediction_us", cold.at("latency_us")}, {"rows", rows}}.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write report");
        std::printf("Observed %zu queries in three request-history phases.\n", queries.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "capability experiment failed: %s\n", error.what());
        return 1;
    }
}

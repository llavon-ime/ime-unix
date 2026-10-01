// Real Engine -> Unix service -> model -> render/commit latency experiment.
#include "rawkey/raw_key_harness.hpp"
#include "engine/service_transport.hpp"
#include "text/utf.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {
using namespace llavon::ime;
using namespace llavon::ime::rawkey;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

protocol::Message exchange(ServiceTransport& transport,
                           const std::function<void(ServiceTransport::Callback)>& send) {
    (void)transport;
    auto promise = std::make_shared<std::promise<protocol::Message>>();
    auto future = promise->get_future();
    send([promise](auto response) { promise->set_value(std::move(response)); });
    if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
        throw std::runtime_error("service timeout");
    auto response = future.get();
    if (const auto* error = std::get_if<protocol::Error>(&response))
        throw std::runtime_error("service: " + error->message);
    return response;
}

Json run(const std::vector<std::string>& args) {
    std::ifstream input(args.at(3));
    const auto spec = Json::parse(input);
    const auto directory = std::filesystem::temp_directory_path() /
                           ("inference-rawkey-" + std::to_string(getpid()));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("scratch directory exists");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    // Isolate synthetic commits and inherited frontend overrides in this process.
    (void)::setenv("XDG_CONFIG_HOME", (directory / "config").c_str(), 1);
    (void)::setenv("XDG_STATE_HOME", (directory / "state").c_str(), 1);
    (void)::setenv("LLAVON_IME_TRAINING_DATABASE_PATH", (directory / "commits.sqlite3").c_str(), 1);
    (void)::setenv("LLAVON_IME_MODEL_PATH", args.at(2).c_str(), 1);
    (void)::setenv("LLAVON_IME_THREADS", std::to_string(spec.at("threads").get<int>()).c_str(), 1);
    (void)::setenv("LLAVON_IME_GPU_LAYERS", std::to_string(spec.at("layers").get<int>()).c_str(), 1);
    HarnessOptions options;
    options.service_path = args.at(1);
    options.model_path = args.at(2);
    options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path().string();
    options.socket_path = (directory / "service.sock").string();
    options.config.model_path = options.model_path;
    options.config.thread_count = spec.at("threads").get<int>();
    options.config.gpu_layers = spec.at("layers").get<int>();
    options.config.smart_english = false;
    ServiceTransportOptions transport_options;
    transport_options.socket_path = options.socket_path;
    transport_options.service_path = options.service_path;
    transport_options.model_path = options.model_path;
    transport_options.tables_dir = options.tables_dir;
    transport_options.threads = static_cast<std::uint32_t>(options.config.thread_count);
    transport_options.gpu_layers = options.config.gpu_layers;
    transport_options.idle_timeout_seconds = 120;
    Json result = {{"spec", spec}, {"records", Json::array()}, {"preedits", Json::object()}};
    {
        ServiceTransport owner(transport_options);
        const auto start = Clock::now();
        const auto opened = exchange(owner, [&](auto callback) { owner.open_session(std::move(callback)); });
        result["service_open_ms"] = elapsed_ms(start);
        const auto session = std::get<protocol::OpenSessionResponse>(opened).session_id;
        (void)exchange(owner, [&](auto callback) { owner.close_session(session, std::move(callback)); });
        {
            Harness harness(options);
            const auto wait_prediction = [&] {
                // Harness::settle_prediction() is an offline-test cancellation
                // helper, not a wait. Pump the actual host/model response queue.
                if (!harness.pump_until([&] {
                        const auto* state = harness.session();
                        return state != nullptr && !state->prediction.pending;
                    }, std::chrono::seconds(60)))
                    throw std::runtime_error("model prediction did not settle");
            };
            const int samples = spec.value("samples", 40);
            const std::array<std::string, 3> sequences = {"su3", "su3cl3", "su3cl3su3cl3su3cl3"};
            const std::array<std::string, 4> contexts = {"今天我想說", "明天我想說", "昨天我想說", "現在我想說"};
            const bool paced = spec.value("paced", false);
            for (std::size_t workload = 0; workload < sequences.size(); ++workload) {
                const auto& keys = sequences[workload];
                for (int i = -4; i < samples; ++i) {
                    harness.reset();
                    const auto& context = contexts.at(static_cast<std::size_t>((i + 4) % 4));
                    const auto context_length = utf8_to_u16(context).size();
                    harness.set_surrounding(context, context_length, context_length);
                    for (std::size_t key = 0; key + 1 < keys.size(); ++key) {
                        harness.key(Key(keys.at(key)));
                        if (paced && keys.at(key) == '3') wait_prediction();
                    }
                    const auto tone_start = Clock::now();
                    harness.key(Key(keys.back()));
                    wait_prediction();
                    const double latency = elapsed_ms(tone_start);
                    if (workload == 0 && i == -4) result["first_model_render_ms"] = latency;
                    const auto status_message = exchange(owner, [&](auto callback) {
                        owner.status(harness.session()->prediction.session_id, std::move(callback));
                    });
                    const auto& status = std::get<protocol::StatusResponse>(status_message);
                    if (!status.model_loaded || !status.session || !status.session->engine_loaded ||
                        status.session->last_request_id == 0)
                        throw std::runtime_error("raw-key request did not reach a loaded model session");
                    const auto preview = harness.preedit();
                    if (preview.empty()) throw std::runtime_error("empty model preview");
                    harness.expect_commit(preview);
                    if (!harness.composition_empty()) throw std::runtime_error("composition did not clear on commit");
                    const auto key = std::to_string(workload) + ":" + std::to_string((i + 4) % 4);
                    if (result["preedits"].contains(key) && result["preedits"][key] != preview)
                        throw std::runtime_error("model preview changed across repeated identical raw keys");
                    result["preedits"][key] = preview;
                    if (i >= 0) result["records"].push_back({{"workload", workload}, {"index", i},
                                                            {"tone_to_settled_render_ms", latency}});
                }
            }
        }
        // Harness teardown shuts down this private service. Do not reconnect
        // the owner here: that would unnecessarily spawn a second service.
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: rawkey_bench SERVICE MODEL SPEC_JSON OUTPUT_JSON");
        const std::vector<std::string> args(argv, argv + argc);
        const auto result = run(args);
        std::ofstream output(args.at(4));
        output << result.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write raw-key observations");
        return 0;
    } catch (const Failure& failure) {
        std::cerr << "raw-key assertion failed: " << failure.message << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "raw-key benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

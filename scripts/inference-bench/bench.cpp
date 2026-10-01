// Experimental harness: uses the pinned, unmodified production ime-core.
#include <ime-core/core.hpp>
#include <ime-core/logger.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace llavon::ime::core;

class SilentLogger final : public Logger {
public:
    void log(std::string) noexcept override {}
    void log(MessageFactory) noexcept override {}
};

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json scheduling(const Json& spec) {
    Json result = {{"requested", spec.value("qos", "default")},
                   {"uclamp_requested", spec.value("uclamp", 0)}};
#if defined(__APPLE__)
    const auto name = spec.value("qos", "default");
    qos_class_t qos = QOS_CLASS_DEFAULT;
    if (name == "initiated") qos = QOS_CLASS_USER_INITIATED;
    else if (name == "interactive") qos = QOS_CLASS_USER_INTERACTIVE;
    else if (name == "background") qos = QOS_CLASS_BACKGROUND;
    else if (name != "default") throw std::runtime_error("unknown QoS");
    const int rc = pthread_set_qos_class_self_np(qos, 0);
    if (rc != 0) throw std::runtime_error("cannot set QoS: " + std::to_string(rc));
    int relative_priority = 0;
    qos_class_t effective = QOS_CLASS_UNSPECIFIED;
    const int query_rc = pthread_get_qos_class_np(pthread_self(), &effective, &relative_priority);
    result["query_rc"] = query_rc;
    result["effective"] = static_cast<unsigned int>(effective);
    // macOS thermal-pressure notification state (diagnostic, not a temperature).
    int thermal = -1;
    auto size = sizeof(thermal);
    if (sysctlbyname("kern.thermal_pressure", &thermal, &size, nullptr, 0) == 0)
        result["thermal_pressure"] = thermal;
#elif defined(__linux__)
    // Kernel ABI layout for sched_getattr/sched_setattr; retain existing policy.
    struct SchedAttr {
        std::uint32_t size = sizeof(SchedAttr);
        std::uint32_t policy = 0;
        std::uint64_t flags = 0;
        std::int32_t nice = 0;
        std::uint32_t priority = 0;
        std::uint64_t runtime = 0;
        std::uint64_t deadline = 0;
        std::uint64_t period = 0;
        std::uint32_t util_min = 0;
        std::uint32_t util_max = 1024;
    } attr;
    if (syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0) != 0)
        throw std::runtime_error("sched_getattr failed: " + std::to_string(errno));
    const auto minimum = spec.value("uclamp", 0U);
    if (minimum > 1024) throw std::runtime_error("uclamp out of range");
    if (spec.contains("uclamp")) {
        attr.util_min = minimum;
        attr.flags = 0x20U; // SCHED_FLAG_UTIL_CLAMP_MIN; no policy/priority change.
        if (syscall(SYS_sched_setattr, 0, &attr, 0) != 0)
            throw std::runtime_error("sched_setattr failed: " + std::to_string(errno));
    }
    if (syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0) != 0)
        throw std::runtime_error("sched_getattr verification failed");
    result["uclamp_effective"] = attr.util_min;
    result["policy"] = attr.policy;
#endif
    return result;
}

std::vector<PaddingEntry> readings(std::initializer_list<std::u16string> input) {
    std::vector<PaddingEntry> entries;
    for (const auto& reading : input) entries.push_back({false, 0, reading});
    return entries;
}

struct Workload {
    std::string id;
    std::u16string context;
    std::vector<PaddingEntry> padding;
};

std::vector<Workload> workloads() {
    std::u16string long_context;
    for (int i = 0; i < 12; ++i)
        long_context += u"今天我們討論輸入法的效能，並且比較不同電腦的推論速度。";
    return {
        {"one", u"我想告訴", readings({u"ㄋㄧˇ"})},
        {"two", u"我想跟你說", readings({u"ㄋㄧˇ", u"ㄏㄠˇ"})},
        {"six", u"氣象預報說", readings({u"ㄐㄧㄣ ", u"ㄊㄧㄢ ", u"ㄊㄧㄢ ", u"ㄑㄧˋ", u"ㄏㄣˇ", u"ㄏㄠˇ"})},
        {"long", std::move(long_context), readings({u"ㄒㄧㄣ ", u"ㄒㄧㄤ "})},
    };
}

Json signature(const std::vector<Prediction>& predictions, const Workload& work, const Json& table) {
    if (predictions.size() != work.padding.size()) throw std::runtime_error("segment count changed");
    Json ranks = Json::array();
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        const auto& candidates = predictions[i].candidates;
        if (candidates.empty()) throw std::runtime_error("empty candidates");
        Json codes = Json::array();
        for (const auto& [code, probability] : candidates) {
            if (!std::isfinite(probability) || probability < 0 || probability > 1)
                throw std::runtime_error("invalid candidate probability");
            // Reading membership is validated against the canonical UTF-8 table
            // by numeric codepoint sets supplied by the Python driver.
            const auto value = static_cast<std::uint32_t>(code);
            const auto& allowed = table.at(work.id).at(i);
            if (std::find(allowed.begin(), allowed.end(), Json(value)) == allowed.end())
                throw std::runtime_error("candidate has wrong reading");
            codes.push_back(value);
        }
        ranks.push_back(std::move(codes));
    }
    return ranks;
}

Json run(const Json& spec, const std::string& model, const std::string& tables) {
    const auto scheduling_before = scheduling(spec);
    const std::string backend = spec.value("backend", "metal");
    const int layers = spec.value("layers", backend == "cpu" ? 0 : 999);
    const auto load_start = Clock::now();
    Core core(CoreConfig{
        .model_path = model,
        .tables_dir = tables,
        .context_length = 512,
        .threads = spec.at("threads").get<std::uint32_t>(),
        .gpu_layers = layers,
        .inference_device = {.backend = backend == "cpu" ? InferenceBackend::cpu : InferenceBackend::automatic,
                             .device_id = {}},
        .logger = std::make_shared<SilentLogger>(),
    });
    const double load_ms = elapsed_ms(load_start);
    const auto runtime = core.inference_runtime_info();
    if (backend != "cpu" && !runtime.gpu_offload) throw std::runtime_error("GPU experiment fell back to CPU");
    Json report = {{"spec", spec}, {"load_ms", load_ms}, {"device", runtime.device.name},
                   {"backend_effective", static_cast<unsigned int>(runtime.device.backend)},
                   {"gpu_offload", runtime.gpu_offload}, {"scheduling", scheduling_before},
                   {"records", Json::array()}, {"signatures", Json::object()},
                   {"repeat_rank_changes", Json::array()}};
    const int count = spec.value("samples", 40);
    const int idle_ms = spec.value("idle_ms", 0);
    const int lead_ms = spec.value("lead_ms", 0);
    const std::string warmup = spec.value("warmup", "none");
    const bool fresh = spec.value("cache", "reuse") == "fresh";
    const std::string only = spec.value("workload", "all");
    auto work = workloads();
    if (only != "all") std::erase_if(work, [&](const auto& entry) { return entry.id != only; });
    if (work.empty() || count < 1 || idle_ms < 0 || lead_ms < 0)
        throw std::runtime_error("invalid benchmark spec");
    const std::array<std::u16string, 4> suffixes = {u"今天", u"明天", u"昨天", u"現在"};
    for (const auto& workload : work) {
        auto session = core.create_session();
        for (int i = -4; i < count; ++i) {
            double create_ms = 0;
            if (fresh) {
                const auto start = Clock::now();
                session = core.create_session();
                create_ms = elapsed_ms(start);
            }
            const auto variant = static_cast<std::size_t>((i + 4) % 4);
            const auto context = workload.context + suffixes[variant];
            if (i >= 0 && idle_ms != 0) std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
            double ready_ms = 0;
            if (i >= 0 && warmup != "none") {
                const auto start = Clock::now();
                session->ready();
                ready_ms = elapsed_ms(start);
                if (warmup == "ahead") std::this_thread::sleep_for(std::chrono::milliseconds(lead_ms));
            }
            const auto start = Clock::now();
            auto predictions = session->predict(context, workload.padding);
            const double predict_ms = elapsed_ms(start);
            // Validation and JSON conversion happen after the timed inference.
            const auto ranks = signature(predictions, workload, spec.at("allowed"));
            const std::string key = workload.id + ":" + std::to_string(variant);
            if (report["signatures"].contains(key) && report["signatures"][key] != ranks) {
                bool top1_changed = false;
                for (std::size_t position = 0; position < ranks.size(); ++position)
                    top1_changed = top1_changed || report["signatures"][key][position][0] != ranks[position][0];
                report["repeat_rank_changes"].push_back({{"query", key}, {"index", i},
                                                         {"top1_changed", top1_changed}, {"ranks", ranks}});
            }
            if (!report["signatures"].contains(key)) report["signatures"][key] = ranks;
            if (i >= 0) report["records"].push_back({
                {"workload", workload.id}, {"variant", variant}, {"index", i},
                {"predict_ms", predict_ms}, {"ready_ms", ready_ms}, {"create_ms", create_ms},
                {"compute_ms", predict_ms + ready_ms},
            });
        }
    }
    report["scheduling_end"] = scheduling(spec);
    return report;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: inference_bench MODEL TABLES SPEC_JSON OUTPUT_JSON");
        // Avoid pointer arithmetic on argv under the project's strict warnings.
        const std::vector<std::string> args(argv, argv + argc);
        std::ifstream input(args.at(3));
        if (!input) throw std::runtime_error("cannot read spec");
        Json spec;
        input >> spec;
        const auto report = run(spec, args.at(1), args.at(2));
        std::ofstream output(args.at(4));
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write report");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

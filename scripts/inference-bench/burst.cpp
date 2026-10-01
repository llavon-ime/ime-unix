// Request-scoped QoS experiment, including actual disposable ggml CPU workers.
// The pinned core/backend is unchanged. pthread_create is interposed only in
// this standalone executable; RTLD_NEXT invokes the original libSystem API.
#include <ime-core/core.hpp>
#include <ime-core/logger.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <libproc.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace llavon::ime::core;

struct WorkerStats {
    std::atomic<unsigned int> created{0};
    std::atomic<unsigned int> initiated{0};
    std::atomic<unsigned int> interactive{0};
    std::atomic<unsigned int> default_qos{0};
    std::atomic<unsigned int> unspecified{0};
    std::atomic<unsigned int> restored{0};
    std::atomic<unsigned int> errors{0};
    std::atomic<unsigned int> other_threads{0};
    std::atomic<std::uint64_t> cpu_ns{0};
};
WorkerStats stats;
std::atomic<bool> recording{false};
std::atomic<unsigned int> worker_qos{0};

double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t thread_cpu_ns() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(value.tv_nsec);
}

qos_class_t requested_qos() {
    qos_class_t value = QOS_CLASS_UNSPECIFIED;
    int priority = 0;
    if (pthread_get_qos_class_np(pthread_self(), &value, &priority) != 0)
        throw std::runtime_error("cannot query caller QoS");
    return value;
}

void set_qos(qos_class_t value) {
    if (pthread_set_qos_class_self_np(value, 0) != 0)
        throw std::runtime_error("cannot change caller QoS");
    if (requested_qos() != value) throw std::runtime_error("caller QoS verification failed");
}

struct Start {
    void* (*function)(void*);
    void* argument;
    qos_class_t boost;
};

void* start_worker(void* opaque) {
    const std::unique_ptr<Start> start(static_cast<Start*>(opaque));
    qos_class_t previous = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    if (pthread_get_qos_class_np(pthread_self(), &previous, &relative) != 0) ++stats.errors;
    if (start->boost != QOS_CLASS_UNSPECIFIED && pthread_set_qos_class_self_np(start->boost, 0) != 0)
        ++stats.errors;
    qos_class_t observed = QOS_CLASS_UNSPECIFIED;
    if (pthread_get_qos_class_np(pthread_self(), &observed, &relative) != 0) ++stats.errors;
    ++stats.created;
    if (observed == QOS_CLASS_USER_INITIATED) ++stats.initiated;
    else if (observed == QOS_CLASS_USER_INTERACTIVE) ++stats.interactive;
    else if (observed == QOS_CLASS_DEFAULT) ++stats.default_qos;
    else if (observed == QOS_CLASS_UNSPECIFIED) ++stats.unspecified;
    if (start->boost != QOS_CLASS_UNSPECIFIED && observed != start->boost) ++stats.errors;
    const auto cpu_start = thread_cpu_ns();
    void* result = start->function(start->argument);
    stats.cpu_ns.fetch_add(thread_cpu_ns() - cpu_start);
    if (start->boost != QOS_CLASS_UNSPECIFIED) {
        // UNSPECIFIED cannot be requested by the setter; these disposable
        // workers terminate immediately after returning from this trampoline.
        const auto restore = previous == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : previous;
        if (pthread_set_qos_class_self_np(restore, 0) != 0) ++stats.errors;
        qos_class_t after = QOS_CLASS_UNSPECIFIED;
        if (pthread_get_qos_class_np(pthread_self(), &after, &relative) != 0 || after != restore)
            ++stats.errors;
        else ++stats.restored;
    }
    return result;
}

void reset_stats() {
    stats.created = 0; stats.initiated = 0; stats.interactive = 0;
    stats.default_qos = 0; stats.unspecified = 0; stats.restored = 0;
    stats.errors = 0; stats.other_threads = 0; stats.cpu_ns = 0;
}

Json worker_stats() {
    return {{"created", stats.created.load()}, {"initiated", stats.initiated.load()},
            {"interactive", stats.interactive.load()}, {"default", stats.default_qos.load()},
            {"unspecified", stats.unspecified.load()}, {"restored", stats.restored.load()},
            {"errors", stats.errors.load()}, {"other_threads", stats.other_threads.load()},
            {"cpu_ns", stats.cpu_ns.load()}};
}

class SilentLogger final : public Logger {
public:
    void log(std::string) noexcept override {}
    void log(MessageFactory) noexcept override {}
};

double process_cpu_ms() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("cannot read process CPU time");
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000 +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000;
}

struct Accounting {
    rusage_info_v6 values{};
    int rc = -1;
    Accounting() {
        rc = proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&values));
    }
};

Json accounting_delta(const Accounting& before, const Accounting& after) {
    Json result = {{"before_rc", before.rc}, {"after_rc", after.rc}};
    if (before.rc != 0 || after.rc != 0) return result;
    const auto& a = after.values;
    const auto& b = before.values;
    result["cpu_ns"] = a.ri_user_time + a.ri_system_time - b.ri_user_time - b.ri_system_time;
    result["performance_cpu_ns"] = a.ri_user_ptime + a.ri_system_ptime - b.ri_user_ptime - b.ri_system_ptime;
    result["cycles"] = a.ri_cycles - b.ri_cycles;
    result["instructions"] = a.ri_instructions - b.ri_instructions;
    result["initiated_cpu_ns"] = a.ri_cpu_time_qos_user_initiated - b.ri_cpu_time_qos_user_initiated;
    result["interactive_cpu_ns"] = a.ri_cpu_time_qos_user_interactive - b.ri_cpu_time_qos_user_interactive;
    result["energy_nj"] = a.ri_energy_nj - b.ri_energy_nj;
    result["performance_energy_nj"] = a.ri_penergy_nj - b.ri_penergy_nj;
    return result;
}

Json validate(const std::vector<Prediction>& predictions, const Json& allowed) {
    if (predictions.size() != allowed.size()) throw std::runtime_error("wrong prediction count");
    Json top = Json::array();
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        const auto& candidates = predictions[i].candidates;
        if (candidates.empty()) throw std::runtime_error("empty candidates");
        for (const auto& [code, probability] : candidates) {
            if (!std::isfinite(probability) || probability < 0 || probability > 1)
                throw std::runtime_error("invalid probability");
            const auto number = static_cast<std::uint32_t>(code);
            if (std::find(allowed[i].begin(), allowed[i].end(), Json(number)) == allowed[i].end())
                throw std::runtime_error("wrong reading");
        }
        top.push_back(static_cast<std::uint32_t>(candidates.front().first));
    }
    return top;
}

Json run(const std::vector<std::string>& args) {
    std::ifstream input(args.at(3));
    const auto spec = Json::parse(input);
    const std::string mode = spec.at("mode");
    const bool all = mode == "workers" || mode == "static" || mode == "interactive";
    const bool raised = mode != "none";
    if (mode != "none" && mode != "caller" && !all) throw std::runtime_error("invalid mode");
    const auto qos = mode == "interactive" ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED;
    const auto idle_qos = mode == "static" ? qos : QOS_CLASS_DEFAULT;
    set_qos(idle_qos);
    const std::string backend = spec.at("backend");
    Core core(CoreConfig{
        .model_path = args.at(1), .tables_dir = args.at(2), .context_length = 512,
        .threads = spec.at("threads").get<std::uint32_t>(),
        .gpu_layers = backend == "cpu" ? 0 : 999,
        .inference_device = {.backend = backend == "cpu" ? InferenceBackend::cpu : InferenceBackend::metal, .device_id = {}},
        .logger = std::make_shared<SilentLogger>(),
    });
    const auto runtime = core.inference_runtime_info();
    if (backend == "metal" && !runtime.gpu_offload) throw std::runtime_error("GPU fallback");
    auto session = core.create_session();
    const std::vector<PaddingEntry> padding = {{false, 0, u"ㄋㄧˇ"}, {false, 0, u"ㄏㄠˇ"}};
    const std::array<std::u16string, 4> suffixes = {u"今天", u"明天", u"昨天", u"現在"};
    for (const auto& suffix : suffixes) (void)session->predict(u"我想跟你說" + suffix, padding);
    Json result = {{"spec", spec}, {"runtime", runtime.device.name}, {"records", Json::array()},
                   {"signatures", Json::object()}};
    const int samples = spec.at("samples");
    const int idle = spec.at("idle_ms");
    const int burst_length = spec.value("burst_length", 4);
    if (samples < 1 || idle < 0 || burst_length < 1) throw std::runtime_error("invalid counts");
    for (int sample = 0; sample < samples; ++sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(idle));
        reset_stats();
        const double cpu_before = process_cpu_ms();
        const Accounting accounting_before;
        const auto window_start = Clock::now();
        if (raised) set_qos(qos);
        worker_qos = all ? static_cast<unsigned int>(qos) : 0;
        recording = true;
        const double setup_ms = ms(window_start);
        std::vector<std::vector<Prediction>> predictions;
        predictions.reserve(static_cast<std::size_t>(burst_length));
        std::vector<double> latencies;
        for (int request = 0; request < burst_length; ++request) {
            const auto variant = static_cast<std::size_t>((sample * burst_length + request) % 4);
            const auto start = Clock::now();
            predictions.push_back(session->predict(u"我想跟你說" + suffixes[variant], padding));
            latencies.push_back(ms(start));
        }
        const auto restore_start = Clock::now();
        recording = false;
        worker_qos = 0;
        set_qos(idle_qos);
        const double restore_ms = ms(restore_start);
        const double total_ms = ms(window_start);
        const double cpu_ms = process_cpu_ms() - cpu_before;
        const Accounting accounting_after;
        const auto workers = worker_stats();
        if (stats.errors.load() != 0) throw std::runtime_error("worker QoS verification/restore failed");
        if (backend == "cpu" && stats.created.load() == 0)
            throw std::runtime_error("actual ggml workers were not intercepted");
        if (backend == "cpu" && all && stats.restored.load() != stats.created.load())
            throw std::runtime_error("not all disposable workers restored QoS before exit");
        Json signatures = Json::array();
        for (int request = 0; request < burst_length; ++request) {
            const auto variant = static_cast<std::size_t>((sample * burst_length + request) % 4);
            const auto top = validate(predictions.at(static_cast<std::size_t>(request)), spec.at("allowed"));
            const auto key = std::to_string(variant);
            if (result["signatures"].contains(key) && result["signatures"][key] != top)
                throw std::runtime_error("repeated top1 changed");
            result["signatures"][key] = top;
            signatures.push_back(top);
        }
        result["records"].push_back({{"sample", sample}, {"predict_ms", latencies},
            {"setup_ms", setup_ms}, {"restore_ms", restore_ms}, {"total_ms", total_ms},
            {"first_including_setup_ms", setup_ms + latencies.front()}, {"process_cpu_ms", cpu_ms},
            {"idle_qos_after", static_cast<unsigned int>(requested_qos())},
            {"workers", workers}, {"top1", signatures}});
        result["records"].back()["accounting"] = accounting_delta(accounting_before, accounting_after);
    }
    return result;
}
} // namespace

// A strong executable definition catches calls from the statically linked
// ggml backend. Symbol matching avoids changing unrelated Metal/dispatch code.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*function)(void*), void* argument) {
    using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static const auto original = std::bit_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (original == nullptr) return ENOSYS;
    if (!recording.load()) return original(thread, attr, function, argument);
    Dl_info info{};
    const bool cpu_worker = dladdr(std::bit_cast<void*>(function), &info) != 0 && info.dli_sname != nullptr &&
                           std::string_view(info.dli_sname).find("ggml_graph_compute_secondary_thread") != std::string_view::npos;
    if (!cpu_worker) {
        ++stats.other_threads;
        return original(thread, attr, function, argument);
    }
    auto* start = new (std::nothrow) Start{function, argument, static_cast<qos_class_t>(worker_qos.load())};
    if (start == nullptr) return ENOMEM;
    const int rc = original(thread, attr, start_worker, start);
    if (rc != 0) delete start;
    return rc;
}

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: burst_bench MODEL TABLES SPEC OUTPUT");
        const std::vector<std::string> args(argv, argv + argc);
        const auto result = run(args);
        std::ofstream output(args.at(4));
        output << result.dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write result");
        return 0;
    } catch (const std::exception& error) {
        recording = false;
        worker_qos = 0;
        (void)pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
        std::cerr << "burst experiment failed: " << error.what() << '\n';
        return 1;
    }
}

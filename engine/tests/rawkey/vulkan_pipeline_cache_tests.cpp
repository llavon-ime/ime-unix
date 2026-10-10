#include "raw_key_harness.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, std::optional<std::string> value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        RAWKEY_ASSERT(value ? ::setenv(name, value->c_str(), 1) == 0 : ::unsetenv(name) == 0);
    }
    ~ScopedEnvironment() {
        if (previous_) (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
private:
    std::string name_;
    std::optional<std::string> previous_;
};

class CacheTestDirectory {
public:
    CacheTestDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "llavon-vulkan-rawkey-XXXXXX").string();
        const auto* created = ::mkdtemp(pattern.data());
        RAWKEY_ASSERT(created != nullptr);
        path = created;
    }
    ~CacheTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    CacheTestDirectory(const CacheTestDirectory&) = delete;
    CacheTestDirectory& operator=(const CacheTestDirectory&) = delete;
    std::filesystem::path path;
};

std::string predict_and_commit(const std::string& service, const std::string& model) {
    HarnessOptions options;
    options.service_path = service;
    options.model_path = model;
    options.tables_dir = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path().string();
    options.config.smart_english = false;
    options.config.gpu_layers = -2;
    Harness harness(options);
    harness.set_surrounding("今天天氣很好，我們一起", 11, 11);
    harness.type("su3cl3");
    RAWKEY_ASSERT(harness.pump_until([&] {
        const auto* session = harness.session();
        return session && session->prediction.session_open() && session->prediction.next_request_id > 1 &&
               !session->prediction.pending;
    }, std::chrono::seconds(120)));
    const auto preview = harness.preedit();
    RAWKEY_ASSERT(!preview.empty());
    RAWKEY_ASSERT(harness.commits().empty());
    harness.key("Down");
    RAWKEY_ASSERT(harness.has_candidates());
    harness.key("Escape");
    harness.expect_commit(preview);
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.last_commit() == preview; }, std::chrono::seconds(30)));
    RAWKEY_ASSERT(harness.composition_empty());
    return preview;
}

bool has_cache_payload(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.file_size() > 0) return true;
    }
    return false;
}

}  // namespace

// Opt in on a Vulkan-equipped host with the service executable and GGUF model.
// Each harness owns a fresh service process; the second run shares only the
// persistent cache, never a loaded model or in-memory pipeline state.
RAWKEY_SUITE("Vulkan pipeline cache preserves prediction and commits", vulkan_pipeline_cache) {
    const auto* service_setting = std::getenv("LLAVON_IME_TEST_VULKAN_SERVICE");
    if (!service_setting || !*service_setting) {
        std::puts("[skip] Vulkan cache hardware test: set LLAVON_IME_TEST_VULKAN_SERVICE and LLAVON_IME_TEST_VULKAN_MODEL");
        return;
    }
    const auto* model_setting = std::getenv("LLAVON_IME_TEST_VULKAN_MODEL");
    RAWKEY_ASSERT(model_setting && *model_setting);
    const std::string service(service_setting), model(model_setting);
    RAWKEY_ASSERT(std::filesystem::is_regular_file(service));
    RAWKEY_ASSERT(std::filesystem::is_regular_file(model));
    CacheTestDirectory directory;
    ScopedEnvironment gpu("LLAVON_IME_GPU_LAYERS", "auto");
    ScopedEnvironment model_path("LLAVON_IME_MODEL_PATH", model);
    ScopedEnvironment cache_home("XDG_CACHE_HOME", directory.path.string());
    ScopedEnvironment cache_override("LLAVON_IME_VULKAN_PIPELINE_CACHE_DIR", std::nullopt);
#ifdef __APPLE__
    ScopedEnvironment home("HOME", directory.path.string());
    const auto cache = directory.path / "Library/Caches/llavon-ime/vulkan";
#else
    const auto cache = directory.path / "llavon-ime/vulkan";
#endif
    const auto cold = predict_and_commit(service, model);
    RAWKEY_ASSERT(has_cache_payload(cache));
    RAWKEY_ASSERT(predict_and_commit(service, model) == cold);
    RAWKEY_ASSERT(has_cache_payload(cache));

    // A non-directory cache path is a cache miss, not an inference failure.
    const auto blocked = directory.path / "blocked";
    { std::ofstream file(blocked); file << "not a directory"; }
    {
        ScopedEnvironment unavailable("LLAVON_IME_VULKAN_PIPELINE_CACHE_DIR", blocked.string());
        RAWKEY_ASSERT(predict_and_commit(service, model) == cold);
    }
    {
        ScopedEnvironment disabled("LLAVON_IME_VULKAN_PIPELINE_CACHE_DIR", "");
        RAWKEY_ASSERT(predict_and_commit(service, model) == cold);
    }
}

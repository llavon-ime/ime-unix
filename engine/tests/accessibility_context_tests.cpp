#include "test_suites.h"
#include "context/accessibility_context.hpp"
#include "text/utf.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace llavon::ime {
namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[FAIL] %s\n", message);
    return condition;
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* saved = std::getenv(name)) saved_ = std::string(saved);
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (saved_) {
            setenv(name_.c_str(), saved_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

std::unique_ptr<AccessibilityContextProvider> make_provider(size_t max_code_units) {
    return create_accessibility_context_provider(max_code_units);
}

bool test_publish_and_sequence() {
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);
    auto provider = make_provider(64);
    bool ok = check(!provider->latest().has_value(), "no sample before the first publish");
    provider->publish(u"你好", true);
    const auto first = provider->latest();
    ok &= check(first.has_value() && first->usable && first->text == u"你好", "publish exposes the sample");
    ok &= check(first->sequence == 1, "the first sample has sequence 1");
    provider->publish(std::u16string(), false);
    const auto second = provider->latest();
    ok &= check(second.has_value() && !second->usable, "an unusable sample still advances the sequence");
    ok &= check(second->sequence == 2, "every publish advances the sequence");
    ok &= check(provider->sequence() == 2, "sequence() reports the latest sequence");
    return ok;
}

bool test_disabled_start() {
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", "1");
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    auto provider = make_provider(64);
    bool ok = check(provider->availability().availability == AccessibilityAvailability::Disabled,
                    "the disabled source reports Disabled");
    ok &= check(!provider->start(), "start() refuses while accessibility is disabled");
    ok &= check(!provider->running(), "a refused provider is not running");
    return ok;
}

bool test_availability_missing_library() {
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    ScopedEnv library("LLAVON_IME_ATSPI_LIBRARY", "/nonexistent/llavon-ime-libatspi.so.0");
#if defined(__linux__)
    auto provider = make_provider(64);
    bool ok = check(!provider->start(), "start() fails when the library is missing");
    const auto state = provider->availability();
    ok &= check(state.availability != AccessibilityAvailability::Available,
                "a missing library never reports Available");
    if (state.availability == AccessibilityAvailability::Unavailable) {
        ok &= check(state.detail == "libatspi-missing", "the missing library detail is reported");
    }
    return ok;
#else
    // The library override only affects the Linux AT-SPI backend.
    (void)make_provider(64);
    return true;
#endif
}

bool test_missing_library_is_graceful() {
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    ScopedEnv library("LLAVON_IME_ATSPI_LIBRARY", "/nonexistent/llavon-ime-libatspi.so.0");
#if defined(__linux__)
    auto provider = make_provider(64);
    bool ok = check(!provider->start(), "a missing AT-SPI library is not fatal");
    ok &= check(!provider->running(), "a missing library leaves the provider stopped");
    return ok;
#else
    (void)make_provider(64);
    return true;
#endif
}

bool test_file_backed_sample() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-context-sample-test.txt";
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary);
        output << "\xe6\x97\xa9\xe5\xae\x89\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c";
    }
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", path.c_str());
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);

    auto provider = make_provider(64);
    bool ok = check(provider->availability().availability == AccessibilityAvailability::Unsupported,
                    "availability is Unsupported before the source starts");
    ok &= check(provider->start(), "the file-backed source starts headlessly");
    ok &= check(provider->running(), "the file-backed source reports running");
    const auto state = provider->availability();
    ok &= check(state.availability == AccessibilityAvailability::Available && state.detail == "sample-file",
                "the file-backed source reports Available");
    const auto first = provider->latest();
    ok &= check(first.has_value() && first->usable && first->text == u"早安，世界",
                "the file content becomes the sample");
    ok &= check(first->sequence == 1, "the initial file read publishes sequence 1");
    provider->set_active(true);

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "\xe7\xac\xac\xe4\xba\x8c\xe6\xae\xb5\xe6\x96\x87\xe5\xad\x97";
    }
    provider->refresh();
    const auto second = provider->latest();
    ok &= check(second.has_value() && second->usable && second->text == u"第二段文字",
                "refresh re-reads the file");
    ok &= check(second->sequence == 2, "refresh advances the sequence");

    std::filesystem::remove(path);
    provider->refresh();
    const auto missing = provider->latest();
    ok &= check(missing.has_value() && !missing->usable, "a missing file publishes an unusable sample");
    ok &= check(missing->sequence == 3, "the unusable sample still advances the sequence");

    provider->stop();
    ok &= check(!provider->running(), "stop() ends the file-backed source");
    return ok;
}

bool test_legacy_sample_alias() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-legacy-sample-test.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "legacy";
    }
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", path.c_str());
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);

    auto provider = make_provider(64);
    bool ok = check(provider->start(), "the legacy sample env starts the file source");
    const auto sample_value = provider->latest();
    ok &= check(sample_value.has_value() && sample_value->usable && sample_value->text == u"legacy",
                "the legacy sample env is still honored");
    std::filesystem::remove(path);
    return ok;
}

bool test_file_sample_bounded_utf16() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-context-window-test.txt";
    std::string text;
    for (int i = 0; i < 100; ++i) text += "ab";
    text += "\xF0\x9F\x98\x80";  // U+1F600
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", path.c_str());
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);

    auto provider = make_provider(5);
    (void)provider->start();
    const auto latest = provider->latest();
    bool ok = check(latest.has_value() && latest->usable, "a long file sample is usable");
    ok &= check(latest->text.size() <= 5, "the file sample respects the code-unit bound");
    ok &= check(latest->text == u"bab\U0001F600", "the sample keeps the newest complete scalars");
    std::filesystem::remove(path);
    return ok;
}

bool test_active_gating() {
    const auto path = std::filesystem::temp_directory_path() / "llavon-ime-context-active-test.txt";
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "active sample";
    }
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", path.c_str());
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);

    auto provider = make_provider(64);
    bool ok = check(provider->start(), "the provider starts for the active-gating test");
    ok &= check(!provider->active(), "providers start inactive");
    ok &= check(provider->sequence() == 1, "the initial file read publishes once");
    ok &= check(provider->activation_generation() == 0, "providers start at activation generation zero");

    provider->set_active(false);
    ok &= check(provider->sequence() == 1, "deactivating while already inactive publishes nothing");
    provider->set_active(true);
    ok &= check(provider->active(), "set_active(true) marks the provider active");
    ok &= check(provider->activation_generation() == 1, "activation advances the generation");
    ok &= check(provider->sequence() == 1, "activating alone does not publish");
    provider->refresh();
    ok &= check(provider->sequence() == 2, "refresh publishes while active");

    provider->set_active(false);
    const auto inactive = provider->latest();
    ok &= check(inactive.has_value() && !inactive->usable, "deactivating invalidates the sample");
    ok &= check(inactive->sequence == 3, "deactivation advances the sequence");
    ok &= check(provider->activation_generation() == 2, "deactivation advances the generation");
    provider->refresh();
    ok &= check(provider->sequence() == 3, "refresh does nothing while inactive");

    provider->set_active(true);
    provider->refresh();
    const auto resumed = provider->latest();
    ok &= check(resumed.has_value() && resumed->usable && resumed->text == u"active sample",
                "reactivating and refreshing restores a usable sample");
    std::filesystem::remove(path);
    return ok;
}

bool test_concurrent_access() {
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);
    auto provider = make_provider(64);
    std::atomic<bool> stop{false};
    std::thread reader([&provider, &stop]() {
        while (!stop.load()) {
            const auto sample_value = provider->latest();
            if (sample_value.has_value() && sample_value->sequence == 0) {
                std::printf("[FAIL] sequence regression\n");
            }
        }
    });
    for (int i = 0; i < 1000; ++i) provider->publish(u"並行", i % 2 == 0);
    stop.store(true);
    reader.join();
    return check(provider->sequence() == 1000, "concurrent publishes keep a monotonic sequence");
}

bool test_failed_start_is_not_retried() {
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", nullptr);
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    auto provider = make_provider(64);
    bool ok = true;
    if (provider->start()) {
        ok &= check(provider->running(), "a started backend reports running");
    } else {
        // libatspi cannot be initialised twice in one process: a failed
        // backend must stay failed instead of being retried on the next
        // configuration reload.
        ok &= check(!provider->start(), "a failed backend is not retried");
        ok &= check(!provider->running(), "a failed backend stays stopped");
    }
    return ok;
}

bool test_max_code_units_can_be_retuned() {
    ScopedEnv disable("LLAVON_IME_DISABLE_ATSPI", "1");
    ScopedEnv sample("LLAVON_IME_CONTEXT_SAMPLE_FILE", nullptr);
    ScopedEnv legacy("LLAVON_IME_ATSPI_SAMPLE_FILE", nullptr);
    auto provider = make_provider(64);
    bool ok = check(provider->max_code_units() == 64, "the initial sampling bound is reported");
    // The engine retunes the bound on a configuration change instead of
    // rebuilding the backend.
    provider->set_max_code_units(256);
    ok &= check(provider->max_code_units() == 256, "the sampling bound can be updated in place");
    return ok;
}

}  // namespace
}  // namespace llavon::ime

int run_accessibility_context_tests() {
    using namespace llavon::ime;
    bool ok = true;
    ok &= test_publish_and_sequence();
    ok &= test_disabled_start();
    ok &= test_availability_missing_library();
    ok &= test_missing_library_is_graceful();
    ok &= test_failed_start_is_not_retried();
    ok &= test_max_code_units_can_be_retuned();
    ok &= test_file_backed_sample();
    ok &= test_legacy_sample_alias();
    ok &= test_file_sample_bounded_utf16();
    ok &= test_active_gating();
    ok &= test_concurrent_access();
    if (ok) std::printf("accessibility context tests passed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

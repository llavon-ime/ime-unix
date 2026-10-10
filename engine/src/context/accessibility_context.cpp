#include "context/accessibility_context.hpp"

#include "text/utf.hpp"
#include "util/env.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifdef LLAVON_IME_HAVE_ATSPI
#include "atspi/atspi_context_provider.hpp"
#endif

namespace llavon::ime {

AccessibilityContextProvider::AccessibilityContextProvider(size_t max_code_units)
    : max_code_units_(max_code_units) {}

AccessibilityContextProvider::~AccessibilityContextProvider() = default;

void AccessibilityContextProvider::set_active(bool active) {
    if (active_.exchange(active) == active) return;
    activation_generation_.fetch_add(1);
    if (!active) publish(std::u16string(), false);
}

bool AccessibilityContextProvider::active() const noexcept { return active_.load(); }

std::uint64_t AccessibilityContextProvider::activation_generation() const noexcept {
    return activation_generation_.load();
}

void AccessibilityContextProvider::publish(std::u16string text, bool usable) {
    std::lock_guard<std::mutex> lock(mutex_);
    sample_.text = std::move(text);
    sample_.usable = usable;
    sample_.sequence = ++next_sequence_;
    has_sample_ = true;
}

std::optional<AccessibilityContextSample> AccessibilityContextProvider::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_sample_) return std::nullopt;
    return sample_;
}

std::uint64_t AccessibilityContextProvider::sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sample_.sequence;
}

AccessibilityContextState AccessibilityContextProvider::availability() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return availability_;
}

void AccessibilityContextProvider::set_availability(AccessibilityAvailability availability, std::string detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    availability_.availability = availability;
    availability_.detail = std::move(detail);
}

namespace {

// Refuses to run; used when accessibility is explicitly disabled or the
// platform has no supported backend.
class UnavailableContextProvider final : public AccessibilityContextProvider {
public:
    UnavailableContextProvider(size_t max_code_units, AccessibilityAvailability availability, std::string detail)
        : AccessibilityContextProvider(max_code_units) {
        set_availability(availability, std::move(detail));
    }

    bool start() override { return false; }
    void stop() override {}
    bool running() const noexcept override { return false; }
    void refresh() override {}
};

// Reads the sample from a UTF-8 file. This keeps the provider path testable in
// headless environments on every platform.
class FileContextProvider final : public AccessibilityContextProvider {
public:
    FileContextProvider(size_t max_code_units, std::string path)
        : AccessibilityContextProvider(max_code_units), path_(std::move(path)) {}

    bool start() override {
        running_.store(true);
        set_availability(AccessibilityAvailability::Available, "sample-file");
        refresh_file();
        return true;
    }

    void stop() override { running_.store(false); }

    bool running() const noexcept override { return running_.load(); }

    void refresh() override {
        if (!running() || !active()) return;
        refresh_file();
    }

private:
    void refresh_file() {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            publish(std::u16string(), false);
            return;
        }
        const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        try {
            publish(utf8_prefix_tail(raw, raw.size(), max_code_units()), true);
        } catch (const std::exception&) {
            publish(std::u16string(), false);
        }
    }

    std::atomic<bool> running_{false};
    std::string path_;
};

}  // namespace

std::unique_ptr<AccessibilityContextProvider> create_accessibility_context_provider(size_t max_code_units) {
    if (env_with_legacy("LLAVON_IME_DISABLE_ATSPI", "IME_FCITX5_DISABLE_ATSPI") != nullptr) {
        return std::make_unique<UnavailableContextProvider>(max_code_units, AccessibilityAvailability::Disabled,
                                                            "configured");
    }
    if (const char* file = env_with_legacy("LLAVON_IME_CONTEXT_SAMPLE_FILE",
                                           "IME_FCITX5_CONTEXT_SAMPLE_FILE");
        file != nullptr) {
        return std::make_unique<FileContextProvider>(max_code_units, file);
    }
    if (const char* file = env_with_legacy("LLAVON_IME_ATSPI_SAMPLE_FILE",
                                           "IME_FCITX5_ATSPI_SAMPLE_FILE");
        file != nullptr) {
        return std::make_unique<FileContextProvider>(max_code_units, file);
    }
#ifdef LLAVON_IME_HAVE_ATSPI
    return std::make_unique<AtspiContextProvider>(max_code_units);
#else
    return std::make_unique<UnavailableContextProvider>(max_code_units, AccessibilityAvailability::Unsupported,
                                                        "no-backend");
#endif
}

}  // namespace llavon::ime

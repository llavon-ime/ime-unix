#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace llavon::ime {

// The most recent text before the caret observed by a context source.
struct AccessibilityContextSample {
    std::u16string text;
    std::uint64_t sequence = 0;
    bool usable = false;
};

// Whether the platform can currently produce accessibility context samples.
enum class AccessibilityAvailability {
    Unsupported,  // no backend for this platform / build
    Disabled,     // turned off by configuration
    Unavailable,  // backend exists but cannot sample right now
    Available,    // backend can sample the focused widget
};

// Availability plus a stable identifier describing the reason. The engine
// maps the identifier to user-facing text for the read-only config status.
struct AccessibilityContextState {
    AccessibilityAvailability availability = AccessibilityAvailability::Unsupported;
    std::string detail;
};

// Platform-independent state and interface for sampling the text before the
// caret of the focused editable widget, independently of what this IME has
// committed.
//
// The base class owns the sample store and the input-method-active gate so
// every backend shares the same sequence/gating semantics. Backends implement
// only the platform lifecycle: AT-SPI on Linux, or the file-backed source used
// by tests and headless validation. macOS obtains surrounding text directly
// from the InputMethodKit client instead of requesting Accessibility access.
//
// Every publish advances sequence(), even when the sample is not usable, so
// callers can detect that the focused widget changed and must not reuse stale
// text. latest() is lock-protected and cheap: the engine calls it while
// building a prediction request.
class AccessibilityContextProvider {
public:
    explicit AccessibilityContextProvider(size_t max_code_units);
    virtual ~AccessibilityContextProvider();

    AccessibilityContextProvider(const AccessibilityContextProvider&) = delete;
    AccessibilityContextProvider& operator=(const AccessibilityContextProvider&) = delete;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool running() const noexcept = 0;

    // Asks the source to resample the focused widget.
    virtual void refresh() = 0;

    // Collection is gated on the input method being active: while inactive the
    // backend ignores events and the latest sample becomes unusable, so the
    // focused widget's text is not retained behind the user's back.
    void set_active(bool active);
    bool active() const noexcept;
    std::uint64_t activation_generation() const noexcept;

    void publish(std::u16string text, bool usable);

    std::optional<AccessibilityContextSample> latest() const;
    std::uint64_t sequence() const;

    AccessibilityContextState availability() const;

    size_t max_code_units() const noexcept { return max_code_units_; }

    // The engine may tighten the sampling bound without rebuilding the
    // backend: libatspi must never be re-initialised inside one process.
    void set_max_code_units(size_t max_code_units) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_code_units_ = max_code_units;
    }

protected:
    // Backends report their capability here; the engine reads it to render the
    // read-only config status.
    void set_availability(AccessibilityAvailability availability, std::string detail = {});

private:
    size_t max_code_units_;
    mutable std::mutex mutex_;
    AccessibilityContextSample sample_;
    AccessibilityContextState availability_;
    bool has_sample_ = false;
    std::uint64_t next_sequence_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<std::uint64_t> activation_generation_{0};
};

// Creates the best available backend for the current platform. The
// LLAVON_IME_DISABLE_ATSPI and LLAVON_IME_CONTEXT_SAMPLE_FILE environment
// variables override the platform backend for opting out and for headless
// tests (LLAVON_IME_ATSPI_SAMPLE_FILE is kept as a legacy alias).
// LLAVON_IME_ATSPI_LIBRARY points the AT-SPI backend at a different libatspi
// name, which the test suite uses to verify the missing-library fallback.
std::unique_ptr<AccessibilityContextProvider> create_accessibility_context_provider(size_t max_code_units);

}  // namespace llavon::ime

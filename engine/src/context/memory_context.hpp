#pragma once

#include "context/accessibility_context.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace llavon::ime {

// Callbacks the memory probe needs from the engine. All of them run on the
// engine's main thread.
struct MemoryProbeCallbacks {
    // Composition currently shown at the caret (UTF-8, empty = none). The
    // client may hold it in memory. Natural changes to the composition serve
    // as the probe signal; no extra character is inserted into the preedit.
    std::function<std::string()> preedit;
    std::function<std::vector<int>()> processes;
    std::function<bool()> sensitive;
};

// Last-resort context source for applications that expose neither client
// surrounding text nor AT-SPI: while a composition is on screen the probe
// searches the focused client's processes with llavon-ime-memscan for the
// composition and checks that the same location follows subsequent natural
// composition changes before publishing context. Nothing is added to preedit.
// It is gated by the `memory_context` setting.
class MemoryContextProvider final : public AccessibilityContextProvider {
public:
    MemoryContextProvider(size_t max_code_units, MemoryProbeCallbacks callbacks,
                          std::filesystem::path helper_path);
    ~MemoryContextProvider() override;

    MemoryContextProvider(const MemoryContextProvider&) = delete;
    MemoryContextProvider& operator=(const MemoryContextProvider&) = delete;

    bool start() override;
    void stop() override;
    bool running() const noexcept override;

    // Main thread. Schedules a scan for each changed composition, except while
    // inactive, sensitive, backed off, or without a composition.
    void refresh() override;

    // A focus change or a forwarded edit invalidates the previously verified
    // document location; the next composition must establish a new one.
    void invalidate();

    // Number of completed helper runs (diagnostics and tests).
    std::size_t probe_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// True when this build has a probe backend for the current platform.
bool memory_context_supported();

}  // namespace llavon::ime

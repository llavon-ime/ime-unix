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
    // Rendered composition preceding the last segment used as the anchor.
    // This text is still in the IME preedit, even when a client draws it in
    // the same buffer as the document before the anchor.
    std::function<std::string()> preedit_prefix;
    // Text the engine committed recently (UTF-8, empty = none). Clients that
    // do not draw the composition into their document (Konsole, VTE) still
    // write the committed text there, so it locates the caret for them.
    std::function<std::string()> commit_history;
    std::function<std::vector<int>()> processes;
    // Optional OS focus association. Only boosts an address that was also
    // found among the regular scanned process list.
    std::function<int()> focused_process;
    // Focused client name, used to try the client's own screen layout first.
    std::function<std::string()> program;
    std::function<bool()> sensitive;
    // Runs on the probe worker thread when a location has just been
    // confirmed. The engine re-requests the prediction for the composition on
    // screen so the fresh context is actually used; a request already in
    // flight is repeated with it once its response arrives.
    std::function<void()> published;
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
    // document location; the next composition must establish a new one. When
    // the caret only moved forward because of a commit (context_continues),
    // retain the previous document tail to compare against the next sample.
    void invalidate(bool context_continues = false);

    // Main thread. Resets the client's soft-dirty baseline before any
    // composition exists, so the first probe of a focused client is a
    // changed-pages scan instead of a full one.
    void prime();

    // Number of completed helper runs (diagnostics and tests).
    std::size_t probe_count() const noexcept;

    // True while a location is being verified or already confirmed, so a
    // rendered-only composition may still be tracked instead of skipped.
    bool tracking() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// True when this build has a probe backend for the current platform.
bool memory_context_supported();

}  // namespace llavon::ime

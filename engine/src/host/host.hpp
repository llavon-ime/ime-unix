#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace llavon::ime {

// Opaque handle for one host input context. The engine keeps one InputSession
// per attached context; the host assigns and owns the lifetime.
using ContextId = std::uint64_t;

// The client's text around the caret, as reported by the host. Offsets are in
// UTF-16 code units; `cursor` and `anchor` are clamped by the engine.
struct HostContext {
    bool valid = false;
    std::u16string text;
    std::size_t cursor = 0;
    std::size_t anchor = 0;
};

// The frontend integration: one implementation per host (the fcitx5 adapter,
// the macOS InputMethodKit controller) plus test fakes.
class Host {
public:
    virtual ~Host() = default;

    // Marshals `body` onto the host's main thread. May be called from any
    // thread. `body` must tolerate its context having been detached.
    virtual void post(std::function<void()> body) = 0;

    // Commits text to the application. Main thread only.
    virtual void commit(ContextId context, std::u16string_view text) = 0;

    // Asks the host to refresh the UI for the context. Main thread only; the
    // host reads the new state through Engine::render_state().
    virtual void update_ui(ContextId context) = 0;

    // Current surrounding text for the context, if the client exposes any.
    // Main thread only.
    virtual HostContext surrounding_text(ContextId context) = 0;

    // Optional experimental Linux source. Implementations must bind results
    // to a focus generation and publish only after insertion AND removal have
    // been acknowledged. Existing/native hosts retain their default behavior.
    virtual void request_memory_context(ContextId) {}
    virtual void invalidate_memory_context(ContextId) {}
    virtual HostContext memory_context(ContextId) { return {}; }

    // Whether the context is a password or otherwise sensitive field where the
    // prediction context must not be read or sent. Main thread only.
    virtual bool is_sensitive(ContextId context) = 0;
};

}  // namespace llavon::ime

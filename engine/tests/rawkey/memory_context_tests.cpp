#include "raw_key_harness.hpp"
#include "context/marker_transaction.hpp"

#ifdef __linux__
#include "memory/focus_probe.hpp"
#include <atomic>
#include <thread>
#endif

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

RAWKEY_SUITE("memory marker transaction", memory_marker_transaction) {
    MarkerTransaction state;
    const HostContext baseline{true, u"前文😀後文", 4, 4};
    const std::u16string marker(128, u'\u2063');
    const auto ticket = state.focus(1);
    auto selection = baseline;
    selection.anchor = 0;
    RAWKEY_ASSERT(!state.begin(ticket, selection, marker));
    auto split_surrogate = baseline;
    split_surrogate.cursor = split_surrogate.anchor = 3;
    RAWKEY_ASSERT(!state.begin(ticket, split_surrogate, marker));
    RAWKEY_ASSERT(state.begin(ticket, baseline, marker));
    RAWKEY_ASSERT(state.observe(ticket, baseline) == MarkerTransaction::Action::none);
    RAWKEY_ASSERT(!state.sample(1).valid);
    const HostContext inserted{true, state.marked_text(), 132, 132};
    RAWKEY_ASSERT(state.observe(ticket, inserted) == MarkerTransaction::Action::delete_marker);
    RAWKEY_ASSERT(!state.finish(ticket, u"前文😀"));
    RAWKEY_ASSERT(state.observe(ticket, inserted) == MarkerTransaction::Action::none);
    RAWKEY_ASSERT(state.observe(ticket, baseline) == MarkerTransaction::Action::scan);
    RAWKEY_ASSERT(state.finish(ticket, u"前文😀"));
    RAWKEY_ASSERT(state.sample(1).text == u"前文😀");
    RAWKEY_ASSERT(!state.sample(2).valid);
    const auto next = state.focus(1);
    RAWKEY_ASSERT(next != ticket);
    RAWKEY_ASSERT(!state.finish(ticket, u"前文😀"));
    RAWKEY_ASSERT(!state.sample(1).valid);
    RAWKEY_ASSERT(state.begin(next, baseline, marker));
    auto unexpected = inserted;
    unexpected.cursor--;
    RAWKEY_ASSERT(state.observe(next, unexpected) == MarkerTransaction::Action::none);
    RAWKEY_ASSERT(state.phase() == MarkerTransaction::Phase::failed);

    // The optional source does not affect the default raw-key path.
    Harness harness;
    harness.set_configs({{"SmartEnglish", "False"}});
    harness.activate();
    harness.type("su3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.expect_commit("你");
}

#ifdef __linux__
namespace {
struct Fixture {
    Harness harness;
    HostContext baseline{true, u"前文😀後文", 4, 4};
    std::u16string marker;
    std::string status;
    std::atomic<int> scans = 0;
    bool eligible = true;
    bool readable = true;
    unsigned deletes = 0;
    int requests = 0;
    std::unique_ptr<memory::FocusProbe> probe;

    Fixture() {
        harness.set_configs({{"SmartEnglish", "False"}});
        harness.host().set_surrounding(baseline);
        memory::FocusProbe::Hooks hooks;
        hooks.snapshot = [this](ContextId id) { return harness.host().surrounding_text(id); };
        hooks.eligible = [this](ContextId) { return eligible && !harness.host().is_sensitive(1); };
        hooks.insert = [this](ContextId, std::u16string_view text) { marker = text; };
        hooks.remove = [this](ContextId, unsigned count) { RAWKEY_ASSERT(count == 128); ++deletes; };
        hooks.post = [this](std::function<void()> body) { harness.host().post(std::move(body)); };
        hooks.status = [this](std::string value) { status = std::move(value); };
        hooks.discover = [this](std::string_view, std::stop_token) {
            return readable ? memory::Discovery{{{123, 456}}, "prepared"} : memory::Discovery{{}, "permission-denied"};
        };
        hooks.capture = [this](const auto&, std::u16string_view text, std::size_t cursor,
                               std::u16string_view nonce, std::stop_token) {
            ++scans;
            if (nonce.size() != 128) return memory::Capture{{}, "invalid-marker", 0};
            return memory::Capture{std::u16string(text.substr(0, cursor)), "ready", 4096};
        };
        probe = std::make_unique<memory::FocusProbe>(std::move(hooks));
        harness.host().on_memory_request = [this](ContextId id) { ++requests; probe->focus(id, "test-client"); };
        harness.host().on_memory_invalidate = [this](ContextId id) { probe->invalidate(id); };
        harness.host().on_memory_sample = [this](ContextId id) { return probe->sample(id); };
    }
    ~Fixture() {
        harness.host().on_memory_request = {};
        harness.host().on_memory_invalidate = {};
        harness.host().on_memory_sample = {};
    }
    void echo(HostContext snapshot) {
        harness.host().set_surrounding(snapshot);
        probe->observe(1, snapshot);
    }
    void start() {
        harness.activate();
        RAWKEY_ASSERT(marker.empty()); // Cached surrounding is insufficient.
        echo(baseline);
        RAWKEY_ASSERT(harness.pump_until([this] { return !marker.empty(); }));
        RAWKEY_ASSERT(deletes == 0 && scans == 0);
    }
    void inserted() {
        auto text = baseline.text;
        text.insert(baseline.cursor, marker);
        echo({true, text, baseline.cursor + marker.size(), baseline.cursor + marker.size()});
        RAWKEY_ASSERT(deletes == 1 && scans == 0);
    }
    void ready() {
        start();
        inserted();
        echo(baseline);
        RAWKEY_ASSERT(harness.pump_until([this] { return status == "ready"; }));
    }
};
}

RAWKEY_SUITE("memory focus verified echo and raw keys", memory_focus_verified) {
    Fixture f;
    f.start();
    bool replayed = false;
    RAWKEY_ASSERT(f.probe->defer_key(1, [&](bool restored) {
        RAWKEY_ASSERT(restored);
        replayed = true;
        f.harness.type("su3");
    }));
    RAWKEY_ASSERT(f.harness.composition_empty());
    RAWKEY_ASSERT(f.harness.commits().empty());
    f.inserted();
    RAWKEY_ASSERT(!replayed);
    f.echo(f.baseline);
    RAWKEY_ASSERT(replayed);
    RAWKEY_ASSERT(f.harness.preedit() == "你");
    RAWKEY_ASSERT(f.harness.pump_until([&] { return f.status == "ready"; }));
    RAWKEY_ASSERT(f.probe->sample(1).text == u"前文😀");
    f.harness.type("cl3");
    RAWKEY_ASSERT(f.harness.session()->context_text == u"前文😀");
    f.harness.expect_commit("你好");
    RAWKEY_ASSERT(!f.probe->sample(1).valid);
    RAWKEY_ASSERT(f.harness.commits().size() == 1); // No marker through engine commit/training.
}

RAWKEY_SUITE("memory focus unsupported clients and selection", memory_focus_skips) {
    {
        Fixture f;
        f.readable = false;
        f.harness.activate();
        f.echo(f.baseline);
        RAWKEY_ASSERT(f.harness.pump_until([&] { return f.status == "permission-denied"; }));
        RAWKEY_ASSERT(f.marker.empty() && f.deletes == 0 && f.scans == 0);
        f.harness.type("su3");
        RAWKEY_ASSERT(f.harness.preedit() == "你");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.harness.activate();
        auto selection = f.baseline;
        selection.anchor = 0;
        f.echo(selection);
        RAWKEY_ASSERT(f.status == "unsupported-selection-or-text" && f.marker.empty());
        f.harness.type("su3");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.harness.host().set_sensitive(true);
        f.harness.activate();
        RAWKEY_ASSERT(f.requests == 0 && f.marker.empty());
        f.harness.type("su3");
        RAWKEY_ASSERT(f.harness.session()->context_text.empty());
        f.harness.expect_commit("你");
    }
}

RAWKEY_SUITE("memory focus stale replies and recovery", memory_focus_cancel) {
    {
        Fixture f;
        f.start();
        f.inserted();
        f.echo(f.baseline);
        // Cancel before dispatching any scan completion.
        f.harness.focus_out();
        f.harness.activate();
        f.harness.drain();
        RAWKEY_ASSERT(!f.probe->sample(1).valid);
        f.harness.type("su3");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.start();
        bool forwarded = false;
        RAWKEY_ASSERT(f.probe->defer_key(1, [&](bool restored) { RAWKEY_ASSERT(!restored); forwarded = true; }));
        f.eligible = false;
        f.probe->tick();
        RAWKEY_ASSERT(forwarded && !f.probe->sample(1).valid);
        RAWKEY_ASSERT(f.status.find("cleanup-unconfirmed") != std::string::npos);
        f.eligible = true;
        f.harness.activate();
        RAWKEY_ASSERT(f.status == "client-disabled-after-unconfirmed-cleanup");
        f.harness.type("su3");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.ready();
        RAWKEY_ASSERT(!f.harness.key_accepted("Left"));
        RAWKEY_ASSERT(!f.probe->sample(1).valid);
        f.harness.type("su3");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.ready();
        f.harness.host().set_surrounding({true, u"另一欄位", 4, 4});
        RAWKEY_ASSERT(!f.probe->sample(1).valid);
        f.harness.type("su3");
        RAWKEY_ASSERT(f.harness.session()->context_text == u"另一欄位");
        f.harness.expect_commit("你");
    }
    {
        Fixture f;
        f.start();
        bool forwarded = false;
        RAWKEY_ASSERT(f.probe->defer_key(1, [&](bool restored) { RAWKEY_ASSERT(!restored); forwarded = true; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        f.probe->tick();
        RAWKEY_ASSERT(forwarded && f.status == "timeout:cleanup-unconfirmed");
        RAWKEY_ASSERT(f.deletes == 0 && f.scans == 0);
        f.harness.type("su3");
        f.harness.expect_commit("你");
    }
}
#endif

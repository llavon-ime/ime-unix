#include "raw_key_harness.hpp"
#include "scripted_service.hpp"

#include <atomic>

using namespace llavon::ime;
using namespace llavon::ime::rawkey;

namespace {
ServiceTransportOptions transport_for(const test::ScriptedService& server) {
    ServiceTransportOptions options;
    options.socket_path = server.path();
    options.auto_start = false;
    options.request_timeout = std::chrono::milliseconds(150);
    options.cold_prediction_timeout = std::chrono::milliseconds(150);
    return options;
}
}

RAWKEY_SUITE("prediction timeout releases submit and reconnects", timeout_submit) {
    std::atomic<int> requests{0};
    test::ScriptedService server([&](int fd, int index) {
        test::ScriptedService::handshake(fd);
        if (index == 0) {
            (void)test::ScriptedService::receive(fd);
            protocol::SessionId id{}; id[0] = 1;
            test::ScriptedService::send(fd, protocol::OpenSessionResponse{id, {}});
        }
        const auto request = std::get<protocol::PredictRequest>(test::ScriptedService::receive(fd));
        ++requests;
        if (index == 0) test::ScriptedService::await_disconnect(fd);
        else test::ScriptedService::send(fd, protocol::Prediction{
            request.session_id, request.request_id, request.buffer_revision, {{U'擬'}}});
    });
    HarnessOptions options;
    options.config.smart_english = false;
    options.on_training_commit = [](const InputEffect::CommitSample&, std::u16string_view) {};
    options.socket_path = server.path().string();
    Harness harness(options);
    harness.engine().set_transport_options(transport_for(server));
    harness.type("su3");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.session()->prediction.inflight_request_id.has_value(); }));
    RAWKEY_ASSERT(!harness.preedit().empty());
    harness.key("Return");
    RAWKEY_ASSERT(harness.commits().empty());
    RAWKEY_ASSERT(harness.pump_until([&] { return !harness.commits().empty(); }));
    RAWKEY_ASSERT(harness.last_commit() == "你");
    RAWKEY_ASSERT(requests.load() == 1);
    RAWKEY_ASSERT(harness.composition_empty());
    harness.type("su3");
    RAWKEY_ASSERT(harness.pump_until([&] { return !harness.session()->prediction.pending; }));
    RAWKEY_ASSERT(harness.preedit() == "擬");
    harness.expect_commit("擬");
    RAWKEY_ASSERT(harness.commits().size() == 2);
    RAWKEY_ASSERT(server.ok());
}

RAWKEY_SUITE("restart preserves composition and cancels old submit", restart_composition) {
    Harness harness;
    harness.type("su3");
    harness.settle_prediction();
    const auto before = harness.preedit();
    harness.session()->deferred_commit = parse_key("Return");
    harness.engine().restart_prediction_service();
    harness.drain();
    RAWKEY_ASSERT(harness.preedit() == before);
    RAWKEY_ASSERT(harness.commits().empty());
    RAWKEY_ASSERT(!harness.session()->deferred_commit);
    RAWKEY_ASSERT(!harness.session()->prediction.session_open());
    harness.expect_commit("你");
}

RAWKEY_SUITE("focus loss during stalled prediction does not submit twice", timeout_focus) {
    test::ScriptedService server([](int fd, int) {
        test::ScriptedService::handshake(fd);
        (void)test::ScriptedService::receive(fd);
        protocol::SessionId id{}; id[0] = 1;
        test::ScriptedService::send(fd, protocol::OpenSessionResponse{id, {}});
        (void)test::ScriptedService::receive(fd);
        test::ScriptedService::await_disconnect(fd);
    });
    HarnessOptions options; options.config.smart_english = false; options.socket_path = server.path().string();
    options.on_training_commit = [](const InputEffect::CommitSample&, std::u16string_view) {};
    Harness harness(options);
    harness.engine().set_transport_options(transport_for(server));
    harness.type("su3");
    RAWKEY_ASSERT(harness.pump_until([&] { return harness.session()->prediction.inflight_request_id.has_value(); }));
    harness.key("Return");
    harness.focus_out();
    RAWKEY_ASSERT(harness.last_commit() == "你");
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(harness.pump_until([&] { return !harness.session()->prediction.pending; }));
    harness.drain();
    RAWKEY_ASSERT(harness.commits().size() == 1);
}

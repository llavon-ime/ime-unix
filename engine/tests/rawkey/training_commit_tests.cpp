#include "raw_key_harness.hpp"
#include "ipc/unix_socket.hpp"
#include "protocol/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace llavon::ime::rawkey;

RAWKEY_SUITE("training commit samples", training_commit_samples) {
    struct Observed {
        llavon::ime::InputEffect::CommitSample sample;
        std::u16string context;
    };
    std::vector<Observed> samples;
    HarnessOptions options;
    options.on_training_commit = [&](const auto& sample, std::u16string_view context) {
        samples.push_back({sample, std::u16string(context)});
    };
    Harness harness(options);
    harness.set_surrounding("早安", 2, 2);

    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(samples.size() == 1);
    RAWKEY_ASSERT(samples[0].sample.answer == u"你");
    RAWKEY_ASSERT(samples[0].sample.entries.size() == 1);
    RAWKEY_ASSERT(samples[0].sample.entries[0].reading == u"ㄋㄧˇ");
    RAWKEY_ASSERT(samples[0].sample.entries[0].character == U'你');
    RAWKEY_ASSERT(!samples[0].sample.entries[0].manually_selected);
    RAWKEY_ASSERT(samples[0].context == u"早安");

    harness.type("su3");
    harness.key("space");
    harness.key("2");
    if (samples.size() == 1) harness.key("Return");
    RAWKEY_ASSERT(samples.size() == 2);
    RAWKEY_ASSERT(samples[1].sample.entries[0].manually_selected);

    harness.host().set_sensitive(true);
    harness.type("su3");
    harness.expect_commit("你");
    RAWKEY_ASSERT(samples.size() == 2);

    harness.host().set_sensitive(false);
    harness.type("abc");
    harness.key("Return");
    RAWKEY_ASSERT(samples.size() == 2);
}

RAWKEY_SUITE("training commit transport", training_commit_transport) {
    using namespace llavon::ime;
    const auto socket = std::filesystem::temp_directory_path() /
                        ("llavon-ime-rawkey-commit-" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket);
    UnixSocketServer server;
    server.bind_listen(socket);
    std::atomic<bool> received{false};
    std::atomic<bool> valid{true};
    std::thread worker([&] {
        try {
            const auto connection = server.accept_one();
            protocol::SessionId session{}; session[0] = 1;
            protocol::ServiceEpoch epoch{}; epoch[0] = 2;
            for (;;) {
                auto header = connection.recv_exact(4);
                std::uint32_t length = 0;
                std::memcpy(&length, header.data(), 4);
                auto payload = connection.recv_exact(length);
                header.insert(header.end(), payload.begin(), payload.end());
                const auto message = protocol::decode(header);
                if (std::holds_alternative<protocol::StatusRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}));
                } else if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::OpenSessionResponse{session, epoch}));
                } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Prediction{
                        session, request->request_id, request->buffer_revision, {{U'你'}}}));
                } else if (const auto* request = std::get_if<protocol::RecordCommitRequest>(&message)) {
                    valid = request->context == u"早安" && request->answer == u"你" &&
                            request->entries.size() == 1 && request->entries[0].reading == u"ㄋㄧˇ";
                    connection.send_all(protocol::encode(protocol::RecordCommitResponse{request->event_id, true}));
                    received = true;
                    break;
                } else if (const auto* request = std::get_if<protocol::CloseSessionRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::CloseSessionResponse{request->session_id, true}));
                    break;
                } else { valid = false; break; }
            }
        } catch (...) { valid = false; }
    });
    bool completed = false;
    {
        HarnessOptions options;
        options.socket_path = socket.string();
        Harness harness(options);
        harness.set_surrounding("早安", 2, 2);
        harness.type("su3");
        harness.expect_commit("你");
        completed = harness.pump_until([&] { return received.load(); });
        harness.detach();
    }
    if (!received.load()) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket);
        } catch (...) {}
    }
    worker.join();
    std::filesystem::remove(socket);
    RAWKEY_ASSERT(completed && valid.load());
}

RAWKEY_SUITE("training commit correction", training_commit_correction) {
    std::size_t commits = 0;
    std::vector<llavon::ime::protocol::SessionId> discards;
    HarnessOptions options;
    options.on_training_commit = [&](const auto&, std::u16string_view) { ++commits; };
    options.on_training_discard = [&](const auto& id) { discards.push_back(id); };
    Harness harness(options);
    harness.set_surrounding("早安", 2, 2);

    // A Backspace that immediately follows the commit withdraws it.
    harness.type("su3");
    harness.expect_commit("你");
    harness.set_surrounding("早安你", 3, 3);
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 1);
    RAWKEY_ASSERT(discards.size() == 1);
    RAWKEY_ASSERT(std::any_of(discards[0].begin(), discards[0].end(), [](auto byte) { return byte != 0; }));

    // An edit outside the IME can change the client's text before Backspace.
    // In that case this is no longer a correction of the recorded commit.
    harness.set_surrounding("早安", 2, 2);
    harness.type("su3");
    harness.expect_commit("你");
    harness.set_surrounding("早安他", 3, 3);
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 2);
    RAWKEY_ASSERT(discards.size() == 1);

    // No client text and no fresh accessibility sample cannot establish that
    // Backspace is correcting this commit, so leave its training row intact.
    harness.set_surrounding("早安", 2, 2);
    harness.type("su3");
    harness.expect_commit("你");
    harness.set_surrounding("", 0, 0);
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 3);
    RAWKEY_ASSERT(discards.size() == 1);

    // Any other key means the user moved on, so a later Backspace only edits
    // the composition and never withdraws the stored sample.
    harness.set_surrounding("早安", 2, 2);
    harness.type("su3");
    harness.expect_commit("你");
    harness.key("Left");
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 4);
    RAWKEY_ASSERT(discards.size() == 1);

    // A Backspace inside a composition belongs to the composition.
    harness.type("su");
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 4);
    RAWKEY_ASSERT(discards.size() == 1);
    harness.key("Escape");
}

RAWKEY_SUITE("training mixed commit samples", training_mixed_commit_samples) {
    struct Observed {
        llavon::ime::InputEffect::CommitSample sample;
        std::u16string context;
    };
    std::vector<Observed> samples;
    HarnessOptions options;
    options.on_training_commit = [&](const auto& sample, std::u16string_view context) {
        samples.push_back({sample, std::u16string(context)});
    };
    Harness harness(options);
    harness.set_surrounding("早安", 2, 2);
    harness.set_config("SmartEnglish", "True");

    // Chinese composed, then an English word typed as literals: the samples
    // keep the literal characters as context without a reading, exactly like
    // the Windows manager.
    harness.type("su3");
    harness.type("hello");
    harness.expect_direct_commit("你hello ", Key(" "));
    std::fprintf(stderr, "[debug] mixed samples=%zu\n", samples.size());
    RAWKEY_ASSERT(samples.size() == 1);
    RAWKEY_ASSERT(samples[0].sample.answer == u"你hello ");
    RAWKEY_ASSERT(samples[0].sample.entries.size() == 7);
    RAWKEY_ASSERT(samples[0].sample.entries[0].reading == u"ㄋㄧˇ");
    RAWKEY_ASSERT(samples[0].sample.entries[0].character == U'你');
    RAWKEY_ASSERT(!samples[0].sample.entries[0].literal);
    RAWKEY_ASSERT(samples[0].sample.entries[1].literal);
    RAWKEY_ASSERT(samples[0].sample.entries[1].character == U'h');
    RAWKEY_ASSERT(samples[0].sample.entries[1].reading.empty());
    RAWKEY_ASSERT(samples[0].sample.entries[6].literal);
    RAWKEY_ASSERT(samples[0].sample.entries[6].character == U' ');

    // A commit without a single composed position is not training data.
    harness.type("hello");
    harness.expect_direct_commit("hello ", Key(" "));
    RAWKEY_ASSERT(samples.size() == 1);

    // Literal punctuation settled into the composition keeps the earlier
    // readings and stays a context-only position.
    harness.type("su3");
    harness.key(Key("Control+,"));
    RAWKEY_ASSERT(harness.preedit() == "你，");
    harness.key("Return");
    RAWKEY_ASSERT(samples.size() == 2);
    RAWKEY_ASSERT(samples[1].sample.answer == u"你，");
    RAWKEY_ASSERT(samples[1].sample.entries.size() == 2);
    RAWKEY_ASSERT(!samples[1].sample.entries[0].literal);
    RAWKEY_ASSERT(samples[1].sample.entries[1].literal);
    RAWKEY_ASSERT(samples[1].sample.entries[1].character == U'，');
}

RAWKEY_SUITE("training commit correction window", training_commit_correction_window) {
    std::size_t commits = 0, discards = 0;
    HarnessOptions options;
    options.commit_correction_window = std::chrono::milliseconds(1000);
    options.on_training_commit = [&](const auto&, std::u16string_view) { ++commits; };
    options.on_training_discard = [&](const auto&) { ++discards; };
    Harness harness(options);
    harness.set_surrounding("早安", 2, 2);

    // Once the window elapsed the Backspace belongs to the host application
    // and leaves the sample alone.
    harness.type("su3");
    harness.expect_commit("你");
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    harness.set_surrounding("早安你", 3, 3);
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 1);
    RAWKEY_ASSERT(discards == 0);

    // Inside the window the immediate Backspace still withdraws the sample.
    harness.set_surrounding("早安", 2, 2);
    harness.type("su3");
    harness.expect_commit("你");
    harness.set_surrounding("早安你", 3, 3);
    harness.key("BackSpace");
    RAWKEY_ASSERT(commits == 2);
    RAWKEY_ASSERT(discards == 1);
}

RAWKEY_SUITE("training commit discard transport", training_commit_discard_transport) {
    using namespace llavon::ime;
    const auto socket = std::filesystem::temp_directory_path() /
                        ("llavon-ime-rawkey-discard-" + std::to_string(::getpid()) + ".sock");
    std::filesystem::remove(socket);
    UnixSocketServer server;
    server.bind_listen(socket);
    std::atomic<bool> discarded{false};
    std::atomic<bool> matched{true};
    protocol::SessionId committed_id{};
    std::thread worker([&] {
        try {
            const auto connection = server.accept_one();
            protocol::SessionId session{}; session[0] = 1;
            protocol::ServiceEpoch epoch{}; epoch[0] = 2;
            for (;;) {
                auto header = connection.recv_exact(4);
                std::uint32_t length = 0;
                std::memcpy(&length, header.data(), 4);
                auto payload = connection.recv_exact(length);
                header.insert(header.end(), payload.begin(), payload.end());
                const auto message = protocol::decode(header);
                if (std::holds_alternative<protocol::StatusRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::StatusResponse{epoch, false, false, 0, 8, std::nullopt}));
                } else if (std::holds_alternative<protocol::OpenSessionRequest>(message)) {
                    connection.send_all(protocol::encode(protocol::OpenSessionResponse{session, epoch}));
                } else if (const auto* request = std::get_if<protocol::PredictRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::Prediction{
                        session, request->request_id, request->buffer_revision, {{U'你'}}}));
                } else if (const auto* request = std::get_if<protocol::RecordCommitRequest>(&message)) {
                    committed_id = request->event_id;
                    connection.send_all(protocol::encode(protocol::RecordCommitResponse{request->event_id, true}));
                } else if (const auto* request = std::get_if<protocol::DiscardCommitRequest>(&message)) {
                    matched = request->event_id == committed_id;
                    connection.send_all(protocol::encode(protocol::DiscardCommitResponse{request->event_id, true}));
                    discarded = true;
                    break;
                } else if (const auto* request = std::get_if<protocol::CloseSessionRequest>(&message)) {
                    connection.send_all(protocol::encode(protocol::CloseSessionResponse{request->session_id, true}));
                    break;
                } else { matched = false; break; }
            }
        } catch (...) { matched = false; }
    });
    bool completed = false;
    {
        HarnessOptions options;
        options.socket_path = socket.string();
        Harness harness(options);
        harness.set_surrounding("早安", 2, 2);
        harness.type("su3");
        harness.expect_commit("你");
        harness.set_surrounding("早安你", 3, 3);
        harness.key("BackSpace");
        completed = harness.pump_until([&] { return discarded.load(); });
        harness.detach();
    }
    if (!discarded.load()) {
        try {
            UnixSocketClient client;
            (void)client.connect(socket);
        } catch (...) {}
    }
    worker.join();
    std::filesystem::remove(socket);
    RAWKEY_ASSERT(completed && matched.load());
}

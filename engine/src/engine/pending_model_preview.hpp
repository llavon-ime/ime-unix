#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "config/config.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "input/input_session.hpp"

namespace llavon::ime {

// A single displayed mixed-input path (or one explicit boundary candidate),
// predicted asynchronously through the
// existing service. Engine owns one coordinator per input context. All state
// transitions run on the host thread; transport callbacks only post work.
class PendingModelPreview {
public:
    struct Client {
        std::function<InputSession*()> session;
        std::function<const Config&()> config;
        std::function<std::u16string()> context;
        std::function<bool()> sensitive;
        std::function<void(std::function<void()>)> post;
        std::function<void()> redraw;
        std::function<void()> settled;
    };

    PendingModelPreview(Client client, const FallbackEngine& fallback,
                        ServiceTransport& transport, const std::filesystem::path& token_table);
    ~PendingModelPreview();
    PendingModelPreview(const PendingModelPreview&) = delete;
    PendingModelPreview& operator=(const PendingModelPreview&) = delete;

    void on_input();
    bool idle() const { return !active_ && !wanted_; }
    std::uint64_t requests() const { return next_request_; }

private:
    struct Snapshot {
        MixedPath path;
        std::u16string raw;
        std::u16string context;
        std::u16string query;
        std::uint64_t raw_revision = 0;
        std::size_t buffer_revision = 0;
        std::size_t caret = 0;
        std::size_t preview = 0;
        int context_length = 0;
        BopomofoKeyboardLayout layout = BopomofoKeyboardLayout::Standard;
        bool sensitive = false;
        bool candidate_only = false;
    };
    struct Job {
        Snapshot snapshot;
        std::size_t segment = 0;
        std::u16string prefix;
        std::vector<std::size_t> run;
        std::uint64_t awaiting_id = 0;
        bool reopened = false;
    };
    struct Lease {
        std::mutex mutex;
        protocol::SessionId id{};
        bool abandoned = false;
    };

    bool supported(std::u16string_view reading) const;
    std::optional<Snapshot> capture() const;
    bool fresh(const Snapshot& snapshot) const;
    static bool compatible(const Snapshot& a, const Snapshot& b);
    static std::u16string text(const MixedSegment& segment);
    static void render(MixedPath& path);
    void start_latest();
    void open(const std::shared_ptr<Job>& job);
    void advance(const std::shared_ptr<Job>& job);
    void reply(const std::shared_ptr<Job>& job, protocol::Message response);
    void finish(const std::shared_ptr<Job>& job, bool apply, bool failed = false);
    void apply_candidate(const Snapshot& snapshot);

    Client client_;
    const FallbackEngine& fallback_;
    ServiceTransport& transport_;
    std::optional<std::unordered_set<std::u16string>> readings_;
    protocol::SessionId session_id_{};
    std::uint64_t next_request_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<Lease> lease_ = std::make_shared<Lease>();
    std::shared_ptr<Job> active_;
    std::optional<Snapshot> wanted_;
    // Keep explicit candidate work out of the automatic preview's cache.
    std::array<std::optional<Snapshot>, 2> cache_;
    std::array<std::optional<Snapshot>, 2> completed_;
};

}  // namespace llavon::ime

#pragma once

// Experimental adapter for the raw-key harness. It updates the actual pending
// MixedPath (including character candidates used by Enter), without changing
// raw keys or production Engine policy. No expected fixture text is consulted.
#include "raw_key_harness.hpp"
#include "engine/fallback_engine.hpp"
#include "engine/service_transport.hpp"
#include "text/utf.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace llavon::ime::rawkey {

class LiveMixedModelPreview {
public:
    struct Stats {
        std::size_t requests = 0;
        std::size_t applied = 0;
        std::size_t stale = 0;
        std::size_t failures = 0;
        std::size_t coalesced = 0;
        std::size_t cache_hits = 0;
        std::size_t early_best_applied = 0;
        std::size_t unsupported_readings = 0;
        std::vector<long long> job_latency_us;
        std::vector<long long> best_latency_us;
        std::vector<std::string> error_examples;
    };

    LiveMixedModelPreview(Harness& harness, const FallbackEngine& fallback,
                          std::string socket, std::size_t path_limit)
        : harness_(harness), fallback_(fallback), path_limit_(path_limit), transport_(options(std::move(socket))) {
        auto promise = std::make_shared<std::promise<protocol::Message>>();
        auto future = promise->get_future();
        transport_.open_session([promise](auto response) { promise->set_value(std::move(response)); });
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) throw std::runtime_error("preview session timeout");
        const auto response = future.get();
        if (const auto* error = std::get_if<protocol::Error>(&response)) throw std::runtime_error(error->message);
        session_id_ = std::get<protocol::OpenSessionResponse>(response).session_id;
    }

    ~LiveMixedModelPreview() {
        alive_.reset();
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        transport_.close_session(session_id_, [promise](auto) { promise->set_value(); });
        (void)future.wait_for(std::chrono::seconds(2));
        transport_.stop();
    }

    LiveMixedModelPreview(const LiveMixedModelPreview&) = delete;
    LiveMixedModelPreview& operator=(const LiveMixedModelPreview&) = delete;

    // Called after a real Engine event and after the client has applied commits.
    void on_key() {
        auto snapshot = capture();
        if (!snapshot) {
            wanted_.reset();
            cache_.reset();
            completed_key_.clear();
            return;
        }
        if (cache_ && compatible_base(*cache_, *snapshot)) {
            auto& live = harness_.session()->mixed_decision;
            bool changed = false;
            for (auto& path : live.result.paths) {
                if (path.rendered == live.result.raw) continue;
                for (const auto& cached : cache_->paths) {
                    bool matching = true;
                    const auto count = std::min(path.segments.size(), cached.path.segments.size());
                    for (std::size_t i = 0; i < count && matching; ++i) {
                        auto& segment = path.segments[i];
                        const auto& old = cached.path.segments[i];
                        matching = same_segment(segment, old);
                        if (matching && segment.kind == MixedSegmentKind::Bopomofo && !old.candidates.empty()) {
                            if (segment.candidates != old.candidates) changed = true;
                            segment.candidates = old.candidates;
                        }
                    }
                    if (matching && count != 0) break;
                }
                path.rendered = render(path);
            }
            if (changed) ++stats.cache_hits;
        }
        // Rendering is not part of the query key. An unfinished tail does not
        // require another model call over the same complete readings.
        if (snapshot->query_key == completed_key_) return;
        if (active_ && wanted_ && wanted_->raw_revision != snapshot->raw_revision) ++stats.coalesced;
        wanted_ = std::move(snapshot);
        if (!active_) start_latest();
    }

    bool idle() const { return !active_ && !wanted_; }
    Stats stats;

private:
    struct PathWork { std::size_t source = 0; MixedPath path; };
    struct Snapshot {
        std::u16string raw;
        std::u16string context;
        std::u16string host_prefix;
        std::u16string query_key;
        std::uint64_t raw_revision = 0;
        std::size_t buffer_revision = 0;
        std::size_t caret = 0;
        std::size_t preview = 0;
        BopomofoKeyboardLayout layout = BopomofoKeyboardLayout::Standard;
        bool sensitive = false;
        std::vector<PathWork> paths;
    };
    struct Job {
        Snapshot snapshot;
        std::size_t path = 0;
        std::size_t segment = 0;
        std::u16string prefix;
        std::vector<std::size_t> run;
        std::uint64_t awaiting_id = 0;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    };

    static ServiceTransportOptions options(std::string socket) {
        ServiceTransportOptions result;
        result.socket_path = std::move(socket);
        result.auto_start = false;
        return result;
    }
    static bool same_segment(const MixedSegment& a, const MixedSegment& b) {
        return a.kind == b.kind && a.begin == b.begin && a.end == b.end && a.raw == b.raw &&
               a.reading == b.reading && a.consumed_boundary == b.consumed_boundary;
    }
    static std::u16string segment_text(const MixedSegment& segment) {
        if (segment.consumed_boundary) return {};
        if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
            return utf8_to_u16(char32_to_utf8(segment.candidates.front()));
        }
        return segment.raw;
    }
    static std::u16string render(const MixedPath& path) {
        std::u16string text;
        for (const auto& segment : path.segments) text += segment_text(segment);
        return text;
    }
    static bool supported(std::u16string_view reading) {
        // Use the same canonical token table passed to the probe's service.
        // The fallback character table also contains legacy/rare syllables
        // which are not necessarily present in the model's reading vocabulary.
        static const auto readings = [] {
            const auto path = std::filesystem::path(LLAVON_IME_TEST_TABLE_PATH).parent_path() / "tokens/bpmf.json";
            std::ifstream file(path);
            if (!file) throw std::runtime_error("missing model reading token table");
            const auto tokens = nlohmann::json::parse(file);
            std::unordered_set<std::u16string> values;
            for (const auto& [token, id] : tokens.items()) {
                (void)id;
                const auto text = utf8_to_u16(token);
                if (text.size() > 2 && text.front() == u'<' && text.back() == u'>') {
                    values.insert(text.substr(1, text.size() - 2));
                }
            }
            return values;
        }();
        return readings.contains(std::u16string(reading));
    }
    static std::u16string signature(const MixedPath& path) {
        std::u16string text;
        const auto last = std::find_if(path.segments.rbegin(), path.segments.rend(), [](const auto& segment) {
            return segment.kind == MixedSegmentKind::Bopomofo && supported(segment.reading);
        });
        if (last == path.segments.rend()) return {};
        const auto count = static_cast<std::size_t>(std::distance(path.segments.begin(), last.base()));
        for (std::size_t i = 0; i < count; ++i) {
            const auto& segment = path.segments[i];
            if (segment.consumed_boundary) continue;
            text += segment.kind == MixedSegmentKind::Bopomofo && supported(segment.reading) ?
                        u"[" + segment.reading + u"]" : u"{" + segment_text(segment) + u"}";
        }
        return text;
    }
    std::u16string host_prefix() const {
        const auto surrounding = harness_.host().surrounding_text(1);
        const auto at = std::min({surrounding.cursor, surrounding.anchor, surrounding.text.size()});
        return surrounding.text.substr(0, at);
    }
    std::optional<Snapshot> capture() const {
        const auto* session = harness_.session();
        if (!session || !harness_.config().smart_english || session->pending_token.empty() ||
            !session->mixed_decision.active() || session->choosing_candidate()) return std::nullopt;
        Snapshot result;
        result.raw = session->pending_token.raw;
        result.raw_revision = session->pending_token.revision;
        result.buffer_revision = session->buffer.revision();
        result.caret = session->buffer.caret();
        result.preview = session->mixed_decision.preview_path;
        result.layout = session->pending_token.layout;
        result.sensitive = harness_.host().is_sensitive(1);
        result.host_prefix = host_prefix();
        result.context = (harness_.host().surrounding_text(1).valid ? result.host_prefix : session->context_text) +
                         session->buffer.rendered_prefix_before_caret();
        if (result.sensitive) result.context.clear();
        result.query_key = result.context + u"\x1f";
        std::unordered_set<std::u16string> seen;
        const auto add = [&](std::size_t index) {
            const auto& path = session->mixed_decision.result.paths.at(index);
            const auto key = signature(path);
            if (key.empty() || !seen.insert(key).second) return;
            result.paths.push_back({index, path});
            result.query_key += key + u"\x1e";
        };
        add(result.preview);
        // With limit=1 only the displayed path is predicted. Larger limits
        // prefetch different reading/Latin boundaries for the candidate panel.
        if (path_limit_ > 1) {
            for (std::size_t i = 1; i < session->mixed_decision.result.paths.size() && result.paths.size() < path_limit_; ++i) {
                add(i);
            }
        }
        if (result.paths.empty()) return std::nullopt;
        return result;
    }
    static bool compatible_base(const Snapshot& a, const Snapshot& b) {
        return a.context == b.context && a.host_prefix == b.host_prefix && a.buffer_revision == b.buffer_revision &&
               a.caret == b.caret && a.layout == b.layout && a.sensitive == b.sensitive;
    }
    bool fresh(const Snapshot& snapshot) const {
        const auto current = capture();
        return current && compatible_base(snapshot, *current) && snapshot.raw_revision == current->raw_revision &&
               snapshot.raw == current->raw && snapshot.preview == current->preview;
    }
    void start_latest() {
        if (!wanted_) return;
        active_ = std::make_shared<Job>();
        active_->snapshot = std::move(*wanted_);
        wanted_.reset();
        advance(active_);
    }
    void advance(const std::shared_ptr<Job>& job) {
        if (!fresh(job->snapshot)) { finish(job, false); return; }
        while (job->path < job->snapshot.paths.size()) {
            auto& path = job->snapshot.paths[job->path].path;
            job->run.clear();
            while (job->segment < path.segments.size()) {
                const auto index = job->segment++;
                const auto& segment = path.segments[index];
                if (segment.consumed_boundary) continue;
                if (segment.kind == MixedSegmentKind::Bopomofo && supported(segment.reading)) {
                    job->run.push_back(index);
                } else if (!job->run.empty()) {
                    --job->segment;
                    break;
                } else {
                    if (segment.kind == MixedSegmentKind::Bopomofo) ++stats.unsupported_readings;
                    job->prefix += segment_text(segment);
                }
            }
            if (job->run.empty()) {
                path.rendered = render(path);
                if (job->path == 0 && job->snapshot.preview != 0 &&
                    job->snapshot.paths.front().source == job->snapshot.preview && fresh(job->snapshot)) {
                    // Publish the displayed path before prefetching alternatives.
                    // Candidate prefetch must not delay the typing preview.
                    harness_.session()->mixed_decision.result.paths[job->snapshot.preview] = path;
                    cache_ = job->snapshot;
                    cache_->paths.resize(1);
                    ++stats.early_best_applied;
                    stats.best_latency_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - job->started).count());
                    harness_.host().update_ui(1);
                }
                ++job->path;
                job->segment = 0;
                job->prefix.clear();
                continue;
            }
            std::vector<protocol::PaddingEntry> padding;
            for (const auto index : job->run) padding.push_back({false, path.segments[index].reading, 0});
            const auto id = ++request_id_;
            job->awaiting_id = id;
            ++stats.requests;
            const std::weak_ptr<bool> alive = alive_;
            const auto reserved = 2 + padding.size() * 2;
            const auto limit = static_cast<std::size_t>(std::max(0, harness_.config().context_length));
            const auto context = utf16_tail(job->snapshot.context + job->prefix, limit > reserved ? limit - reserved : 0);
            transport_.predict(session_id_, id, job->snapshot.raw_revision, context,
                std::move(padding), [this, alive, job](auto response) mutable {
                    if (alive.expired()) return;
                    harness_.host().post([this, alive, job, posted_response = std::move(response)]() mutable {
                        if (alive.expired()) return;
                        reply(job, std::move(posted_response));
                    });
                });
            return;
        }
        finish(job, true);
    }
    void reply(const std::shared_ptr<Job>& job, protocol::Message response) {
        if (active_ != job) return;
        if (!fresh(job->snapshot)) { finish(job, false); return; }
        const auto* prediction = std::get_if<protocol::Prediction>(&response);
        if (!prediction || prediction->session_id != session_id_ || prediction->request_id != job->awaiting_id ||
            prediction->buffer_revision != job->snapshot.raw_revision || prediction->candidates.size() != job->run.size()) {
            if (stats.error_examples.size() < 8) {
                const auto* error = std::get_if<protocol::Error>(&response);
                stats.error_examples.push_back(error ? "service: " + error->message :
                    "correlation/shape mismatch, expected=" + std::to_string(job->run.size()) +
                    ", received=" + std::to_string(prediction ? prediction->candidates.size() : 0));
            }
            ++stats.failures;
            finish(job, false, true);
            return;
        }
        auto& path = job->snapshot.paths[job->path].path;
        for (std::size_t i = 0; i < job->run.size(); ++i) {
            auto& segment = path.segments[job->run[i]];
            const auto allowed = fallback_.lookup(segment.reading);
            std::vector<char32_t> candidates;
            for (const auto candidate : prediction->candidates[i]) {
                if (std::ranges::find(allowed, candidate) != allowed.end() &&
                    std::ranges::find(candidates, candidate) == candidates.end()) candidates.push_back(candidate);
            }
            if (candidates.empty()) {
                if (stats.error_examples.size() < 8) {
                    std::u16string returned;
                    for (const auto candidate : prediction->candidates[i]) returned += utf8_to_u16(char32_to_utf8(candidate));
                    stats.error_examples.push_back("no allowed homophone for " + u16_to_utf8(segment.reading) +
                        ", returned=" + u16_to_utf8(returned));
                }
                ++stats.failures;
                finish(job, false, true);
                return;
            }
            for (const auto candidate : allowed) {
                if (std::ranges::find(candidates, candidate) == candidates.end()) candidates.push_back(candidate);
            }
            segment.candidates = std::move(candidates);
            job->prefix += segment_text(segment);
        }
        advance(job);
    }
    void finish(const std::shared_ptr<Job>& job, bool apply, bool failure = false) {
        if (active_ != job) return;
        if (apply && fresh(job->snapshot)) {
            auto& decision = harness_.session()->mixed_decision;
            for (const auto& work : job->snapshot.paths) {
                if (work.source < decision.result.paths.size() && work.source != 0) decision.result.paths[work.source] = work.path;
            }
            // Preserve the raw path and the user's selected path; deduplicate
            // other model-refined rows without changing a displayed panel.
            const auto selected = decision.result.paths.at(decision.preview_path);
            std::vector<MixedPath> paths{decision.result.paths.front()};
            std::unordered_set<std::u16string> seen{paths.front().rendered};
            if (decision.preview_path != 0) {
                paths.push_back(selected);
                seen.insert(selected.rendered);
            }
            for (std::size_t i = 1; i < decision.result.paths.size(); ++i) {
                if (seen.insert(decision.result.paths[i].rendered).second) paths.push_back(decision.result.paths[i]);
            }
            decision.result.paths = std::move(paths);
            decision.preview_path = decision.preview_path == 0 ? 0 : 1;
            decision.result.best_path = decision.preview_path;
            decision.preview_character = 0;
            cache_ = job->snapshot;
            completed_key_ = job->snapshot.query_key;
            ++stats.applied;
            stats.job_latency_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - job->started).count());
            harness_.host().update_ui(1);
        } else if (failure) {
            completed_key_ = job->snapshot.query_key;
        } else {
            ++stats.stale;
        }
        active_.reset();
        if (wanted_) {
            if (wanted_->query_key == completed_key_) wanted_.reset();
            else start_latest();
        }
    }

    Harness& harness_;
    const FallbackEngine& fallback_;
    std::size_t path_limit_ = 1;
    ServiceTransport transport_;
    protocol::SessionId session_id_{};
    std::uint64_t request_id_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<Job> active_;
    std::optional<Snapshot> wanted_;
    std::optional<Snapshot> cache_;
    std::u16string completed_key_;
};

}  // namespace llavon::ime::rawkey

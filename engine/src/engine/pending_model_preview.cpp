#include "engine/pending_model_preview.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "text/utf.hpp"

namespace llavon::ime {

PendingModelPreview::PendingModelPreview(Client client, const FallbackEngine& fallback,
                                       ServiceTransport& transport, const std::filesystem::path& token_table)
    : client_(std::move(client)), fallback_(fallback), transport_(transport) {
    std::ifstream file(token_table);
    if (!file) return;  // Older installations let the service validate readings.
    const auto tokens = nlohmann::json::parse(file, nullptr, false);
    if (!tokens.is_object()) return;
    readings_.emplace();
    for (const auto& [token, id] : tokens.items()) {
        (void)id;
        const auto value = utf8_to_u16(token);
        if (value.size() > 2 && value.front() == u'<' && value.back() == u'>') {
            readings_->insert(value.substr(1, value.size() - 2));
        }
    }
}

PendingModelPreview::~PendingModelPreview() {
    alive_.reset();
    protocol::SessionId id{};
    {
        std::lock_guard lock(lease_->mutex);
        lease_->abandoned = true;
        id = lease_->id;
        lease_->id = {};
    }
    if (!protocol::is_zero(id)) transport_.close_session(id, [](auto) {});
}

bool PendingModelPreview::supported(std::u16string_view reading) const {
    return !readings_ || readings_->contains(std::u16string(reading));
}

std::u16string PendingModelPreview::text(const MixedSegment& segment) {
    if (segment.consumed_boundary) return {};
    if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
        return utf8_to_u16(char32_to_utf8(segment.candidates.front()));
    }
    return segment.raw;
}

void PendingModelPreview::render(MixedPath& path) {
    path.rendered.clear();
    for (const auto& segment : path.segments) path.rendered += text(segment);
}

std::optional<PendingModelPreview::Snapshot> PendingModelPreview::capture() const {
    auto* session = client_.session();
    const auto& config = client_.config();
    if (!session || !config.smart_english || !config.smart_model_preview || session->pending_token.empty() ||
        !session->mixed_decision.active() || session->choosing_candidate() || session->mixed_decision.preview_path == 0) {
        return std::nullopt;
    }
    Snapshot result;
    result.preview = session->mixed_decision.preview_path;
    if (result.preview >= session->mixed_decision.result.paths.size()) return std::nullopt;
    result.path = session->mixed_decision.result.paths[result.preview];
    result.raw = session->pending_token.raw;
    result.raw_revision = session->pending_token.revision;
    result.buffer_revision = session->buffer.revision();
    result.caret = session->buffer.caret();
    result.layout = session->pending_token.layout;
    result.context_length = config.context_length;
    result.sensitive = client_.sensitive();
    result.context = result.sensitive ? std::u16string{} : client_.context();
    // User-selected characters and exact literals in the buffer are fixed
    // context, never targets of pending-input model selection.
    result.context += session->buffer.rendered_prefix_before_caret();
    const auto last = std::find_if(result.path.segments.rbegin(), result.path.segments.rend(), [this](const auto& segment) {
        return segment.kind == MixedSegmentKind::Bopomofo && supported(segment.reading);
    });
    if (last == result.path.segments.rend()) return std::nullopt;
    const auto count = static_cast<std::size_t>(std::distance(result.path.segments.begin(), last.base()));
    for (std::size_t i = 0; i < count; ++i) {
        const auto& segment = result.path.segments[i];
        if (segment.consumed_boundary) continue;
        const bool reading = segment.kind == MixedSegmentKind::Bopomofo && supported(segment.reading);
        const auto value = reading ? segment.reading : text(segment);
        result.query += (reading ? u"r" : u"t") + utf8_to_u16(std::to_string(value.size())) + u":" + value;
    }
    return result;
}

bool PendingModelPreview::compatible(const Snapshot& a, const Snapshot& b) {
    return a.context == b.context && a.buffer_revision == b.buffer_revision && a.caret == b.caret &&
           a.layout == b.layout && a.sensitive == b.sensitive && a.context_length == b.context_length;
}

bool PendingModelPreview::fresh(const Snapshot& snapshot) const {
    const auto current = capture();
    return current && compatible(snapshot, *current) && snapshot.raw_revision == current->raw_revision &&
           snapshot.raw == current->raw && snapshot.preview == current->preview && snapshot.query == current->query;
}

void PendingModelPreview::on_input() {
    auto current = capture();
    if (!current) {
        wanted_.reset();
        cache_.reset();
        completed_.reset();
        return;
    }
    if (cache_ && compatible(*cache_, *current)) {
        auto& path = client_.session()->mixed_decision.result.paths[current->preview];
        const auto before = path.rendered;
        const auto count = std::min(path.segments.size(), cache_->path.segments.size());
        for (std::size_t i = 0; i < count; ++i) {
            auto& segment = path.segments[i];
            const auto& cached = cache_->path.segments[i];
            if (segment.kind != cached.kind || segment.begin != cached.begin || segment.end != cached.end ||
                segment.reading != cached.reading || segment.raw != cached.raw ||
                segment.consumed_boundary != cached.consumed_boundary) break;
            if (segment.kind == MixedSegmentKind::Bopomofo) segment.candidates = cached.candidates;
        }
        render(path);
        current->path = path;
        if (path.rendered != before) client_.redraw();
    }
    if (completed_ && compatible(*completed_, *current) && completed_->query == current->query) return;
    wanted_ = std::move(current);
    if (!active_) start_latest();
}

void PendingModelPreview::start_latest() {
    if (!wanted_) return;
    auto job = std::make_shared<Job>();
    job->snapshot = std::move(*wanted_);
    wanted_.reset();
    active_ = job;
    if (!fresh(job->snapshot)) { finish(job, false); return; }
    if (protocol::is_zero(session_id_)) open(job);
    else advance(job);
}

void PendingModelPreview::open(const std::shared_ptr<Job>& job) {
    const std::weak_ptr<bool> alive = alive_;
    auto* transport = &transport_;
    const auto post = client_.post;
    const auto lease = lease_;
    transport_.open_session([this, alive, job, transport, post, lease](protocol::Message response) mutable {
        if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&response)) {
            bool abandoned = false;
            {
                std::lock_guard lock(lease->mutex);
                abandoned = lease->abandoned;
                if (!abandoned) lease->id = opened->session_id;
            }
            if (abandoned) transport->close_session(opened->session_id, [](auto) {});
        }
        if (alive.expired()) return;
        post([this, alive, job, posted = std::move(response)]() mutable {
            if (alive.expired() || active_ != job) return;
            if (const auto* opened = std::get_if<protocol::OpenSessionResponse>(&posted)) {
                session_id_ = opened->session_id;
                advance(job);
            } else {
                finish(job, false, true);
            }
        });
    });
}

void PendingModelPreview::advance(const std::shared_ptr<Job>& job) {
    if (!fresh(job->snapshot)) { finish(job, false); return; }
    auto& path = job->snapshot.path;
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
            job->prefix += text(segment);
        }
    }
    if (job->run.empty()) {
        render(path);
        finish(job, true);
        return;
    }
    std::vector<protocol::PaddingEntry> padding;
    for (const auto index : job->run) padding.push_back({false, path.segments[index].reading, 0});
    job->awaiting_id = ++next_request_;
    const auto reserved = 2 + padding.size() * 2;
    const auto limit = static_cast<std::size_t>(std::max(0, job->snapshot.context_length));
    const auto context = utf16_tail(job->snapshot.context + job->prefix, limit > reserved ? limit - reserved : 0);
    const std::weak_ptr<bool> alive = alive_;
    const auto post = client_.post;
    transport_.predict(session_id_, job->awaiting_id, job->snapshot.raw_revision, context, std::move(padding),
        [this, alive, job, post](protocol::Message response) mutable {
            if (alive.expired()) return;
            post([this, alive, job, posted = std::move(response)]() mutable {
                if (!alive.expired()) reply(job, std::move(posted));
            });
        });
}

void PendingModelPreview::reply(const std::shared_ptr<Job>& job, protocol::Message response) {
    if (active_ != job) return;
    const auto* error = std::get_if<protocol::Error>(&response);
    const bool unknown = error && error->code == protocol::ErrorCode::UnknownSession &&
                         error->session_id == session_id_ && error->request_id == job->awaiting_id &&
                         error->buffer_revision == job->snapshot.raw_revision;
    if (unknown) session_id_ = {};
    if (!fresh(job->snapshot)) { finish(job, false); return; }
    if (unknown && !job->reopened) {
        // A service restart must not require another syllable to recover. Each
        // job gets one reopen attempt; repeated failures remain non-blocking.
        job->reopened = true;
        job->segment = 0;
        job->prefix.clear();
        open(job);
        return;
    }
    const auto* prediction = std::get_if<protocol::Prediction>(&response);
    if (!prediction || prediction->session_id != session_id_ || prediction->request_id != job->awaiting_id ||
        prediction->buffer_revision != job->snapshot.raw_revision || prediction->candidates.size() != job->run.size()) {
        finish(job, false, true);
        return;
    }
    for (std::size_t i = 0; i < job->run.size(); ++i) {
        auto& segment = job->snapshot.path.segments[job->run[i]];
        const auto allowed = fallback_.lookup(segment.reading);
        std::vector<char32_t> candidates;
        for (const auto candidate : prediction->candidates[i]) {
            if (std::ranges::find(allowed, candidate) != allowed.end() &&
                std::ranges::find(candidates, candidate) == candidates.end()) candidates.push_back(candidate);
        }
        if (candidates.empty()) { finish(job, false, true); return; }
        for (const auto candidate : allowed) {
            if (std::ranges::find(candidates, candidate) == candidates.end()) candidates.push_back(candidate);
        }
        segment.candidates = std::move(candidates);
        job->prefix += text(segment);
    }
    advance(job);
}

void PendingModelPreview::finish(const std::shared_ptr<Job>& job, bool apply, bool failed) {
    if (active_ != job) return;
    if (apply && fresh(job->snapshot)) {
        client_.session()->mixed_decision.result.paths[job->snapshot.preview] = job->snapshot.path;
        cache_ = job->snapshot;
        completed_ = job->snapshot;
        client_.redraw();
    } else if (failed) {
        // Preserve the current fallback preview, and avoid retrying the same
        // failed query for every unfinished key. The next reading may retry.
        completed_ = job->snapshot;
    }
    active_.reset();
    if (wanted_) {
        if (completed_ && compatible(*completed_, *wanted_) && completed_->query == wanted_->query) wanted_.reset();
        else start_latest();
    }
}

}  // namespace llavon::ime

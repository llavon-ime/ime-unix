#include "session_manager.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace ime::unix_service {

namespace {

template <typename Array>
void random_bytes(Array& bytes) {
    std::random_device device;
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(device());
    if (std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; })) bytes[0] = 1;
}

}  // namespace

SessionManager::SessionManager(std::shared_ptr<CoreRuntime> runtime, SessionLimits limits,
                               EngineFactory engine_factory)
    : runtime_(std::move(runtime)), limits_(limits), engine_factory_(std::move(engine_factory)),
      last_activity_(Clock::now()) {
    if (limits_.max_sessions == 0 || limits_.max_concurrent_predictions == 0) {
        throw std::invalid_argument("session limits must be positive");
    }
    if (limits_.max_idle_sessions == 0) limits_.max_idle_sessions = limits_.max_sessions;
    if (limits_.max_idle_sessions > limits_.max_sessions) limits_.max_idle_sessions = limits_.max_sessions;
    random_bytes(service_epoch_);
    if (!engine_factory_) {
        if (!runtime_) throw std::invalid_argument("session manager requires an ime-core runtime");
        engine_factory_ = [runtime = runtime_]() { return create_session_engine(runtime); };
    }
}

SessionManager::~SessionManager() {
    shutdown();
}

const protocol::ServiceEpoch& SessionManager::service_epoch() const noexcept {
    return service_epoch_;
}

const SessionLimits& SessionManager::limits() const noexcept {
    return limits_;
}

std::size_t SessionManager::SessionIdHash::operator()(const protocol::SessionId& id) const noexcept {
    std::size_t hash = 1469598103934665603ULL;
    for (const auto byte : id) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

OpenSessionResult SessionManager::open_session(std::uint64_t owner_uid) {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return error(protocol::ErrorCode::ServiceShuttingDown, {}, 0, 0, "service is shutting down");

    const auto now = Clock::now();
    reap_locked(now);
    evict_idle_locked(limits_.max_idle_sessions > 0 ? limits_.max_idle_sessions - 1 : 0);
    if (sessions_.size() >= limits_.max_sessions) {
        return error(protocol::ErrorCode::ResourceExhausted, {}, 0, 0, "maximum session count reached");
    }

    auto session = std::make_shared<Session>();
    do {
        random_bytes(session->id);
    } while (sessions_.contains(session->id));
    session->owner_uid = owner_uid;
    session->created_at = now;
    session->last_used = now;
    sessions_.emplace(session->id, session);
    last_activity_ = now;
    return protocol::OpenSessionResponse{session->id, service_epoch_};
}

PredictionResult SessionManager::predict(std::uint64_t owner_uid, const protocol::PredictRequest& request) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            return error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                         request.buffer_revision, "service is shutting down");
        }
        session = find_session_locked(request.session_id);
        if (!session) {
            return error(protocol::ErrorCode::UnknownSession, request.session_id, request.request_id,
                         request.buffer_revision, "unknown session");
        }
        if (!owns(*session, owner_uid)) {
            return error(protocol::ErrorCode::Unauthorized, request.session_id, request.request_id,
                         request.buffer_revision, "session belongs to another user");
        }
    }

    {
        std::lock_guard lock(session->mutex);
        if (session->state == SessionState::Closing) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "session is closing");
        }
        if (session->prediction_in_flight && request.request_id == session->last_request_id) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "duplicate prediction is already in flight");
        }
        if (request.request_id == 0 || request.request_id <= session->last_request_id) {
            return error(protocol::ErrorCode::OutOfOrder, request.session_id, request.request_id,
                         request.buffer_revision, "request id is not strictly increasing");
        }
        if (session->prediction_in_flight) {
            return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                         request.buffer_revision, "session already has a prediction in flight");
        }
        session->prediction_in_flight = true;
        session->last_request_id = request.request_id;
        session->last_used = Clock::now();
    }

    {
        std::unique_lock lock(prediction_mutex_);
        prediction_condition_.wait(lock, [this, &session]() {
            std::lock_guard session_lock(session->mutex);
            return shutting_down_ || session->state == SessionState::Closing ||
                   running_predictions_ < limits_.max_concurrent_predictions;
        });
        {
            std::lock_guard session_lock(session->mutex);
            if (session->state == SessionState::Closing) {
                lock.unlock();
                {
                    std::lock_guard cancelled_lock(session->mutex);
                    session->prediction_in_flight = false;
                }
                session->condition.notify_all();
                return error(protocol::ErrorCode::SessionBusy, request.session_id, request.request_id,
                             request.buffer_revision, "queued prediction was cancelled by close");
            }
        }
        if (shutting_down_) {
            lock.unlock();
            std::lock_guard session_lock(session->mutex);
            session->prediction_in_flight = false;
            session->condition.notify_all();
            return error(protocol::ErrorCode::ServiceShuttingDown, request.session_id, request.request_id,
                         request.buffer_revision, "service is shutting down");
        }
        ++running_predictions_;
    }

    auto finish = [this, &session]() {
        {
            std::lock_guard lock(prediction_mutex_);
            if (running_predictions_ > 0) --running_predictions_;
        }
        prediction_condition_.notify_one();
        {
            std::lock_guard lock(session->mutex);
            session->prediction_in_flight = false;
            session->last_used = Clock::now();
        }
        session->condition.notify_all();
        std::lock_guard lock(mutex_);
        last_activity_ = Clock::now();
    };

    try {
        std::vector<std::vector<char32_t>> candidates;
        {
            std::lock_guard lock(session->mutex);
            if (!session->engine) session->engine = engine_factory_();
            if (!session->engine) throw std::runtime_error("session engine allocation failed");
            candidates = session->engine->predict(request);
        }
        finish();

        if (candidates.size() != request.padding.size()) {
            return error(protocol::ErrorCode::ProtocolError, request.session_id, request.request_id,
                         request.buffer_revision, "prediction segment count does not match padding");
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (request.padding[i].chosen() &&
                (candidates[i].size() != 1 || candidates[i].front() != request.padding[i].chosen_char())) {
                return error(protocol::ErrorCode::ProtocolError, request.session_id, request.request_id,
                             request.buffer_revision, "chosen prediction is not a singleton");
            }
        }
        return protocol::Prediction{request.session_id, request.request_id, request.buffer_revision, std::move(candidates)};
    } catch (const std::bad_alloc&) {
        finish();
        return error(protocol::ErrorCode::ResourceExhausted, request.session_id, request.request_id,
                     request.buffer_revision, "prediction allocation failed");
    } catch (const std::exception& exception) {
        finish();
        return error(protocol::ErrorCode::ModelError, request.session_id, request.request_id,
                     request.buffer_revision, exception.what());
    }
}

CloseSessionResult SessionManager::close_session(std::uint64_t owner_uid, const protocol::SessionId& session_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        session = find_session_locked(session_id);
        if (!session) return error(protocol::ErrorCode::UnknownSession, session_id, 0, 0, "unknown session");
        if (!owns(*session, owner_uid)) return error(protocol::ErrorCode::Unauthorized, session_id, 0, 0, "session belongs to another user");
        {
            std::lock_guard session_lock(session->mutex);
            session->state = SessionState::Closing;
        }
        prediction_condition_.notify_all();
    }

    std::unique_lock session_lock(session->mutex);
    session->condition.wait(session_lock, [&session]() { return !session->prediction_in_flight; });
    session->engine.reset();
    session_lock.unlock();

    {
        std::lock_guard lock(mutex_);
        sessions_.erase(session_id);
        last_activity_ = Clock::now();
    }
    return protocol::CloseSessionResponse{session_id, true};
}

StatusResult SessionManager::status(std::uint64_t owner_uid, const std::optional<protocol::SessionId>& session_id) const {
    StatusResult result;
    protocol::StatusResponse response;
    response.service_epoch = service_epoch_;
    {
        std::lock_guard lock(mutex_);
        response.shutting_down = shutting_down_;
        response.active_sessions = static_cast<std::uint32_t>(std::min<std::size_t>(sessions_.size(), std::numeric_limits<std::uint32_t>::max()));
        response.max_sessions = static_cast<std::uint32_t>(std::min<std::size_t>(limits_.max_sessions, std::numeric_limits<std::uint32_t>::max()));
        response.model_loaded = runtime_ && runtime_->loaded();
        if (session_id) {
            const auto session = find_session_locked(*session_id);
            if (!session) return error(protocol::ErrorCode::UnknownSession, *session_id, 0, 0, "unknown session");
            if (!owns(*session, owner_uid)) return error(protocol::ErrorCode::Unauthorized, *session_id, 0, 0, "session belongs to another user");
            response.session = session_status(*session);
        }
    }
    return response;
}

void SessionManager::reap(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    reap_locked(now);
    evict_idle_locked(limits_.max_idle_sessions);
}

bool SessionManager::should_idle_shutdown(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || !sessions_.empty()) return false;
    std::lock_guard prediction_lock(prediction_mutex_);
    return running_predictions_ == 0 && now - last_activity_ >= limits_.idle_timeout;
}

bool SessionManager::shutting_down() const noexcept {
    std::lock_guard lock(mutex_);
    return shutting_down_;
}

void SessionManager::shutdown() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_ && sessions_.empty()) return;
        shutting_down_ = true;
        for (auto& [id, session] : sessions_) {
            std::lock_guard session_lock(session->mutex);
            session->state = SessionState::Closing;
            sessions.push_back(session);
        }
    }
    {
        std::lock_guard lock(prediction_mutex_);
        prediction_condition_.notify_all();
    }
    for (const auto& session : sessions) {
        std::unique_lock lock(session->mutex);
        session->condition.wait(lock, [&session]() { return !session->prediction_in_flight; });
        session->engine.reset();
    }
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

std::size_t SessionManager::session_count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::shared_ptr<Session> SessionManager::find_session_locked(const protocol::SessionId& id) const {
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

protocol::Error SessionManager::error(protocol::ErrorCode code, const protocol::SessionId& id, std::uint64_t request_id,
                                std::uint64_t revision, std::string message) {
    return protocol::Error{code, id, request_id, revision, std::move(message)};
}

bool SessionManager::owns(const Session& session, std::uint64_t owner_uid) const noexcept {
    return session.owner_uid == owner_uid;
}

void SessionManager::reap_locked(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto& session = it->second;
        bool expired = false;
        {
            std::lock_guard lock(session->mutex);
            expired = session->state == SessionState::Active && !session->prediction_in_flight &&
                      now - session->last_used >= limits_.idle_timeout;
        }
        if (expired) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SessionManager::evict_idle_locked(std::size_t target_count) {
    auto idle_count = [&]() {
        std::size_t count = 0;
        for (const auto& [id, session] : sessions_) {
            std::lock_guard lock(session->mutex);
            if (session->state == SessionState::Active && !session->prediction_in_flight) ++count;
        }
        return count;
    };

    while (idle_count() > target_count) {
        auto oldest = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            const auto& session = it->second;
            std::lock_guard lock(session->mutex);
            if (session->state != SessionState::Active || session->prediction_in_flight) continue;
            if (oldest == sessions_.end() || session->last_used < oldest->second->last_used) oldest = it;
        }
        if (oldest == sessions_.end()) break;
        sessions_.erase(oldest);
    }
}

protocol::SessionStatus SessionManager::session_status(const Session& session) const {
    std::lock_guard lock(session.mutex);
    return protocol::SessionStatus{session.id,
                             session.state == SessionState::Active,
                             session.state == SessionState::Closing,
                             session.engine != nullptr && session.engine->loaded(),
                             session.last_request_id};
}

}  // namespace ime::unix_service

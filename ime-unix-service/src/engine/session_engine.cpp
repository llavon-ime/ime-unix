#include "session_engine.hpp"

#include <stdexcept>
#include <utility>

namespace ime::unix_service {

namespace detail {

std::vector<llavon::ime::core::PaddingEntry> to_core_padding(const protocol::PredictRequest& request) {
    std::vector<llavon::ime::core::PaddingEntry> padding;
    padding.reserve(request.padding.size());
    for (const auto& entry : request.padding) {
        if (entry.chosen() && (entry.chosen_char() == 0 || !protocol::valid_scalar(entry.chosen_char()))) {
            throw std::invalid_argument("chosen padding contains an invalid character");
        }
        padding.push_back(llavon::ime::core::PaddingEntry{
            .chosen = entry.chosen(),
            .chosen_char = entry.chosen_char(),
            .bopomofo = std::u16string(entry.bopomofo()),
        });
    }

    return padding;
}

std::vector<std::vector<char32_t>> to_protocol_candidates(
    const protocol::PredictRequest& request,
    const std::vector<llavon::ime::core::Prediction>& predictions) {
    if (predictions.size() != request.padding.size()) {
        throw std::runtime_error("ime-core prediction segment count does not match padding");
    }

    std::vector<std::vector<char32_t>> result;
    result.reserve(predictions.size());
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        const auto& entry = request.padding[i];
        if (entry.chosen()) {
            result.push_back({entry.chosen_char()});
            continue;
        }

        std::vector<char32_t> candidates;
        candidates.reserve(predictions[i].candidates.size());
        for (const auto& [candidate, probability] : predictions[i].candidates) {
            (void)probability;
            if (candidate == 0 || !protocol::valid_scalar(candidate)) {
                throw std::runtime_error("ime-core returned an invalid candidate character");
            }
            candidates.push_back(candidate);
        }
        result.push_back(std::move(candidates));
    }
    return result;
}

}  // namespace detail

CoreSessionEngine::CoreSessionEngine(std::shared_ptr<CoreRuntime> runtime) {
    if (!runtime) throw std::invalid_argument("session engine requires an ime-core runtime");
    session_ = runtime->create_session();
}

std::vector<std::vector<char32_t>> CoreSessionEngine::predict(const protocol::PredictRequest& request) {
    const auto padding = detail::to_core_padding(request);
    return detail::to_protocol_candidates(request, session_->predict(request.context, padding));
}

bool CoreSessionEngine::loaded() const noexcept {
    return session_ != nullptr;
}

std::unique_ptr<ISessionEngine> create_session_engine(std::shared_ptr<CoreRuntime> runtime) {
    return std::make_unique<CoreSessionEngine>(std::move(runtime));
}

}  // namespace ime::unix_service

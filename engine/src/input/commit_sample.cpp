#include "input/commit_sample.hpp"
#include "text/utf.hpp"

namespace llavon::ime {
namespace {

bool append_literal_tail(InputEffect::CommitSample& sample, std::u16string_view committed) {
    if (!committed.starts_with(sample.answer)) return false;
    const auto tail = utf8_to_u32(u16_to_utf8(committed.substr(sample.answer.size())));
    for (const auto character : tail) {
        if (character == 0 || sample.entries.size() >= 1024) return false;
        sample.entries.push_back({std::u16string{}, character, false, true});
    }
    sample.answer += committed.substr(sample.answer.size());
    return sample.answer == committed && sample.answer.size() <= 1024;
}

std::optional<InputEffect::CommitSample> training_sample(const CompositionBuffer& buffer,
                                                        std::u16string_view committed) {
    if (buffer.segments().empty() || buffer.segments().size() > 1024) return std::nullopt;
    InputEffect::CommitSample sample;
    bool trainable = false;
    for (const auto& segment : buffer.segments()) {
        if (!segment.visible_candidate() || segment.selected_candidate() == 0) return std::nullopt;
        if (segment.literal != 0) {
            sample.entries.push_back({std::u16string{}, segment.literal, false, true});
        } else {
            if (segment.reading().empty()) return std::nullopt;
            sample.entries.push_back({segment.reading(), segment.selected_candidate(), segment.manually_chosen, false});
            trainable = true;
        }
        sample.answer += segment.rendered_text();
    }
    if (!trainable || !append_literal_tail(sample, committed)) return std::nullopt;
    return sample;
}

std::optional<InputEffect::CommitSample> mixed_training_sample(const InputSession& session,
                                                             std::u16string_view committed) {
    if (!session.mixed_decision.active()) return std::nullopt;
    // The longest matching rendering identifies the submitted interpretation;
    // an explicitly paged candidate must not be labelled with a wrong target.
    const MixedPath* chosen = nullptr;
    for (const auto& path : session.mixed_decision.result.paths) {
        if (path.rendered.empty() || !committed.starts_with(path.rendered)) continue;
        if (chosen == nullptr || path.rendered.size() > chosen->rendered.size()) chosen = &path;
    }
    if (chosen == nullptr) return std::nullopt;
    const auto& segments = chosen->segments;
    if (segments.empty() || segments.size() > 1024) return std::nullopt;
    InputEffect::CommitSample sample;
    bool trainable = false;
    for (const auto& segment : segments) {
        if (segment.consumed_boundary) continue;
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            if (segment.reading.empty() || segment.candidates.empty() || segment.candidates.front() == 0) return std::nullopt;
            sample.entries.push_back({segment.reading, segment.candidates.front(), false, false});
            sample.answer += utf8_to_u16(char32_to_utf8(segment.candidates.front()));
            trainable = true;
        } else {
            for (const auto character : utf8_to_u32(u16_to_utf8(segment.raw))) {
                if (character == 0 || sample.entries.size() >= 1024) return std::nullopt;
                sample.entries.push_back({std::u16string{}, character, false, true});
                sample.answer += utf8_to_u16(char32_to_utf8(character));
            }
        }
    }
    if (!trainable || !append_literal_tail(sample, committed)) return std::nullopt;
    return sample;
}

}  // namespace

std::optional<InputEffect::CommitSample> commit_training_sample(const InputSession& session,
                                                               std::u16string_view committed) {
    if (!session.pending_token.empty() && !session.buffer.caret_at_end()) {
        // Materialize at the real caret, not as an append-only suffix.
        auto buffer = session.buffer;
        const auto& decision = session.mixed_decision;
        if (decision.active() && decision.source_revision == session.pending_token.revision &&
            decision.result.raw == session.pending_token.raw && decision.preview_path > 0 &&
            decision.preview_path < decision.result.paths.size()) {
            for (const auto& segment : decision.result.paths[decision.preview_path].segments) {
                if (segment.consumed_boundary) continue;
                if (segment.kind == MixedSegmentKind::Bopomofo) {
                    const auto inserted = buffer.add_bopomofo_keys(segment.body_keys, segment.tone_key,
                        session.pending_token.layout, true, segment.reading);
                    if (!inserted || !inserted->completed || segment.candidates.empty() ||
                        !buffer.set_segment_candidates(inserted->segment_index, segment.candidates)) return std::nullopt;
                } else {
                    for (const char16_t ch : segment.raw) (void)buffer.add_literal(ch);
                }
            }
        } else {
            for (const char16_t ch : session.pending_token.raw) (void)buffer.add_literal(ch);
        }
        return training_sample(buffer, committed);
    }
    auto sample = training_sample(session.buffer, committed);
    if (!sample) sample = mixed_training_sample(session, committed);
    return sample;
}

}  // namespace llavon::ime

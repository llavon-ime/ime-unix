#include "context/marker_transaction.hpp"

#include <algorithm>
#include <utility>

namespace llavon::ime {

MarkerTransaction::Ticket MarkerTransaction::focus(ContextId context) {
    ticket_ = {context, ticket_.generation + 1};
    phase_ = Phase::idle;
    baseline_ = {};
    marker_.clear();
    marked_.clear();
    prefix_.reset();
    return ticket_;
}

void MarkerTransaction::cancel(ContextId context) {
    if (ticket_.context == context) (void)focus(0);
}

bool MarkerTransaction::begin(Ticket ticket, HostContext baseline, std::u16string marker) {
    if (!current(ticket) || phase_ != Phase::idle || !baseline.valid || baseline.text.empty() ||
        baseline.text.size() > 4096 || baseline.cursor != baseline.anchor ||
        baseline.cursor > baseline.text.size() || marker.size() != 128 ||
        baseline.text.find(marker) != std::u16string::npos) return false;
    // Our invisible binary alphabet uses BMP scalars: Fcitx's scalar deletion
    // count equals marker.size(), even when the document contains emoji.
    if (!std::ranges::all_of(marker, [](char16_t unit) { return unit == u'\u2063' || unit == u'\u2064'; })) return false;
    if (baseline.cursor != 0 && baseline.cursor < baseline.text.size() &&
        baseline.text[baseline.cursor] >= 0xdc00 && baseline.text[baseline.cursor] <= 0xdfff) return false;
    baseline_ = std::move(baseline);
    marker_ = std::move(marker);
    marked_ = baseline_.text;
    marked_.insert(baseline_.cursor, marker_);
    prefix_.reset();
    phase_ = Phase::awaiting_insert;
    return true;
}

bool MarkerTransaction::same(const HostContext& first, const HostContext& second) {
    return first.valid && second.valid && first.text == second.text &&
           first.cursor == second.cursor && first.anchor == second.anchor;
}

MarkerTransaction::Action MarkerTransaction::observe(Ticket ticket, const HostContext& fresh) {
    if (!current(ticket) || !fresh.valid) return Action::none;
    if (phase_ == Phase::awaiting_insert) {
        const auto expected_cursor = baseline_.cursor + marker_.size();
        if (fresh.text == marked_ && fresh.cursor == expected_cursor && fresh.anchor == expected_cursor) {
            phase_ = Phase::awaiting_delete;
            return Action::delete_marker;
        }
        // A repeated baseline can arrive before the application processes the
        // commit. It must never be mistaken for a deletion acknowledgement.
        if (!same(fresh, baseline_)) fail(ticket);
    } else if (phase_ == Phase::awaiting_delete) {
        if (same(fresh, baseline_)) {
            phase_ = Phase::scanning;
            return Action::scan;
        }
        const auto expected_cursor = baseline_.cursor + marker_.size();
        if (fresh.text != marked_ || fresh.cursor != expected_cursor || fresh.anchor != expected_cursor) fail(ticket);
    } else if ((phase_ == Phase::scanning || phase_ == Phase::ready) && !same(fresh, baseline_)) {
        fail(ticket);
    }
    return Action::none;
}

bool MarkerTransaction::finish(Ticket ticket, std::optional<std::u16string> scanned_prefix) {
    if (!current(ticket) || phase_ != Phase::scanning) return false;
    // This first Fcitx backend intentionally requires a trustworthy client
    // snapshot as its oracle. Never publish arbitrary text adjacent to a hit.
    if (!scanned_prefix || *scanned_prefix != baseline_.text.substr(0, baseline_.cursor)) {
        fail(ticket);
        return false;
    }
    prefix_ = std::move(scanned_prefix);
    phase_ = Phase::ready;
    return true;
}

void MarkerTransaction::fail(Ticket ticket) {
    if (!current(ticket)) return;
    phase_ = Phase::failed;
    prefix_.reset();
}

HostContext MarkerTransaction::sample(ContextId context) const {
    if (context != ticket_.context || phase_ != Phase::ready || !prefix_) return {};
    return {true, *prefix_, prefix_->size(), prefix_->size()};
}

} // namespace llavon::ime

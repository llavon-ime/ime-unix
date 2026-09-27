#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "host/host.hpp"

namespace llavon::ime {

// Main-thread protocol state. An outbound commit/delete is NOT an
// acknowledgement: only a fresh surrounding-text notification may advance it.
class MarkerTransaction {
public:
    enum class Phase { idle, awaiting_insert, awaiting_delete, scanning, ready, failed };
    enum class Action { none, delete_marker, scan };
    struct Ticket {
        ContextId context = 0;
        std::uint64_t generation = 0;
        bool operator==(const Ticket&) const = default;
    };

    Ticket focus(ContextId context);
    void cancel(ContextId context);
    bool begin(Ticket ticket, HostContext baseline, std::u16string marker);
    Action observe(Ticket ticket, const HostContext& fresh);
    bool finish(Ticket ticket, std::optional<std::u16string> scanned_prefix);
    void fail(Ticket ticket);
    bool current(Ticket ticket) const { return ticket == ticket_ && ticket.context != 0; }
    Ticket ticket() const { return ticket_; }
    Phase phase() const { return phase_; }
    bool editing() const { return phase_ == Phase::awaiting_insert || phase_ == Phase::awaiting_delete; }
    const HostContext& baseline() const { return baseline_; }
    const std::u16string& marker() const { return marker_; }
    const std::u16string& marked_text() const { return marked_; }
    HostContext sample(ContextId context) const;
    static bool same(const HostContext& first, const HostContext& second);

private:
    Ticket ticket_;
    Phase phase_ = Phase::idle;
    HostContext baseline_;
    std::u16string marker_;
    std::u16string marked_;
    std::optional<std::u16string> prefix_;
};

} // namespace llavon::ime

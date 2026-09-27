#include "memory/focus_probe.hpp"
#include <utility>

namespace llavon::ime::memory {
using namespace std::chrono_literals;

FocusProbe::FocusProbe(Hooks hooks) : hooks_(std::move(hooks)), worker_([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::function<void(std::stop_token)> work;
        std::stop_token token;
        {
            std::unique_lock lock(mutex_);
            if (!condition_.wait(lock, stop, [this] { return static_cast<bool>(job_); })) break;
            work = std::exchange(job_, {});
            token = job_token_;
        }
        if (!token.stop_requested()) work(token);
    }
}) {}

FocusProbe::~FocusProbe() {
    alive_.reset();
    job_cancel_.request_stop();
    worker_.request_stop();
    condition_.notify_all();
    worker_.join();
}

void FocusProbe::submit(std::function<void(std::stop_token)> job) {
    job_cancel_.request_stop();
    job_cancel_ = {};
    {
        std::lock_guard lock(mutex_);
        job_token_ = job_cancel_.get_token();
        job_ = std::move(job);
    }
    condition_.notify_one();
}

bool FocusProbe::usable(Ticket ticket) const {
    return transaction_.current(ticket) && hooks_.eligible(ticket.context);
}

void FocusProbe::focus(ContextId context, std::string program) {
    invalidate(transaction_.ticket().context);
    if (failed_programs_.contains(program)) {
        hooks_.status("client-disabled-after-unconfirmed-cleanup");
        return;
    }
    if (!context || !hooks_.eligible(context)) {
        hooks_.status("client-unavailable");
        return;
    }
    program_ = std::move(program);
    (void)transaction_.focus(context);
    waiting_snapshot_ = true;
    deadline_ = Clock::now() + 500ms;
    hooks_.status("waiting-fresh-surrounding");
}

void FocusProbe::drain(bool restored) {
    auto keys = std::exchange(deferred_keys_, {});
    for (auto& replay : keys) replay(restored);
}

void FocusProbe::abort(std::string status) {
    if (transaction_.editing()) {
        failed_programs_.insert(program_);
        status += ":cleanup-unconfirmed";
    }
    waiting_snapshot_ = preparing_ = false;
    job_cancel_.request_stop();
    transaction_.fail(transaction_.ticket());
    hooks_.status(std::move(status));
    drain(false);
}

void FocusProbe::invalidate(ContextId context) {
    if (transaction_.ticket().context != context) return;
    if (active()) abort("cancelled");
    else if (transaction_.phase() == MarkerTransaction::Phase::ready) hooks_.status("invalidated");
    transaction_.cancel(context);
    initial_ = {};
    processes_.clear();
}

bool FocusProbe::active() const {
    return waiting_snapshot_ || preparing_ || transaction_.editing() ||
           transaction_.phase() == MarkerTransaction::Phase::scanning;
}

void FocusProbe::tick() {
    if (!active()) return;
    if (!usable(transaction_.ticket())) abort("client-changed");
    else if (Clock::now() >= deadline_) abort("timeout");
}

void FocusProbe::observe(ContextId context, const HostContext& fresh) {
    const auto ticket = transaction_.ticket();
    if (ticket.context != context) return;
    if (!active() && transaction_.phase() != MarkerTransaction::Phase::ready) return;
    if (active() && Clock::now() >= deadline_) { abort("timeout"); return; }
    if (!usable(ticket) || !fresh.valid) { abort("invalid-surrounding"); return; }
    if (waiting_snapshot_) {
        waiting_snapshot_ = false;
        std::u16string marker;
        try { marker = make_marker(); }
        catch (...) { abort("entropy-unavailable"); return; }
        auto validation = transaction_;
        if (!validation.begin(ticket, fresh, marker)) { abort("unsupported-selection-or-text"); return; }
        preparing_ = true;
        initial_ = fresh;
        deadline_ = Clock::now() + 300ms;
        hooks_.status("checking-process-access");
        const std::weak_ptr<bool> alive = alive_;
        submit([this, alive, ticket, program = program_, marker = std::move(marker)](std::stop_token stop) mutable {
            Discovery result;
            try { result = hooks_.discover(program, stop); }
            catch (...) { result.status = "process-error"; }
            if (stop.stop_requested()) return;
            hooks_.post([this, alive, ticket, marker = std::move(marker), result = std::move(result)]() mutable {
                if (alive.expired() || !transaction_.current(ticket) || !preparing_) return;
                if (Clock::now() >= deadline_) { abort("timeout"); return; }
                if (!usable(ticket) || !MarkerTransaction::same(initial_, hooks_.snapshot(ticket.context))) {
                    abort("client-changed"); return;
                }
                preparing_ = false;
                if (result.processes.empty()) { abort(result.status); return; }
                processes_ = std::move(result.processes);
                if (!transaction_.begin(ticket, initial_, std::move(marker))) { abort("invalid-baseline"); return; }
                deadline_ = Clock::now() + 150ms;
                hooks_.status("awaiting-insert-echo");
                // Copy: a synchronous client echo can invalidate transaction_.
                const auto text = transaction_.marker();
                hooks_.insert(ticket.context, text);
            });
        });
        return;
    }
    if (preparing_) {
        if (!MarkerTransaction::same(initial_, fresh)) abort("client-changed");
        return;
    }
    const bool editing = transaction_.editing();
    const auto action = transaction_.observe(ticket, fresh);
    if (transaction_.phase() == MarkerTransaction::Phase::failed) {
        if (editing) failed_programs_.insert(program_);
        abort(editing ? "unexpected-echo:cleanup-unconfirmed" : "client-changed");
    } else if (action == MarkerTransaction::Action::delete_marker) {
        hooks_.status("awaiting-delete-echo");
        hooks_.remove(context, static_cast<unsigned>(transaction_.marker().size()));
    } else if (action == MarkerTransaction::Action::scan) {
        deadline_ = Clock::now() + 300ms;
        scan(ticket);
        drain(true);
    }
}

void FocusProbe::scan(Ticket ticket) {
    hooks_.status("scanning-residue");
    const std::weak_ptr<bool> alive = alive_;
    submit([this, alive, ticket, processes = processes_, baseline = transaction_.baseline(),
            marker = transaction_.marker()](std::stop_token stop) {
        Capture result;
        try { result = hooks_.capture(processes, baseline.text, baseline.cursor, marker, stop); }
        catch (...) { result.status = "scan-error"; }
        if (stop.stop_requested()) return;
        hooks_.post([this, alive, ticket, result = std::move(result)]() mutable {
            if (alive.expired() || !transaction_.current(ticket) ||
                transaction_.phase() != MarkerTransaction::Phase::scanning) return;
            if (Clock::now() >= deadline_) { abort("timeout"); return; }
            if (!usable(ticket) || !MarkerTransaction::same(transaction_.baseline(), hooks_.snapshot(ticket.context))) {
                abort("client-changed"); return;
            }
            const bool accepted = transaction_.finish(ticket, std::move(result.prefix));
            hooks_.status(accepted ? "ready" : result.status == "ready" ? "invalid-scan-result" : result.status);
        });
    });
}

bool FocusProbe::defer_key(ContextId context, std::function<void(bool)> replay) {
    if (transaction_.ticket().context != context) return false;
    if (waiting_snapshot_ || preparing_) { invalidate(context); return false; }
    if (!transaction_.editing()) return false;
    if (deferred_keys_.size() >= 64) { abort("key-queue-limit"); return false; }
    deferred_keys_.push_back(std::move(replay));
    tick();
    return true;
}

HostContext FocusProbe::sample(ContextId context) {
    if (!transaction_.sample(context).valid) return {};
    if (!usable(transaction_.ticket()) || !MarkerTransaction::same(transaction_.baseline(), hooks_.snapshot(context))) {
        invalidate(context);
        return {};
    }
    return transaction_.sample(context);
}
} // namespace llavon::ime::memory

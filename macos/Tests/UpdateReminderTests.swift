import Foundation

@main
struct UpdateReminderTests {
    static func main() {
        var composition = true
        var probes: [(Bool) -> Void] = []
        var presentations = 0
        var canPresent = true
        let gate = UpdateInstallationGate(hasComposition: { composition },
                                          probeBusy: { probes.append($0) }, waiting: { _ in })
        let reminder = UpdateReadyReminder(gate: gate, present: {
            guard canPresent else { return false }
            presentations += 1
            return true
        })

        reminder.ready(version: "1.0.1")
        reminder.ready(version: "1.0.1")
        precondition(presentations == 0 && probes.isEmpty, "typing must postpone the popup")
        composition = false
        gate.poll()
        probes.removeFirst()(true)
        precondition(presentations == 0, "training must postpone the popup")
        gate.poll()
        composition = true
        probes.removeFirst()(false)
        precondition(presentations == 0, "typing during the probe must postpone the popup")
        composition = false
        gate.poll()
        probes.removeFirst()(false)
        precondition(presentations == 1, "downloaded update must present without an extra click")
        reminder.ready(version: "1.0.1")
        precondition(probes.isEmpty, "a dismissed version must not repeatedly steal focus")

        reminder.ready(version: "1.0.2")
        let stale = probes.removeFirst()
        reminder.userAcknowledged(version: "1.0.2")
        stale(false)
        reminder.ready(version: "1.0.2")
        precondition(presentations == 1 && probes.isEmpty, "manual attention must cancel pending reminders")

        canPresent = false
        reminder.ready(version: "1.0.3")
        probes.removeFirst()(false)
        canPresent = true
        // Exercise the retry timer's action without another ready callback.
        reminder.poll()
        composition = true
        probes.removeFirst()(false)
        precondition(presentations == 1, "a retry must recheck composition")
        composition = false
        gate.poll()
        probes.removeFirst()(false)
        precondition(presentations == 2, "a busy updater must retry without another menu click")

        canPresent = false
        reminder.ready(version: "1.0.4")
        probes.removeFirst()(false)
        reminder.cancel()
        canPresent = true
        reminder.poll()
        precondition(presentations == 2 && probes.isEmpty, "cancel must also stop busy-updater retries")

        reminder.ready(version: "1.0.5")
        let canceled = probes.removeFirst()
        reminder.cancel()
        canceled(false)
        precondition(presentations == 2, "an aborted update must not present a stale popup")

        var installs = 0
        var installProbes: [(Bool) -> Void] = []
        let installGate = UpdateInstallationGate(hasComposition: { false },
                                                 probeBusy: { installProbes.append($0) }, waiting: { _ in })
        installGate.waitUntilIdle { installs += 1 }
        reminder.ready(version: "1.0.6")
        reminder.cancel()
        installProbes.removeFirst()(false)
        probes.removeFirst()(false)
        precondition(installs == 1 && presentations == 2, "reminder cancellation must not cancel installation")

        canPresent = false
        reminder.ready(version: "1.0.7")
        probes.removeFirst()(false)
        canPresent = true
        let deadline = Date().addingTimeInterval(3)
        while probes.isEmpty && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        precondition(!probes.isEmpty, "a busy updater must schedule a real automatic retry")
        probes.removeFirst()(false)
        precondition(presentations == 3, "the automatic retry must open the install prompt")

        reminder.ready(version: "1.0.8")
        let superseded = probes.removeFirst()
        reminder.ready(version: "1.0.9")
        superseded(false)
        precondition(presentations == 3, "a newer download must invalidate the old pending prompt")
        probes.removeFirst()(false)
        precondition(presentations == 4, "the newest downloaded update must still be presented")
        print("downloaded update reminder tests passed")
    }
}

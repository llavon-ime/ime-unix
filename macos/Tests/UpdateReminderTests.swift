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
        let reminder = UpdateReadyReminder(gate: gate, present: { _ in
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
        testRememberedReminders()
        testFailedPresentation()
        print("downloaded update reminder tests passed")
    }

    private static func immediateGate(_ probes: @escaping () -> Void = {}) -> UpdateInstallationGate {
        UpdateInstallationGate(hasComposition: { false },
                               probeBusy: { probes(); $0(false) }, waiting: { _ in })
    }

    // A presented version survives a restart and is reminded again only after
    // the repeat interval.
    static func testRememberedReminders() {
        var now = Date(timeIntervalSince1970: 1_000_000)
        var stored: UpdateReadyReminder.Record?
        var shown: [String] = []
        let make = {
            UpdateReadyReminder(gate: immediateGate(), repeatInterval: 100, now: { now },
                                load: { stored }, save: { stored = $0 },
                                present: { shown.append($0); return true })
        }
        var reminder = make()
        reminder.ready(version: "2.0.0")
        precondition(shown == ["2.0.0"] && stored == .init(version: "2.0.0", date: now),
                     "a presented version must be stored")

        reminder = make()
        now.addTimeInterval(99)
        reminder.ready(version: "2.0.0")
        precondition(shown == ["2.0.0"], "a restart must not repeat a recent reminder")
        reminder.ready(version: "2.0.1")
        precondition(shown == ["2.0.0", "2.0.1"], "a newer version is reminded at once")

        now.addTimeInterval(100)
        reminder.ready(version: "2.0.1")
        precondition(shown == ["2.0.0", "2.0.1", "2.0.1"], "an ignored version is reminded again later")

        reminder.userAcknowledged(version: "2.0.2")
        now.addTimeInterval(50)
        reminder = make()
        reminder.ready(version: "2.0.2")
        precondition(shown.count == 3 && stored?.version == "2.0.2",
                     "an opened update must not be reminded again soon, even after a restart")

        now.addTimeInterval(-1_000)
        reminder.ready(version: "2.0.2")
        precondition(shown.count == 4, "a clock set backwards must not suppress reminders forever")
    }

    // A notification that was not delivered goes back through the idle gate,
    // so the fallback window also waits until typing stopped.
    static func testFailedPresentation() {
        var stored: UpdateReadyReminder.Record?
        var shown: [String] = []
        var probes: [(Bool) -> Void] = []
        let gate = UpdateInstallationGate(hasComposition: { false },
                                          probeBusy: { probes.append($0) }, waiting: { _ in })
        let reminder = UpdateReadyReminder(gate: gate, load: { stored }, save: { stored = $0 },
                                           present: { shown.append($0); return true })
        reminder.ready(version: "3.0.0")
        probes.removeFirst()(false)
        precondition(shown == ["3.0.0"] && stored != nil)

        reminder.presentationFailed(version: "3.0.0")
        precondition(shown.count == 1 && stored == nil && probes.count == 1,
                     "a failed notification must wait for idle again")
        probes.removeFirst()(false)
        precondition(shown == ["3.0.0", "3.0.0"] && stored?.version == "3.0.0",
                     "the fallback presentation must follow")

        reminder.ready(version: "3.0.1")
        reminder.presentationFailed(version: "3.0.0")
        probes.removeFirst()(false)
        precondition(shown == ["3.0.0", "3.0.0", "3.0.1"] && probes.isEmpty,
                     "a late failure for an older version must not replace the newer reminder")
        reminder.cancel()
    }
}

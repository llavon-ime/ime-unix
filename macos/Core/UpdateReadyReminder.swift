import Foundation

// Reminder waiting is independent of the actual installation continuation.
// Opening the consent UI never accepts installation on the user's behalf.
// A presented or acknowledged version is remembered, across restarts when a
// store is given, and is reminded again only after repeatInterval.
final class UpdateReadyReminder {
    struct Record: Equatable {
        var version: String
        var date: Date
    }

    private let gate: UpdateInstallationGate
    private let present: (String) -> Bool
    private let now: () -> Date
    private let repeatInterval: TimeInterval
    private let save: (Record?) -> Void
    private var pendingVersion: String?
    private var presented: Record?
    private var retryTimer: Timer?

    init(gate: UpdateInstallationGate,
         repeatInterval: TimeInterval = 20 * 60 * 60,
         now: @escaping () -> Date = { Date() },
         load: () -> Record? = { nil },
         save: @escaping (Record?) -> Void = { _ in },
         present: @escaping (String) -> Bool) {
        self.gate = gate
        self.present = present
        self.now = now
        self.repeatInterval = repeatInterval
        self.save = save
        presented = load()
    }

    deinit { retryTimer?.invalidate() }

    func ready(version: String) {
        guard version != pendingVersion, !recentlyPresented(version) else { return }
        cancel()
        pendingVersion = version
        poll()
    }

    func poll() {
        retryTimer?.invalidate()
        retryTimer = nil
        guard let version = pendingVersion else { return }
        gate.waitUntilIdle { [weak self] in
            guard let self, self.pendingVersion == version else { return }
            if self.present(version) {
                self.pendingVersion = nil
                self.remember(version)
            } else {
                // Sparkle may still be finishing its background cycle. Retry
                // without requiring another update callback or a menu click.
                let timer = Timer(timeInterval: 1, repeats: false) { [weak self] _ in self?.poll() }
                self.retryTimer = timer
                RunLoop.main.add(timer, forMode: .common)
            }
        }
    }

    func userAcknowledged(version: String) {
        cancel()
        remember(version)
    }

    // An asynchronous presentation (a notification) was not delivered. Wait
    // for idle again before trying the next presentation.
    func presentationFailed(version: String) {
        guard pendingVersion == nil || pendingVersion == version else { return }
        if presented?.version == version {
            presented = nil
            save(nil)
        }
        cancel()
        pendingVersion = version
        poll()
    }

    func cancel() {
        pendingVersion = nil
        retryTimer?.invalidate()
        retryTimer = nil
        gate.cancel()
    }

    private func recentlyPresented(_ version: String) -> Bool {
        guard let presented, presented.version == version else { return false }
        let age = now().timeIntervalSince(presented.date)
        return age >= 0 && age < repeatInterval
    }

    private func remember(_ version: String) {
        let record = Record(version: version, date: now())
        presented = record
        save(record)
    }
}

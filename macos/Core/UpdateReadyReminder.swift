import Foundation

// Reminder waiting is independent of the actual installation continuation.
// Opening the consent UI never accepts installation on the user's behalf.
final class UpdateReadyReminder {
    private let gate: UpdateInstallationGate
    private let present: () -> Bool
    private var pendingVersion: String?
    private var presentedVersion: String?
    private var retryTimer: Timer?

    init(gate: UpdateInstallationGate, present: @escaping () -> Bool) {
        self.gate = gate
        self.present = present
    }

    deinit { retryTimer?.invalidate() }

    func ready(version: String) {
        guard version != pendingVersion, version != presentedVersion else { return }
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
            if self.present() {
                self.pendingVersion = nil
                self.presentedVersion = version
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
        presentedVersion = version
    }

    func cancel() {
        pendingVersion = nil
        retryTimer?.invalidate()
        retryTimer = nil
        gate.cancel()
    }
}

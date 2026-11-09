import Foundation

// A pending installation observes composition; it never finalizes or resets it.
// The process probe runs asynchronously, and composition is checked again when
// its result arrives so typing during the probe cannot slip through the gate.
// The composition is empty between committed characters, so idle also requires
// that no key arrived for quietInterval seconds.
final class UpdateInstallationGate {
    private let hasComposition: () -> Bool
    private let secondsSinceInput: () -> TimeInterval
    private let quietInterval: TimeInterval
    private let probeBusy: (@escaping (Bool) -> Void) -> Void
    private let waiting: (String) -> Void
    private var continuation: (() -> Void)?
    private var generation = UUID()
    private var probing = false
    private var timer: Timer?

    deinit { timer?.invalidate() }

    init(hasComposition: @escaping () -> Bool,
         secondsSinceInput: @escaping () -> TimeInterval = { .infinity },
         quietInterval: TimeInterval = 0,
         probeBusy: @escaping (@escaping (Bool) -> Void) -> Void,
         waiting: @escaping (String) -> Void) {
        self.hasComposition = hasComposition
        self.secondsSinceInput = secondsSinceInput
        self.quietInterval = quietInterval
        self.probeBusy = probeBusy
        self.waiting = waiting
    }

    func waitUntilIdle(_ continuation: @escaping () -> Void) {
        cancel()
        self.continuation = continuation
        let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.poll() }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
        poll()
    }

    func cancel() {
        generation = UUID()
        continuation = nil
        probing = false
        timer?.invalidate()
        timer = nil
    }

    func poll() {
        guard continuation != nil, !probing else { return }
        if let message = inputBusyMessage() {
            waiting(message)
            return
        }
        probing = true
        let current = generation
        probeBusy { [weak self] busy in
            guard let self, self.generation == current else { return }
            self.probing = false
            if busy {
                self.waiting("更新已準備好，等待個人化訓練或管理工作完成…")
            } else if let message = self.inputBusyMessage() {
                self.waiting(message)
            } else {
                let resume = self.continuation
                self.cancel()
                resume?()
            }
        }
    }

    private func inputBusyMessage() -> String? {
        if hasComposition() { return "更新已準備好，等待目前組字完成…" }
        if secondsSinceInput() < quietInterval { return "更新已準備好，等待停止輸入…" }
        return nil
    }
}

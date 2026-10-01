import Foundation

// A pending installation observes composition; it never finalizes or resets it.
// The process probe runs asynchronously, and composition is checked again when
// its result arrives so typing during the probe cannot slip through the gate.
final class UpdateInstallationGate {
    private let hasComposition: () -> Bool
    private let probeBusy: (@escaping (Bool) -> Void) -> Void
    private let waiting: (String) -> Void
    private var continuation: (() -> Void)?
    private var generation = UUID()
    private var probing = false
    private var timer: Timer?

    deinit { timer?.invalidate() }

    init(hasComposition: @escaping () -> Bool,
         probeBusy: @escaping (@escaping (Bool) -> Void) -> Void,
         waiting: @escaping (String) -> Void) {
        self.hasComposition = hasComposition
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
        guard !hasComposition() else {
            waiting("更新已準備好，等待目前組字完成…")
            return
        }
        probing = true
        let current = generation
        probeBusy { [weak self] busy in
            guard let self, self.generation == current else { return }
            self.probing = false
            if busy {
                self.waiting("更新已準備好，等待個人化訓練或管理工作完成…")
            } else if self.hasComposition() {
                self.waiting("更新已準備好，等待目前組字完成…")
            } else {
                let resume = self.continuation
                self.cancel()
                resume?()
            }
        }
    }
}

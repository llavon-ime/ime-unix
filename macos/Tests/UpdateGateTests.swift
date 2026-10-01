import Foundation

@main
struct UpdateGateTests {
    static func main() {
        var composition = true
        var replies: [(Bool) -> Void] = []
        var installs = 0
        var messages: [String] = []
        let gate = UpdateInstallationGate(hasComposition: { composition },
                                          probeBusy: { replies.append($0) },
                                          waiting: { messages.append($0) })

        gate.waitUntilIdle { installs += 1 }
        precondition(installs == 0 && replies.isEmpty, "unfinished text must postpone installation")
        composition = false
        gate.poll()
        precondition(replies.count == 1)
        // The user starts typing while the asynchronous training check runs.
        composition = true
        replies.removeFirst()(false)
        precondition(installs == 0, "composition must be rechecked after the process probe")
        composition = false
        gate.poll()
        replies.removeFirst()(true)
        precondition(installs == 0, "a running training job must postpone installation")
        gate.poll()
        replies.removeFirst()(false)
        gate.poll()
        precondition(installs == 1 && replies.isEmpty, "installation must resume exactly once")
        precondition(messages.count == 3)

        gate.waitUntilIdle { installs += 100 }
        let staleReply = replies.removeFirst()
        gate.cancel()
        gate.waitUntilIdle { installs += 1 }
        staleReply(false)
        precondition(installs == 1, "an old probe must not resume a canceled update")
        replies.removeFirst()(false)
        precondition(installs == 2)
        gate.cancel()
        print("update installation gate tests passed")
    }
}

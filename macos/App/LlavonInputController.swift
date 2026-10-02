import Cocoa
import InputMethodKit

// One controller per client session; every controller owns a ContextId and
// forwards keys, commits and renders through the shared EngineBridge.
@objc(LlavonInputController)
final class LlavonInputController: IMKInputController, EngineHost {
    private let contextId: UInt64
    private let candidatePanel: CandidatePanel
    private let testClient: TextClient?
    private var lastSnapshot: RenderSnapshot?
    private var hasMarkedText = false

    // The client the effects go to: a test double, or the IMK session client.
    private func textClient() -> TextClient? {
        if let testClient { return testClient }
        guard let imk = client() else { return nil }
        return IMKTextClientSink(client: imk)
    }

    override init!(server: IMKServer!, delegate: Any!, client inputClient: Any!) {
        contextId = EngineBridge.shared.allocateContext()
        candidatePanel = CandidatePanel()
        testClient = nil
        super.init(server: server, delegate: delegate, client: inputClient)
        candidatePanel.onSelect = { [weak self] index in
            self?.selectCandidate(index)
        }
        EngineBridge.shared.attach(contextId, host: self)
    }

    // Test seam: drives the controller without an IMK session (IMKInputController
    // rejects clients that are not real IMK proxies). The raw-input harness
    // builds the controller this way and feeds it NSEvents.
    init(testClient: TextClient) {
        contextId = EngineBridge.shared.allocateContext()
        candidatePanel = CandidatePanel()
        self.testClient = testClient
        super.init()
        candidatePanel.onSelect = { [weak self] index in
            self?.selectCandidate(index)
        }
        EngineBridge.shared.attach(contextId, host: self)
    }

    deinit {
        EngineBridge.shared.detach(contextId)
    }

    // MARK: - IMKInputController

    override func recognizedEvents(_ sender: Any!) -> Int {
        let events: NSEvent.EventTypeMask = [.keyDown, .keyUp]
        return Int(events.rawValue)
    }

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event else { return false }
        switch event.type {
        case .keyDown, .keyUp:
            return EngineBridge.shared.sendKey(contextId,
                                               keyCode: event.keyCode,
                                               charactersIgnoringModifiers: event.charactersIgnoringModifiers,
                                               modifiers: Keysym.modifiers(for: event),
                                               capsLock: event.modifierFlags.contains(.capsLock),
                                               isRelease: event.type == .keyUp)
        default:
            return false
        }
    }

    override func activateServer(_ sender: Any!) {
        // The fcitx5 addon reloads the settings and phrase overrides on every
        // activation so edits made outside the app take effect.
        EngineBridge.shared.reloadConfigFromDisk()
        EngineBridge.shared.activate(contextId)
        super.activateServer(sender)
    }

    override func deactivateServer(_ sender: Any!) {
        EngineBridge.shared.deactivate(contextId)
        candidatePanel.hide()
        hasMarkedText = false
        super.deactivateServer(sender)
    }

    override func commitComposition(_ sender: Any!) {
        EngineBridge.shared.finalize(contextId)
    }

    // Keep the input menu minimal; all management pages live in Settings.
    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "LlavonIME")
        let settings = menu.addItem(withTitle: "設定…",
                                    action: #selector(openSettings),
                                    keyEquivalent: "")
        settings.target = self
        let restart = menu.addItem(withTitle: "重新啟動",
                                   action: #selector(restartPredictionService),
                                   keyEquivalent: "")
        restart.target = self
        return menu
    }

    @objc private func openSettings() {
        if !EngineBridge.shared.openSettingsApp() {
            let alert = NSAlert()
            alert.messageText = "找不到拉風設定與個人化程式"
            alert.informativeText = "請重新安裝包含設定程式的完整套件。"
            alert.runModal()
        }
    }

    // Drops the prediction service; the engine starts a fresh process on the
    // next prediction. Settings are re-read first so external edits apply.
    @objc private func restartPredictionService() {
        EngineBridge.shared.reloadAndRestartPredictionService()
    }

    // MARK: - Host callbacks

    func insertCommit(_ text: String) {
        guard let textClient = textClient() else { return }
        textClient.insertText(text, replacementRange: NSRange(location: NSNotFound, length: 0))
        hasMarkedText = false
    }

    func refreshUI() {
        guard let textClient = textClient(),
              let snapshot = EngineBridge.shared.snapshot(contextId) else { return }
        lastSnapshot = snapshot

        if snapshot.compositionEmpty && snapshot.preedit.isEmpty {
            if hasMarkedText {
                textClient.setMarkedText(NSAttributedString(string: ""),
                                         selectionRange: NSRange(location: 0, length: 0),
                                         replacementRange: NSRange(location: NSNotFound, length: 0))
                hasMarkedText = false
            }
        } else {
            let marked = NSMutableAttributedString()
            for segment in snapshot.preedit {
                var attributes: [NSAttributedString.Key: Any] = [:]
                if segment.underlined {
                    attributes[.underlineStyle] = NSUnderlineStyle.single.rawValue
                }
                marked.append(NSAttributedString(string: segment.text, attributes: attributes))
            }
            let caret = min(max(snapshot.caret, 0), marked.length)
            textClient.setMarkedText(marked,
                                     selectionRange: NSRange(location: caret, length: 0),
                                     replacementRange: NSRange(location: NSNotFound, length: 0))
            hasMarkedText = true
        }

        if snapshot.hasCandidates {
            let level = NSWindow.Level(rawValue: Int(textClient.windowLevel()) + 1)
            candidatePanel.update(snapshot: snapshot)
            candidatePanel.show(anchoredTo: anchorRect(client: textClient), level: level)
        } else {
            candidatePanel.hide()
        }
    }

    // Text before the caret, as UTF-16 units (the engine clips it further).
    // Like the fcitx5 addon, the sample never includes the composition itself:
    // it stops at the client's marked range, so the model sees committed text
    // only instead of its own in-progress guess.
    func surroundingSample() -> (text: [UInt16], cursor: Int, anchor: Int)? {
        guard let textClient = textClient() else { return nil }
        let selected = textClient.selectedRange()
        guard selected.location != NSNotFound else { return nil }

        var end = selected.location
        let marked = textClient.markedRange()
        if marked.location != NSNotFound && marked.location < end {
            end = marked.location
        }

        let limit = max(EngineBridge.shared.contextLength, 1)
        let start = max(0, end - limit)
        guard end > start else { return nil }
        let range = NSRange(location: start, length: end - start)
        guard let attribute = textClient.attributedSubstring(from: range) else { return nil }
        let units = Array(attribute.string.utf16)
        return (units, units.count, units.count)
    }

    // MARK: - Helpers

    private func selectCandidate(_ index: Int) {
        guard let snapshot = lastSnapshot else { return }
        if snapshot.target == RenderTargetC.symbolMenu {
            EngineBridge.shared.selectSymbol(contextId, index: Int32(index), epoch: snapshot.symbolEpoch)
        } else if snapshot.target == RenderTargetC.candidates {
            EngineBridge.shared.selectCandidate(contextId, index: Int32(index))
        }
    }

    private func anchorRect(client: TextClient) -> NSRect {
        var rect = NSRect.zero
        _ = client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
        return rect
    }
}

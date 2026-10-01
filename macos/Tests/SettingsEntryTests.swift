import Cocoa
import InputMethodKit
import ApplicationServices

private final class InertClient: TextClient {
    func insertText(_ text: String, replacementRange: NSRange) {}
    func setMarkedText(_ text: NSAttributedString, selectionRange: NSRange, replacementRange: NSRange) {}
    func selectedRange() -> NSRange { NSRange(location: 0, length: 0) }
    func markedRange() -> NSRange { NSRange(location: NSNotFound, length: 0) }
    func attributedSubstring(from range: NSRange) -> NSAttributedString? { nil }
    func attributes(forCharacterIndex index: Int, lineHeightRectangle: UnsafeMutablePointer<NSRect>?) -> [AnyHashable: Any]? { nil }
    func windowLevel() -> Int32 { 0 }
}

// Drives the real IMK controller's NSMenu targets, without registering a second
// input source or changing the user's active input method/configuration.
@main struct SettingsEntryTests {
    @MainActor static func main() throws {
        setbuf(stdout, nil)
        _ = NSApplication.shared
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("settings entry \(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let launcher = root.appendingPathComponent("settings launcher")
        let result = root.appendingPathComponent("arguments")
        try "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LLAVON_TEST_ARGS\"\n".write(to: launcher, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
        setenv("LLAVON_IME_SETTINGS_APP_PATH", launcher.path, 1)
        setenv("LLAVON_TEST_ARGS", result.path, 1)
        setenv("XDG_CONFIG_HOME", root.path, 1)
        let controller = LlavonInputController(testClient: InertClient())
        let menu = controller.menu()!
        func expect(_ page: String, _ action: () -> Void) throws {
            try? FileManager.default.removeItem(at: result)
            action()
            let expected = "--page\n\(page)\n"
            let deadline = Date().addingTimeInterval(3)
            while Date() < deadline {
                if (try? String(contentsOf: result, encoding: .utf8)) == expected {
                    print("PASS macOS original entry → \(page)"); return
                }
                RunLoop.main.run(until: Date().addingTimeInterval(0.02))
            }
            fatalError("Original entry did not launch \(page)")
        }
        for (title, page) in [("設定…", "settings"), ("管理個人化訓練…", "records")] {
            let index = menu.indexOfItem(withTitle: title)
            precondition(index >= 0 && menu.item(at: index)?.target === controller)
            try expect(page) { menu.performActionForItem(at: index) }
        }
        try expect("phrases") { precondition(EngineBridge.shared.openSettingsApp(page: "phrases")) }
        try expect("updates") { UpdateController.shared.showSettings(nil) }
        // Retain the old launcher override used by existing development setups.
        unsetenv("LLAVON_IME_SETTINGS_APP_PATH")
        setenv("LLAVON_IME_LORA_GUI_PATH", launcher.path, 1)
        try expect("records") { precondition(EngineBridge.shared.openLoraManager()) }
        print("PASS menu targets, legacy entry points and paths containing spaces")
        setenv("LLAVON_IME_SETTINGS_PORT", "org.llavon-ime.settings.test.\(UUID().uuidString)", 1)
        let settingsHost = SettingsHost { UpdateController.shared.settingsRequest($0) }
        precondition(settingsHost.start())
        defer { settingsHost.stop() }
        if let launcherPath = ProcessInfo.processInfo.environment["LLAVON_TEST_REAL_GUI"] {
            // Record arguments and completion separately: recording argv alone
            // does not prove a warm-launch process has reached the IPC server.
            let completed = root.appendingPathComponent("completed")
            setenv("LLAVON_TEST_DONE", completed.path, 1)
            try "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LLAVON_TEST_ARGS\"\n\"$LLAVON_TEST_REAL_GUI\" \"$@\"\nprintf '%s\\n' \"$@\" > \"$LLAVON_TEST_DONE\"\n".write(to: launcher, atomically: true, encoding: .utf8)
            setenv("LLAVON_IME_SETTINGS_APP_PATH", launcher.path, 1)
            setenv("XDG_STATE_HOME", root.appendingPathComponent("state").path, 1)
            let expectedExecutable = URL(fileURLWithPath: launcherPath).deletingLastPathComponent()
                .appendingPathComponent("llavon-ime-lora-gui.app/Contents/MacOS/llavon-ime-lora-gui").path
            func wait(_ condition: () -> Bool) {
                let deadline = Date().addingTimeInterval(10)
                while Date() < deadline {
                    if condition() { return }
                    RunLoop.main.run(until: Date().addingTimeInterval(0.05))
                }
                fatalError("Timed out waiting for real settings app")
            }
            func visible(_ pid: pid_t) -> Bool {
                let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as? [[String: Any]] ?? []
                return windows.contains { $0[kCGWindowOwnerPID as String] as? Int32 == pid && $0[kCGWindowLayer as String] as? Int == 0 }
            }
            func warm(_ page: String, _ action: () -> Void) throws {
                try? FileManager.default.removeItem(at: completed)
                try expect(page, action)
                wait { (try? String(contentsOf: completed, encoding: .utf8)) == "--page\n\(page)\n" }
            }
            func running() -> NSRunningApplication? {
                NSWorkspace.shared.runningApplications.first { $0.executableURL?.path == expectedExecutable && !$0.isTerminated }
            }
            func quit(_ application: NSRunningApplication) {
                precondition(kill(application.processIdentifier, SIGTERM) == 0)
            }
            precondition(running() == nil, "Close this staged settings app before testing")
            var launched: NSRunningApplication?
            defer { if let launched, !launched.isTerminated { kill(launched.processIdentifier, SIGTERM) } }
            try expect("settings") { menu.performActionForItem(at: menu.indexOfItem(withTitle: "設定…")) }
            wait { running() != nil }; launched = running()
            let firstPID = launched!.processIdentifier
            wait { visible(firstPID) }
            print("PASS real macOS Settings menu cold launch")
            launched?.hide(); wait { launched!.isHidden }
            try warm("records") { menu.performActionForItem(at: menu.indexOfItem(withTitle: "管理個人化訓練…")) }
            wait { !launched!.isHidden && visible(firstPID) }
            precondition(running()?.processIdentifier == firstPID)
            print("PASS real training entry restores hidden existing app")
            try warm("phrases") { precondition(EngineBridge.shared.openSettingsApp(page: "phrases")) }
            try warm("updates") { UpdateController.shared.showSettings(nil) }
            try warm("settings") { menu.performActionForItem(at: menu.indexOfItem(withTitle: "設定…")) }
            wait { visible(firstPID) }; precondition(running()?.processIdentifier == firstPID)
            print("PASS real legacy phrases and settings entries reuse one process")
            quit(launched!); wait { running() == nil }
            try expect("records") { menu.performActionForItem(at: menu.indexOfItem(withTitle: "管理個人化訓練…")) }
            wait { running() != nil }; launched = running()
            wait { visible(launched!.processIdentifier) }
            print("PASS real training entry relaunch after app closes")
            quit(launched!); wait { running() == nil }
        }
    }
}

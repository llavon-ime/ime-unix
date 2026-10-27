import Cocoa
import InputMethodKit

@main
struct LlavonIMEApp {
    // NSApplication.delegate is not retaining; keep the delegate alive for the
    // whole process.
    private static let delegate = LlavonAppDelegate()

    static func main() {
        let application = NSApplication.shared
        application.delegate = delegate
        application.run()
    }
}

final class LlavonAppDelegate: NSObject, NSApplicationDelegate {
    private var server: IMKServer?
    private var settingsHost: SettingsHost?

    func applicationDidFinishLaunching(_ notification: Notification) {
        EngineBridge.shared.startResolved()

        let identifier = Bundle.main.bundleIdentifier ?? "com.llavon.inputmethod.LlavonIME"
        let connectionName = Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String
            ?? "\(identifier)_Connection"
        server = IMKServer(name: connectionName, bundleIdentifier: identifier)
        NSLog("llavon-ime: input method server started on \(connectionName)")
        UpdateController.shared.start()
        let settingsHost = SettingsHost { UpdateController.shared.settingsRequest($0) }
        if !settingsHost.start() { NSLog("llavon-ime: settings bridge could not start") }
        self.settingsHost = settingsHost
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard UpdateController.shared.preparingInstallation else { return .terminateNow }
        UpdateController.shared.waitUntilIdle { sender.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        settingsHost?.stop(); settingsHost = nil
        server = nil
        EngineBridge.shared.stop()
    }
}

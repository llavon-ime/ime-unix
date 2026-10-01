import Foundation
import CoreFoundation
#if os(macOS)

// Small same-user transport for the shared settings app. Preferences remain
// owned by Sparkle in the running IMK host, not a second updater in the GUI.
@MainActor
final class SettingsHost {
    private var port: CFMessagePort?
    private var source: CFRunLoopSource?
    private let handle: ([String: Any]) -> [String: Any]

    init(handle: @escaping ([String: Any]) -> [String: Any]) { self.handle = handle }

    @discardableResult func start() -> Bool {
        guard port == nil else { return true }
        let name = ProcessInfo.processInfo.environment["LLAVON_IME_SETTINGS_PORT"]
            ?? "org.llavon-ime.settings.\(getuid())"
        var context = CFMessagePortContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(),
                                           retain: nil, release: nil, copyDescription: nil)
        guard let port = CFMessagePortCreateLocal(nil, name as CFString, { _, _, data, info in
            guard let data, let info, CFDataGetLength(data) <= 4096,
                  let request = try? JSONSerialization.jsonObject(with: data as Data) as? [String: Any] else { return nil }
            let host = Unmanaged<SettingsHost>.fromOpaque(info).takeUnretainedValue()
            let reply = MainActor.assumeIsolated { host.handle(request) }
            guard let encoded = try? JSONSerialization.data(withJSONObject: reply) else { return nil }
            return Unmanaged.passRetained(encoded as CFData)
        }, &context, nil), let source = CFMessagePortCreateRunLoopSource(nil, port, 0) else { return false }
        self.port = port; self.source = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        return true
    }

    func stop() {
        if let source { CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes) }
        if let port { CFMessagePortInvalidate(port) }
        source = nil; port = nil
    }
}
#endif

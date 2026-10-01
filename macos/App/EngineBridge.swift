import Cocoa
import Carbon

// macOS specialization of the engine core: posts engine callbacks to the main
// queue, reads secure-input state and resolves the packaged resource paths.
final class EngineBridge: EngineCore {
    static let shared = EngineBridge()

    private let supportRoot = "/Library/Application Support/llavon-ime"

    // Settings and personalization share one native app. Opening a page also
    // activates that page when the app is already running.
    func openSettingsApp(page: String = "settings") -> Bool {
        let environment = ProcessInfo.processInfo.environment
        let candidates = [
            environment["LLAVON_IME_SETTINGS_APP_PATH"],
            environment["LLAVON_IME_LORA_GUI_PATH"],
            "\(supportRoot)/payload/bin/llavon-ime-lora-gui",
            "\(home)/Library/fcitx5/bin/llavon-ime-lora-gui",
        ].compactMap { $0 }
        guard let path = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else {
            return false
        }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = ["--page", page]
        do { try process.run(); return true }
        catch { NSLog("llavon-ime: cannot open LoRA manager: \(error)"); return false }
    }

    func openLoraManager() -> Bool { openSettingsApp(page: "records") }

    // Set by the caller before launch to point the prediction service at a
    // development model. Captured here because applyServiceEnvironment() writes
    // the settings-file value into the same variable.
    private let modelPathOverride: String?

    private override init() {
        modelPathOverride = ProcessInfo.processInfo.environment["LLAVON_IME_MODEL_PATH"]
        super.init()
        setPostToMain { work in
            if Thread.isMainThread {
                work()
            } else {
                DispatchQueue.main.async(execute: work)
            }
        }
        setSensitiveCheck { IsSecureEventInputEnabled() }
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(),
                                        Unmanaged.passUnretained(self).toOpaque(),
                                        { _, observer, _, _, _ in
            guard let observer else { return }
            let bridge = Unmanaged<EngineBridge>.fromOpaque(observer).takeUnretainedValue()
            DispatchQueue.main.async { bridge.reloadConfigFromDisk() }
        }, "org.llavon-ime.lora.model-changed" as CFString, nil, .deliverImmediately)
    }

    private var configRootURL: URL {
        if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
            return URL(fileURLWithPath: xdg, isDirectory: true)
        }
        return URL(fileURLWithPath: home, isDirectory: true).appendingPathComponent(".config", isDirectory: true)
    }

    var configFileURL: URL {
        configRootURL.appendingPathComponent("llavon-ime/config.json")
    }

    var phraseOverridesURL: URL {
        configRootURL.appendingPathComponent("llavon-ime/phrase_overrides.txt")
    }

    // Effective prediction context window; the surrounding sample and the
    // service runtime follow the settings file, like the fcitx5 addon.
    private(set) var contextLength = 512

    func startResolved() {
        let configJson = effectiveConfigJSON()
        let serviceModelPath = modelPathOverride
        start(options: EngineStartOptions(tablePath: resolveTablePath(),
                                          phraseOverridesPath: resolvePhraseOverridesPath(),
                                          servicePath: resolveServicePath(),
                                          modelPath: serviceModelPath,
                                          tablesDir: resolveTablesDir(),
                                          configJson: configJson,
                                          autoStartService: true,
                                          // InputMethodKit supplies surrounding text.
                                          enableAccessibility: false))
        applyServiceEnvironment()
    }

    // Mirrors the fcitx5 addon's reload_config(): re-reads the settings file on
    // activation, without settling the sessions, and refreshes the phrase
    // overrides so external edits take effect.
    func reloadConfigFromDisk() {
        reloadPhraseOverrides()
        guard let configJson = effectiveConfigJSON() else { return }
        let previous = ConfigJSON.object(self.configJson())
        _ = reloadConfigJson(configJson)
        // External edits to the service settings need the same restart the
        // settings window asks for; the engine alone cannot apply them.
        if let previous, let current = ConfigJSON.object(self.configJson()),
           Self.serviceSettingsChanged(from: previous, to: current) {
            applyServiceEnvironment()
            restartPredictionService()
        }
    }

    // The settings file with the effective model path filled in: the config
    // carries it (the installed model when unset), so the settings window shows
    // and saves it. The environment override only changes the path the
    // prediction service is started with.
    private func effectiveConfigJSON() -> String? {
        var configJson: String?
        if let data = try? Data(contentsOf: configFileURL), !data.isEmpty {
            configJson = String(data: data, encoding: .utf8)
        }
        let effectiveModelPath = configuredModelPath(in: configJson) ?? installedModelPath()
        let filled = ConfigJSON.fillingModelPath(configJson, with: effectiveModelPath)
        if let length = configuredContextLength(in: filled) {
            contextLength = length
        }
        return filled
    }

    private func configuredContextLength(in json: String?) -> Int? {
        guard let json, let data = json.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let value = object["context_length"] as? Int, value > 0 else {
            return nil
        }
        return value
    }

    // MARK: - Prediction service

    // Settings only the service process reads; the engine passes them on the
    // command line when it spawns the service.
    private static let serviceSettings = [
        "model_path",
        "context_length",
        "thread_count",
        "gpu_layers",
        "idle_timeout_seconds",
    ]

    private static func serviceSettingsChanged(from previous: [String: Any], to next: [String: Any]) -> Bool {
        return serviceSettings.contains { (previous[$0] as? NSObject) != (next[$0] as? NSObject) }
    }

    // The service inherits the app's environment, so the settings it owns are
    // passed here instead of through the command line the engine captured when
    // it was created. Refreshed at launch and whenever they are saved.
    private func applyServiceEnvironment() {
        guard let config = ConfigJSON.object(configJson()) else { return }
        let values: [(String, String?)] = [
            ("LLAVON_IME_MODEL_PATH", modelPathOverride ?? config["model_path"] as? String),
            ("LLAVON_IME_CONTEXT_LENGTH", (config["context_length"] as? Int).map { String($0) }),
            ("LLAVON_IME_THREADS", (config["thread_count"] as? Int).map { String($0) }),
            ("LLAVON_IME_GPU_LAYERS", (config["gpu_layers"] as? Int).map { $0 == -2 ? "auto" : String($0) }),
            ("LLAVON_IME_IDLE_TIMEOUT", (config["idle_timeout_seconds"] as? Int).map { String($0) }),
        ]
        for (name, value) in values {
            if let value {
                _ = setenv(name, value, 1)
            } else {
                _ = unsetenv(name)
            }
        }
    }

    // The engine spawns the service lazily, so shutting the running one down is
    // enough: the next prediction starts a fresh process, which reads the
    // settings file again and therefore ignores the stale command line.
    func restartPredictionService() {
        guard let path = serviceSocketPath() else { return }
        let descriptor = socket(AF_UNIX, SOCK_STREAM, 0)
        guard descriptor >= 0 else { return }
        defer { close(descriptor) }

        var timeout = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))

        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(path.utf8)
        guard pathBytes.count < MemoryLayout.size(ofValue: address.sun_path) else { return }
        withUnsafeMutableBytes(of: &address.sun_path) { buffer in
            buffer.copyBytes(from: pathBytes)
        }

        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                connect(descriptor, socketAddress, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connected == 0 else { return }

        // MessageType::Shutdown (6) with payload kind 0, behind the 4-byte
        // little-endian frame length the service protocol uses.
        let frame: [UInt8] = [2, 0, 0, 0, 6, 0]
        _ = frame.withUnsafeBytes { write(descriptor, $0.baseAddress, frame.count) }
        var response = [UInt8](repeating: 0, count: 8)
        _ = read(descriptor, &response, response.count)
    }

    // The input menu's 「重新啟動」: re-read the settings file so edits made
    // outside the app take effect, refresh the environment the service
    // inherits, then drop the running service. The engine starts a fresh one on
    // the next prediction.
    func reloadAndRestartPredictionService() {
        reloadConfigFromDisk()
        applyServiceEnvironment()
        restartPredictionService()
    }

    // Mirrors ServiceTransport::default_socket_path in the engine.
    private func serviceSocketPath() -> String? {
        let environment = ProcessInfo.processInfo.environment
        if let override = environment["LLAVON_IME_UNIX_SOCKET_PATH"], !override.isEmpty {
            return override
        }
        let runtime = environment["XDG_RUNTIME_DIR"] ?? environment["TMPDIR"] ?? "/tmp"
        return URL(fileURLWithPath: runtime, isDirectory: true)
            .appendingPathComponent("llavon-ime/ime.sock").path
    }

    // MARK: - Paths

    private var payloadRoot: String { supportRoot + "/payload" }
    private var home: String { FileManager.default.homeDirectoryForCurrentUser.path }

    private func resolveTablePath() -> String {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_TABLE_PATH"] { return value }
        let candidates = [
            payloadRoot + "/share/llavon-ime/tables/bopomofo_char.json",
            home + "/Library/fcitx5/share/llavon-ime/tables/bopomofo_char.json",
        ]
        return firstExisting(candidates) ?? candidates[0]
    }

    private func resolvePhraseOverridesPath() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_PHRASE_OVERRIDES_PATH"] { return value }
        // Keep the engine default (~/.config/llavon-ime/phrase_overrides.txt,
        // XDG aware) so the native app and the fcitx5 addon share one file.
        return nil
    }

    private func resolveServicePath() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_UNIX_SERVICE_PATH"] { return value }
        return firstExisting([
            payloadRoot + "/bin/llavon-ime-unix-service",
            home + "/Library/fcitx5/bin/llavon-ime-unix-service",
        ])
    }

    private func resolveTablesDir() -> String? {
        if let value = ProcessInfo.processInfo.environment["LLAVON_IME_TABLES_DIR"] { return value }
        return firstExisting([
            payloadRoot + "/share/llavon-ime/tables",
            home + "/Library/fcitx5/share/llavon-ime/tables",
        ])
    }

    private func installedModelPath() -> String? {
        firstExisting([
            supportRoot + "/models/llavon-ime-llama-250m-Q4_K_M.gguf",
        ])
    }

    private func configuredModelPath(in json: String?) -> String? {
        guard let json, let data = json.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let value = object["model_path"] as? String, !value.isEmpty else {
            return nil
        }
        return value
    }

    private func firstExisting(_ candidates: [String]) -> String? {
        candidates.first { FileManager.default.fileExists(atPath: $0) }
    }
}

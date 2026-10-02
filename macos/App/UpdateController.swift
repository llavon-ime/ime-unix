import Cocoa
import Sparkle

// Every installation source, including Homebrew, uses this same updater.
// Sparkle owns downloads, signature checks, authorization and pkg installation.
@MainActor
final class UpdateController: NSObject, SPUUpdaterDelegate, SPUStandardUserDriverDelegate {
    static let shared = UpdateController()

    private var controller: SPUStandardUpdaterController?
    private var availableVersion: String?
    private var readyVersion: String?
    private var readyOnQuit = false
    private(set) var preparingInstallation = false
    private(set) var status = "開發版本未啟用自動更新"

    private lazy var installationGate = UpdateInstallationGate(
        hasComposition: { EngineBridge.shared.hasPendingComposition },
        probeBusy: Self.probeTraining,
        waiting: { [weak self] message in self?.setStatus(message) })

    private lazy var readyReminder = UpdateReadyReminder(
        gate: UpdateInstallationGate(
            hasComposition: { EngineBridge.shared.hasPendingComposition },
            probeBusy: Self.probeTraining,
            waiting: { [weak self] _ in
                self?.setStatus("更新已下載；組字與訓練完成後會顯示安裝提示，也可按「安裝並重新啟動…」。")
            }),
        present: { [weak self] in
            guard let self, let updater = self.updater,
                  updater.canCheckForUpdates, !self.preparingInstallation else { return false }
            self.setDownloadedStatus()
            // Resume Sparkle's downloaded update into its consent UI, not its
            // immediate-install handler. Authorization still belongs to Sparkle.
            updater.checkForUpdates()
            return true
        })

    var updater: SPUUpdater? { controller?.updater }
    var checkMenuTitle: String {
        if let readyVersion { return "安裝 \(readyVersion) 並重新啟動…" }
        return availableVersion.map { "更新至 \($0)…" } ?? "檢查更新…"
    }

    func start() {
        guard controller == nil,
              Bundle.main.object(forInfoDictionaryKey: "LlavonIMEUpdatesEnabled") as? Bool == true else { return }
        // The pkg always installs the system bundle. Never let a developer's
        // user-level copy silently convert into a system-wide installation.
        guard Bundle.main.bundleURL.standardizedFileURL.path == "/Library/Input Methods/LlavonIME.app" else {
            setStatus("此安裝位置不支援套件更新，請安裝正式版 .pkg")
            return
        }
        let controller = SPUStandardUpdaterController(startingUpdater: false,
                                                      updaterDelegate: self,
                                                      userDriverDelegate: self)
        do {
            try controller.updater.start()
            self.controller = controller
            setStatus("由拉風輸入法管理更新；安裝需要管理員授權")
        } catch {
            setStatus("無法啟動更新：\(error.localizedDescription)")
        }
    }

    @objc func checkForUpdates(_ sender: Any?) {
        guard let updater else {
            showSettings(sender)
            return
        }
        guard updater.canCheckForUpdates else { return }
        if let readyVersion { readyReminder.userAcknowledged(version: readyVersion) }
        else { readyReminder.cancel() }
        setStatus(readyVersion == nil ? "正在檢查更新…" : "正在開啟安裝與重新啟動提示…")
        updater.checkForUpdates()
    }

    @objc func showSettings(_ sender: Any?) {
        if !EngineBridge.shared.openSettingsApp(page: "updates") {
            let alert = NSAlert(); alert.messageText = "找不到拉風設定與個人化程式"; alert.runModal()
        }
    }

    // The unified app reads live Sparkle state and changes the same persisted
    // preferences used by the previous update window. It never copies defaults
    // to a different bundle/domain.
    func settingsRequest(_ request: [String: Any]) -> [String: Any] {
        switch request["action"] as? String {
        case "status": break
        case "preferences":
            guard let updater else { return ["error": status] }
            if let checks = request["checks"] as? Bool { updater.automaticallyChecksForUpdates = checks }
            if let downloads = request["downloads"] as? Bool {
                guard updater.allowsAutomaticUpdates else { return ["error": "目前更新來源不支援背景下載"] }
                updater.automaticallyDownloadsUpdates = downloads
            }
        case "check":
            guard let updater, updater.canCheckForUpdates else { return ["error": status] }
            checkForUpdates(nil)
        default: return ["error": "不支援的設定要求"]
        }
        return ["version": Bundle.main.object(forInfoDictionaryKey: "LlavonIMEDisplayVersion") as? String ?? "開發版本",
                "context": "InputMethodKit：由應用程式提供游標周圍文字，不需輔助使用權限。",
                "status": status, "enabled": updater != nil, "checks": updater?.automaticallyChecksForUpdates ?? false,
                "downloads": updater?.automaticallyDownloadsUpdates ?? false,
                "allowsDownloads": updater?.allowsAutomaticUpdates ?? false, "canCheck": updater?.canCheckForUpdates ?? false,
                "readyToInstall": readyVersion != nil]
    }
    // Sparkle's package installer is allowed to proceed only after the app is
    // idle. Also used by applicationShouldTerminate for a last-moment check.
    func waitUntilIdle(_ resume: @escaping () -> Void) {
        installationGate.waitUntilIdle(resume)
    }

    func updater(_ updater: SPUUpdater, shouldPostponeRelaunchForUpdate item: SUAppcastItem,
                 untilInvokingBlock installHandler: @escaping () -> Void) -> Bool {
        preparingInstallation = true
        readyReminder.cancel()
        waitUntilIdle(installHandler)
        return true
    }

    func updater(_ updater: SPUUpdater, didFindValidUpdate item: SUAppcastItem) {
        availableVersion = item.displayVersionString
        if readyVersion == item.displayVersionString { setDownloadedStatus() }
        else { setStatus("有新版本 \(item.displayVersionString)") }
    }

    func updater(_ updater: SPUUpdater, didDownloadUpdate item: SUAppcastItem) {
        readyVersion = item.displayVersionString
        setStatus("\(item.displayVersionString) 已下載，正在準備安裝提示…")
    }

    func updater(_ updater: SPUUpdater, willInstallUpdateOnQuit item: SUAppcastItem,
                 immediateInstallationBlock: @escaping () -> Void) -> Bool {
        readyVersion = item.displayVersionString
        readyOnQuit = true
        setDownloadedStatus()
        // An input method rarely quits. Once the background cycle completes,
        // show the ready-to-install UI instead of waiting indefinitely for quit.
        return false
    }

    func updater(_ updater: SPUUpdater, didFinishUpdateCycleFor updateCheck: SPUUpdateCheck, error: Error?) {
        if error == nil, readyOnQuit, let readyVersion {
            readyOnQuit = false
            readyReminder.ready(version: readyVersion)
        }
    }

    func updaterDidNotFindUpdate(_ updater: SPUUpdater) {
        availableVersion = nil
        readyVersion = nil
        readyOnQuit = false
        readyReminder.cancel()
        setStatus("目前沒有可安裝的新版")
    }

    func updater(_ updater: SPUUpdater, didAbortWithError error: Error) {
        preparingInstallation = false
        readyVersion = nil
        readyOnQuit = false
        readyReminder.cancel()
        installationGate.cancel()
        let code = (error as NSError).code
        if code == SUError.noUpdateError.rawValue {
            setStatus("目前沒有可安裝的新版")
        } else if code == SUError.installationCanceledError.rawValue {
            setStatus("已取消安裝，可稍後再次檢查更新")
        } else {
            setStatus("更新未完成：\(error.localizedDescription)")
        }
    }

    // Discovery stays quiet. Only downloaded/prepared updates automatically
    // open the install UI, after composition and personalization work are idle.
    nonisolated var supportsGentleScheduledUpdateReminders: Bool { true }

    nonisolated func standardUserDriverShouldHandleShowingScheduledUpdate(_ update: SUAppcastItem,
                                                              andInImmediateFocus immediateFocus: Bool) -> Bool {
        false
    }

    nonisolated func standardUserDriverWillHandleShowingUpdate(_ handleShowingUpdate: Bool,
                                                   forUpdate update: SUAppcastItem,
                                                   state: SPUUserUpdateState) {
        MainActor.assumeIsolated {
            guard !state.userInitiated else { return }
            if state.stage == .downloaded || state.stage == .installing {
                readyVersion = update.displayVersionString
                setDownloadedStatus()
                if !handleShowingUpdate { readyReminder.ready(version: update.displayVersionString) }
            } else {
                setStatus("有新版本 \(update.displayVersionString)；請按「檢查更新…」開啟更新視窗。")
            }
        }
    }

    nonisolated func standardUserDriverDidReceiveUserAttention(forUpdate update: SUAppcastItem) {
        MainActor.assumeIsolated { readyReminder.userAcknowledged(version: update.displayVersionString) }
    }

    nonisolated func standardUserDriverWillFinishUpdateSession() {
        MainActor.assumeIsolated { readyReminder.cancel() }
    }

    private func setDownloadedStatus() {
        guard let readyVersion else { return }
        setStatus("\(readyVersion) 已下載，可安裝；請在安裝提示中確認，或按「安裝並重新啟動…」。")
    }

    private func setStatus(_ message: String) {
        status = message
    }

    private static func probeTraining(_ completion: @escaping (Bool) -> Void) {
        DispatchQueue.global(qos: .utility).async {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
            // The CLI owns trainer/export/download jobs; a running manager
            // window alone is not a reason to block an update indefinitely.
            process.arguments = ["-x", "llavon-lora|llavon-ime-lora"]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            let busy: Bool
            do {
                try process.run()
                process.waitUntilExit()
                busy = process.terminationStatus != 1
            } catch { busy = true }
            DispatchQueue.main.async { completion(busy) }
        }
    }
}

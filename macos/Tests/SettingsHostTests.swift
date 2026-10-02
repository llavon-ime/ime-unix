import Foundation
import CoreFoundation

@main struct SettingsHostTests {
    @MainActor static func main() throws {
        setbuf(stdout, nil)
        var checks = true, downloads = false, checked = false
        let host = SettingsHost { request in
            switch request["action"] as? String {
            case "status": break
            case "preferences":
                if let value = request["checks"] as? Bool { checks = value }
                if let value = request["downloads"] as? Bool { downloads = value }
            case "check": checked = true
            default: return ["error": "不支援的設定要求"]
            }
            let readyFile = ProcessInfo.processInfo.environment["LLAVON_TEST_READY_FILE"]
            let ready = checked || readyFile.map { FileManager.default.fileExists(atPath: $0) } == true
            return ["version": "migration-test", "context": "InputMethodKit 測試狀態", "enabled": true,
                    "checks": checks, "downloads": downloads, "allowsDownloads": true, "canCheck": true,
                    "readyToInstall": ready,
                    "status": ready ? "1.0.1 已下載，可安裝；請在安裝提示中確認，或按「安裝並重新啟動…」。" : "既有更新偏好已載入"]
        }
        precondition(host.start())
        defer { host.stop() }
        if CommandLine.arguments.contains("--serve") {
            print("READY"); RunLoop.main.run(); return
        }
        guard let helper = ProcessInfo.processInfo.environment["LLAVON_TEST_HOST_HELPER"] else { fatalError("Set LLAVON_TEST_HOST_HELPER") }
        func call(_ request: [String: Any]) throws -> [String: Any] {
            let process = Process(); process.executableURL = URL(fileURLWithPath: helper)
            process.arguments = ["--host-request", String(data: try JSONSerialization.data(withJSONObject: request), encoding: .utf8)!]
            let pipe = Pipe(); process.standardOutput = pipe
            try process.run()
            let deadline = Date().addingTimeInterval(5)
            while process.isRunning && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
            precondition(!process.isRunning && process.terminationStatus == 0)
            return try JSONSerialization.jsonObject(with: pipe.fileHandleForReading.readDataToEndOfFile()) as! [String: Any]
        }
        let initial = try call(["action": "status"]); precondition(initial["checks"] as? Bool == true)
        precondition(initial["readyToInstall"] as? Bool == false)
        let changedChecks = try call(["action": "preferences", "checks": false]); precondition(changedChecks["checks"] as? Bool == false)
        let changedDownloads = try call(["action": "preferences", "downloads": true]); precondition(changedDownloads["downloads"] as? Bool == true)
        let refreshed = try call(["action": "status"]); precondition(refreshed["downloads"] as? Bool == true)
        let checkedState = try call(["action": "check"]); precondition((checkedState["status"] as? String)?.contains("已下載") == true)
        precondition(checkedState["readyToInstall"] as? Bool == true)
        let rejected = try call(["action": "write-config"]); precondition(rejected["error"] != nil)
        host.stop()
        let disconnected = try call(["action": "status"]); precondition(disconnected["error"] != nil)
        print("PASS live status, update preferences, check command, unknown action and disconnected host")
    }
}

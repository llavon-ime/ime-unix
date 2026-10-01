import Foundation

// Runtime JSON only. The C++ engine validates settings; the shared app owns
// all form metadata and persistence. No duplicate Swift settings/schema layer.
enum ConfigJSON {
    static func object(_ json: String?) -> [String: Any]? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    static func fillingModelPath(_ json: String?, with path: String?) -> String? {
        guard let path, !path.isEmpty else { return json }
        var object = object(json) ?? [:]
        if let existing = object["model_path"] as? String, !existing.isEmpty { return json }
        object["model_path"] = path
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return json }
        return String(data: data, encoding: .utf8)
    }
}

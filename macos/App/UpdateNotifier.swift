import Foundation
import UserNotifications

private let updateNotificationIdentifier = "org.llavon-ime.update-ready"
private let updateNotificationCategory = "org.llavon-ime.update-ready"
private let updateInstallAction = "install"

// A downloaded update is announced with a system notification, which never
// takes keyboard focus from the app the user is typing in. Only a click opens
// Sparkle's consent window. When notifications are denied or unavailable (for
// example for an ad-hoc signed build), delivery reports failure and the caller
// falls back to opening the window.
@MainActor
final class UpdateNotifier: NSObject, UNUserNotificationCenterDelegate {
    private(set) var available = true
    private let onOpen: () -> Void
    private let onDelivery: (String, Bool) -> Void

    init(onOpen: @escaping () -> Void, onDelivery: @escaping (String, Bool) -> Void) {
        self.onOpen = onOpen
        self.onDelivery = onDelivery
        super.init()
    }

    func start() {
        let center = UNUserNotificationCenter.current()
        center.delegate = self
        let install = UNNotificationAction(identifier: updateInstallAction, title: "安裝並重新啟動…",
                                           options: [.foreground])
        center.setNotificationCategories([
            UNNotificationCategory(identifier: updateNotificationCategory, actions: [install],
                                   intentIdentifiers: [], options: []),
        ])
    }

    func post(version: String) {
        Self.deliver(version: version) { success in
            Task { @MainActor in self.delivered(version: version, success: success) }
        }
    }

    // Notification callbacks arrive on a background queue; nothing here
    // touches main-actor state.
    nonisolated private static func deliver(version: String, completion: @escaping @Sendable (Bool) -> Void) {
        // The first post asks for permission. macOS shows that request as a
        // notification too, so it does not take focus either.
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert]) { granted, error in
            guard granted, error == nil else {
                completion(false)
                return
            }
            let content = UNMutableNotificationContent()
            content.title = "拉風輸入法 \(version) 已下載"
            content.body = "按「安裝並重新啟動…」確認安裝；安裝需要管理員授權。"
            content.categoryIdentifier = updateNotificationCategory
            let request = UNNotificationRequest(identifier: updateNotificationIdentifier,
                                                content: content, trigger: nil)
            UNUserNotificationCenter.current().add(request) { error in completion(error == nil) }
        }
    }

    func remove() {
        let center = UNUserNotificationCenter.current()
        center.removePendingNotificationRequests(withIdentifiers: [updateNotificationIdentifier])
        center.removeDeliveredNotifications(withIdentifiers: [updateNotificationIdentifier])
    }

    private func delivered(version: String, success: Bool) {
        // Stop asking for this process once the system refuses; the next
        // launch checks again in case the user enabled notifications.
        if !success { available = false }
        onDelivery(version, success)
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler:
                                                @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list])
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            didReceive response: UNNotificationResponse,
                                            withCompletionHandler completionHandler: @escaping () -> Void) {
        let action = response.actionIdentifier
        if response.notification.request.identifier == updateNotificationIdentifier,
           action == UNNotificationDefaultActionIdentifier || action == updateInstallAction {
            Task { @MainActor in self.onOpen() }
        }
        completionHandler()
    }
}

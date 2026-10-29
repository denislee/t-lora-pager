import AudioToolbox
import Foundation
import PagerKit
import UIKit
import UserNotifications

/// The Inbox tab's state (`IOS_COMPANION_PLAN.md` §6.1 "Inbox", PROTOCOL.md
/// §6.5): what the pager sent the phone — `clip.push` text and `phone.find`
/// requests — plus the share extension's outbox in the App Group.
///
/// Pager → phone `clip.push`: in the foreground the text goes on the
/// pasteboard at once with an in-app banner; in the background a local
/// notification is posted, and tapping it copies the text
/// (`NotificationRouter`). `phone.find`: a time-sensitive local notification
/// with sound (rings a locked phone), and in the foreground an alert sound.
/// The phone → pager calls themselves go through `PagerSession`.
@MainActor
final class InboxModel: ObservableObject {
    /// One thing the pager sent.
    struct Received: Identifiable, Equatable {
        enum Kind { case clip, find }
        let id = UUID()
        let kind: Kind
        let text: String?
        let at: Date
    }

    @Published private(set) var received: [Received] = []
    /// The in-app banner text ("Copied from the pager"), cleared after a few seconds.
    @Published var banner: String?
    /// Messages the share extension queued and the app has not sent yet.
    @Published private(set) var outboxCount = 0

    /// The App Group outbox, nil without the App Group (unsigned builds).
    let outbox: PagerOutboxStore?
    /// Session-log sink (kinds and sizes, never the text).
    var log: (String) -> Void = { _ in }

    static let maxReceived = 100

    init() {
        let group = PagerStatusStore.appGroupID(bundleID: Bundle.main.bundleIdentifier ?? "")
        outbox = UserDefaults(suiteName: group).map(PagerOutboxStore.init(defaults:))
        outboxChanged()
    }

    func outboxChanged() {
        outboxCount = outbox?.pending().count ?? 0
    }

    // MARK: - From the pager

    func pagerClip(_ text: String) {
        record(Received(kind: .clip, text: text, at: Date()))
        log("clip.push from pager: \(text.utf8.count) bytes")
        if UIApplication.shared.applicationState == .active {
            UIPasteboard.general.string = text
            show("Copied from the pager")
        } else {
            NotificationRouter.post(
                id: "clip-\(UUID().uuidString)", title: "Pager clipboard", body: Self.preview(text),
                userInfo: [NotificationRouter.clipKey: text], timeSensitive: false)
        }
    }

    func pagerFind() {
        record(Received(kind: .find, text: nil, at: Date()))
        log("phone.find from pager")
        NotificationRouter.post(id: "find-\(UUID().uuidString)", title: "Your pager is looking for this phone",
                                body: "Find phone was pressed on the pager.", userInfo: [:], timeSensitive: true)
        if UIApplication.shared.applicationState == .active {
            show("The pager is looking for this phone")
            Task {
                for _ in 0..<3 {
                    AudioServicesPlayAlertSound(SystemSoundID(1005))
                    try? await Task.sleep(for: .seconds(1.5))
                }
            }
        }
    }

    /// Put a received clip on the pasteboard again.
    func copy(_ item: Received) {
        guard let text = item.text else { return }
        UIPasteboard.general.string = text
        show("Copied")
    }

    func clear() {
        received = []
    }

    private func record(_ item: Received) {
        received.insert(item, at: 0)
        if received.count > Self.maxReceived { received.removeLast(received.count - Self.maxReceived) }
    }

    private func show(_ text: String) {
        banner = text
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            if self?.banner == text { self?.banner = nil }
        }
    }

    /// The first 160 characters on one line, for a notification body.
    nonisolated static func preview(_ text: String) -> String {
        let line = text.replacingOccurrences(of: "\n", with: " ").replacingOccurrences(of: "\t", with: " ")
        return line.count > 160 ? String(line.prefix(160)) + "…" : line
    }
}

/// Local notifications for the pager's `clip.push` and `phone.find`: asks
/// for permission once, shows them in the foreground too, and copies a
/// clip's text when its notification is tapped. Installed as the
/// `UNUserNotificationCenter` delegate at launch (`PagerCompanionApp`).
final class NotificationRouter: NSObject, UNUserNotificationCenterDelegate, @unchecked Sendable {
    static let shared = NotificationRouter()
    static let clipKey = "pagerClip"

    /// Become the delegate and ask for alert + sound permission (once; iOS
    /// remembers the answer). Time-sensitive delivery needs the entitlement
    /// in `Config/PagerCompanion.entitlements`, not a separate prompt.
    @MainActor
    static func install() {
        let center = UNUserNotificationCenter.current()
        center.delegate = shared
        center.requestAuthorization(options: [.alert, .sound, .badge]) { _, _ in }
    }

    static func post(id: String, title: String, body: String, userInfo: [String: String], timeSensitive: Bool) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        content.userInfo = userInfo
        if timeSensitive { content.interruptionLevel = .timeSensitive }
        let request = UNNotificationRequest(identifier: id, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { _ in }
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .sound, .list])
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                            withCompletionHandler completionHandler: @escaping () -> Void) {
        if let text = response.notification.request.content.userInfo[Self.clipKey] as? String {
            Task { @MainActor in UIPasteboard.general.string = text }
        }
        completionHandler()
    }
}

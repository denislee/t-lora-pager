import AppIntents
import Foundation
import PagerKit

/// How App Intents reach the running app's link: the one `PagerSession`
/// sets itself here in `init`. Intents run in the app's process (they are
/// defined in the app target), so this is the live session, never a copy.
@MainActor
enum AppBridge {
    static weak var session: PagerSession?

    /// The session when a pager is connected, else an error Shortcuts shows.
    static func connected() throws -> PagerSession {
        guard let session, case .connected = session.state else { throw PagerIntentError.notConnected }
        return session
    }
}

/// What an intent says when it can't run.
enum PagerIntentError: Error, CustomLocalizedStringResourceConvertible {
    case notConnected
    case unsupported(String)
    case failed(String)

    var localizedStringResource: LocalizedStringResource {
        switch self {
        case .notConnected: return "No pager is connected. Open Pager Companion and connect first."
        case .unsupported(let what): return "This pager's firmware can't \(what)."
        case .failed(let why): return "\(why)"
        }
    }
}

/// S17: "Send to pager" — a notify.post with the given text.
struct SendToPagerIntent: AppIntent {
    static let title: LocalizedStringResource = "Send to Pager"
    static let description = IntentDescription("Shows a message on the pager and keeps it in its inbox.")

    @Parameter(title: "Text")
    var text: String

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let session = try AppBridge.connected()
        guard session.has("notify") else { throw PagerIntentError.unsupported("receive messages") }
        let message = NotifyMessage.shared(text: text, url: nil, src: "Shortcuts", id: NotifyMessage.newID(prefix: "sc"),
                                           ts: UInt32(clamping: Int(Date().timeIntervalSince1970)))
        do {
            _ = try await session.notifyPost(message)
        } catch {
            throw PagerIntentError.failed(NotesModel.describe(error))
        }
        return .result(dialog: "Sent to the pager.")
    }
}

/// S17: "Push weather" — fetch the forecast for the pager's location and push it.
struct PushWeatherIntent: AppIntent {
    static let title: LocalizedStringResource = "Push Weather to Pager"
    static let description = IntentDescription("Fetches the forecast for the pager's location and sends it.")

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let session = try AppBridge.connected()
        if let problem = await session.pushWeather() { throw PagerIntentError.failed(problem) }
        return .result(dialog: "Weather pushed to the pager.")
    }
}

/// S17: "Sync notes" — the Notes tab's move to GitHub.
struct SyncNotesIntent: AppIntent {
    static let title: LocalizedStringResource = "Sync Pager Notes to GitHub"
    static let description = IntentDescription(
        "Moves the pager's notes to the GitHub repository set in the Notes tab.")

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let session = try AppBridge.connected()
        let notes = session.notes
        guard notes.account.isConnected else { throw PagerIntentError.failed("Set up GitHub in the Notes tab first.") }
        guard notes.canSync else { throw PagerIntentError.failed("A sync is already running, or the notes aren't loaded yet.") }
        notes.startSync()
        return .result(dialog: "Notes sync started.")
    }
}

/// S17: "Find pager" — buzz the pager for 30 s.
struct FindPagerIntent: AppIntent {
    static let title: LocalizedStringResource = "Find My Pager"
    static let description = IntentDescription("Wakes the pager's screen and buzzes it for 30 seconds.")

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let session = try AppBridge.connected()
        guard session.has("find") else { throw PagerIntentError.unsupported("be found from the phone") }
        do {
            try await session.findPager()
        } catch {
            throw PagerIntentError.failed(NotesModel.describe(error))
        }
        return .result(dialog: "The pager is buzzing.")
    }
}

/// Siri / Shortcuts phrases. A `String` parameter can't be part of a phrase,
/// so "Send to pager" asks for the text.
struct PagerShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: SendToPagerIntent(), phrases: ["Send to pager with \(.applicationName)",
                                                           "Message my pager with \(.applicationName)"])
        AppShortcut(intent: PushWeatherIntent(), phrases: ["Push weather to my pager with \(.applicationName)"])
        AppShortcut(intent: SyncNotesIntent(), phrases: ["Sync pager notes with \(.applicationName)"])
        AppShortcut(intent: FindPagerIntent(), phrases: ["Find my pager with \(.applicationName)"])
    }
}

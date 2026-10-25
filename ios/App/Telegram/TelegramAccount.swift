import Foundation
import os
import PagerKit

/// The phone's own Telegram login (TDLib), which answers the pager's `tg.*`
/// with no tg-bridge (`TelegramService(native:)`).
///
/// Needs an `api_id` / `api_hash` from my.telegram.org (kept in
/// UserDefaults). The session lives in Application Support/tdlib and survives
/// relaunches; `logOut()` ends it on Telegram's side too.
@MainActor
final class TelegramAccount: ObservableObject {
    enum Phase {
        case needsKeys
        case starting
        case waitPhone
        case waitCode
        case waitPassword(hint: String)
        case ready(name: String)
        case unsupported(String)
    }

    @Published private(set) var phase: Phase = .needsKeys
    @Published private(set) var lastError: String?
    @Published private(set) var busy = false

    /// What `TelegramService` calls; ready only while logged in.
    let native: TelegramNative

    private let readyFlag = OSAllocatedUnfairLock(initialState: false)
    private let client: TDJSONClient

    private static let apiIDKey = "Telegram.apiID"
    private static let apiHashKey = "Telegram.apiHash"

    var apiID: String { UserDefaults.standard.string(forKey: Self.apiIDKey) ?? "" }
    var apiHash: String { UserDefaults.standard.string(forKey: Self.apiHashKey) ?? "" }

    init() {
        let flag = readyFlag
        let relay = Relay()
        let client = TDJSONClient { update in
            Task { @MainActor in relay.account?.handle(update) }
        }
        self.client = client
        native = TDTelegram(td: client) { flag.withLock { $0 } }
        relay.account = self
        if !apiID.isEmpty, !apiHash.isEmpty { phase = .starting }
    }

    /// Hands TDLib updates (from its thread) to the account once it exists.
    @MainActor private final class Relay {
        weak var account: TelegramAccount?
    }

    // MARK: - Actions

    func saveKeys(apiID: String, apiHash: String) {
        let id = apiID.trimmingCharacters(in: .whitespaces)
        let hash = apiHash.trimmingCharacters(in: .whitespaces)
        guard Int32(id) != nil, !hash.isEmpty else {
            lastError = "api_id must be a number and api_hash non-empty"
            return
        }
        UserDefaults.standard.set(id, forKey: Self.apiIDKey)
        UserDefaults.standard.set(hash, forKey: Self.apiHashKey)
        lastError = nil
        phase = .starting
        Task { await sendParameters() }
    }

    func submitPhone(_ phone: String) {
        run(["@type": "setAuthenticationPhoneNumber", "phone_number": .string(phone), "settings": nil])
    }

    func submitCode(_ code: String) {
        run(["@type": "checkAuthenticationCode", "code": .string(code)])
    }

    func submitPassword(_ password: String) {
        run(["@type": "checkAuthenticationPassword", "password": .string(password)])
    }

    func logOut() {
        run(["@type": "logOut"])
    }

    private func run(_ request: JSONValue) {
        busy = true
        lastError = nil
        Task {
            do {
                _ = try await client.query(request)
            } catch {
                lastError = Self.describe(error)
            }
            busy = false
        }
    }

    // MARK: - TDLib

    private func handle(_ update: JSONValue) {
        guard update["@type"]?.stringValue == "updateAuthorizationState",
              let state = update["authorization_state"] else { return }
        setReady(false)
        switch state["@type"]?.stringValue ?? "" {
        case "authorizationStateWaitTdlibParameters":
            if apiID.isEmpty || apiHash.isEmpty {
                phase = .needsKeys
            } else {
                phase = .starting
                Task { await sendParameters() }
            }
        case "authorizationStateWaitPhoneNumber":
            phase = .waitPhone
        case "authorizationStateWaitCode":
            phase = .waitCode
        case "authorizationStateWaitPassword":
            phase = .waitPassword(hint: state["password_hint"]?.stringValue ?? "")
        case "authorizationStateReady":
            setReady(true)
            phase = .ready(name: "")
            Task { await loadMe() }
        case "authorizationStateLoggingOut", "authorizationStateClosing":
            phase = .starting
        case "authorizationStateClosed":
            phase = .starting
            client.reset()
        case let other:
            let name = other.replacingOccurrences(of: "authorizationState", with: "")
            phase = .unsupported("Telegram asks for \(name), which this app can't do yet. "
                + "Log in once in the official app, then retry.")
        }
    }

    private func setReady(_ ready: Bool) {
        readyFlag.withLock { $0 = ready }
    }

    private func sendParameters() async {
        guard let id = Int32(apiID), !apiHash.isEmpty else {
            phase = .needsKeys
            return
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        let request: JSONValue = [
            "@type": "setTdlibParameters",
            "use_test_dc": false,
            "database_directory": .string(support.appendingPathComponent("tdlib", isDirectory: true).path),
            "files_directory": .string(caches.appendingPathComponent("tdlib-files", isDirectory: true).path),
            "database_encryption_key": "",
            "use_file_database": false,
            "use_chat_info_database": true,
            "use_message_database": true,
            "use_secret_chats": false,
            "api_id": .number(Double(id)),
            "api_hash": .string(apiHash),
            "system_language_code": .string(Locale.current.language.languageCode?.identifier ?? "en"),
            "device_model": "LilyGo Pager Companion",
            "system_version": .string(ProcessInfo.processInfo.operatingSystemVersionString),
            "application_version": "0.1",
        ]
        do {
            _ = try await client.query(request)
        } catch {
            lastError = Self.describe(error)
        }
    }

    /// TDLib's own message for a `TDError`, else the error as Swift prints it.
    nonisolated private static func describe(_ error: Error) -> String {
        (error as? TDError)?.message ?? String(describing: error)
    }

    private func loadMe() async {
        guard let me = try? await client.query(["@type": "getMe"]) else { return }
        if case .ready = phase { phase = .ready(name: TDTelegram.displayName(me)) }
    }
}

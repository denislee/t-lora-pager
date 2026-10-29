import Foundation
import PagerKit

/// The Chat tab's state: every `chat.ask` turn the phone answered for the
/// pager (persisted by PagerKit's `ChatHistoryStore`), and the Groq API key
/// `ChatService` and the voice-memo import use (Keychain, this device only;
/// never sent to the pager, never logged).
///
/// Owned by `PagerSession`, which registers `ChatService` on every link and
/// feeds its records here.
@MainActor
final class ChatHistoryModel: ObservableObject {
    @Published private(set) var records: [ChatRecord] = []
    @Published private(set) var hasGroqKey = false

    private let store: ChatHistoryStore
    private let secrets: KeychainSecretStore
    nonisolated private static let keyName = "groq.api_key"

    init(store: ChatHistoryStore = ChatHistoryStore(),
         secrets: KeychainSecretStore = KeychainSecretStore(service: "PagerCompanion.groq")) {
        self.store = store
        self.secrets = secrets
        records = store.all
        hasGroqKey = !(secrets.read(Self.keyName) ?? "").isEmpty
    }

    /// The key as `GroqChatBackend` asks for it on every call (any thread).
    nonisolated var groqKey: @Sendable () -> String? {
        let secrets = self.secrets
        return { secrets.read(ChatHistoryModel.keyName) }
    }

    func setGroqKey(_ key: String) throws {
        let trimmed = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            secrets.delete(Self.keyName)
        } else {
            try secrets.write(Self.keyName, trimmed)
        }
        hasGroqKey = !trimmed.isEmpty
    }

    /// One record from `ChatService.onRecord`.
    func append(_ record: ChatRecord) {
        store.append(record)
        records = store.all
    }

    /// Records matching `query` (all for a blank one), newest first.
    func search(_ query: String) -> [ChatRecord] {
        store.search(query).reversed()
    }

    func clear() {
        store.clear()
        records = []
    }

    /// Write the history as a text file for the share sheet.
    func exportFile(_ records: [ChatRecord]) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("pager-chat-history.txt")
        try Data(ChatHistoryStore.exportText(records).utf8).write(to: url, options: .atomic)
        return url
    }
}

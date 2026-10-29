import Foundation

/// One entry of the phone's chat history: an answered `chat.ask` turn, a
/// `reset` (the pager started a new conversation) or a failed ask.
public struct ChatRecord: Codable, Sendable, Equatable, Identifiable {
    public enum Kind: String, Codable, Sendable {
        case turn
        case reset
        case error
    }

    public var id: UUID
    public var at: Date
    public var kind: Kind
    /// What the pager asked: its text, or the Whisper transcript of a voice
    /// memo (`voice`). Nil for a reset or an ask that failed before it had one.
    public var prompt: String?
    /// The prompt is a transcript of the pager's voice memo.
    public var voice: Bool
    public var reply: String?
    /// The error the pager was sent (`stt: …`, `llm: …`, …).
    public var error: String?

    public init(id: UUID = UUID(), at: Date = Date(), kind: Kind, prompt: String? = nil,
                voice: Bool = false, reply: String? = nil, error: String? = nil) {
        self.id = id
        self.at = at
        self.kind = kind
        self.prompt = prompt
        self.voice = voice
        self.reply = reply
        self.error = error
    }
}

/// The phone's copy of every chat the pager has had through it (the pager
/// keeps only the last few turns): persisted as one JSON file, newest last,
/// capped at `capacity` records (the oldest go first). Searchable and
/// exportable as text or JSON. Thread-safe.
public final class ChatHistoryStore: @unchecked Sendable {
    /// Records kept; at a few hundred bytes each the file stays well under 2 MB.
    public static let defaultCapacity = 5000

    private let lock = NSLock()
    private let file: JSONFile<[ChatRecord]>
    private let capacity: Int
    private var records: [ChatRecord]

    /// `Application Support/chat-history.json` in the app container.
    public static func defaultURL() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return base.appendingPathComponent("chat-history.json")
    }

    public init(url: URL = ChatHistoryStore.defaultURL(), capacity: Int = ChatHistoryStore.defaultCapacity) {
        self.file = JSONFile(url: url)
        self.capacity = max(1, capacity)
        self.records = file.load() ?? []
    }

    /// Every record, oldest first.
    public var all: [ChatRecord] { lock.withLock { records } }

    /// Add one record and persist. A write failure keeps it in memory.
    public func append(_ record: ChatRecord) {
        lock.withLock {
            records.append(record)
            if records.count > capacity { records.removeFirst(records.count - capacity) }
            try? file.save(records)
        }
    }

    /// Forget everything (and delete the file).
    public func clear() {
        lock.withLock {
            records.removeAll()
            file.remove()
        }
    }

    /// Records whose prompt, reply or error contains `query` (case- and
    /// diacritic-insensitive), oldest first; every record for a blank query.
    public func search(_ query: String) -> [ChatRecord] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let all = self.all
        guard !needle.isEmpty else { return all }
        return all.filter { record in
            [record.prompt, record.reply, record.error].contains { text in
                guard let text else { return false }
                return text.range(of: needle, options: [.caseInsensitive, .diacriticInsensitive]) != nil
            }
        }
    }

    /// The history split into conversations at each reset (a reset starts
    /// the next one); empty conversations are dropped. Oldest first.
    public static func conversations(_ records: [ChatRecord]) -> [[ChatRecord]] {
        var out: [[ChatRecord]] = []
        var current: [ChatRecord] = []
        for record in records {
            if record.kind == .reset {
                if !current.isEmpty { out.append(current) }
                current = []
            } else {
                current.append(record)
            }
        }
        if !current.isEmpty { out.append(current) }
        return out
    }

    /// Plain-text export: one block per record, `--- new conversation ---`
    /// at each reset. Times are ISO 8601 in UTC so exports compare across zones.
    public static func exportText(_ records: [ChatRecord]) -> String {
        var lines: [String] = []
        for record in records {
            let stamp = Self.iso(record.at)
            switch record.kind {
            case .reset:
                lines.append("--- new conversation (\(stamp)) ---")
            case .turn:
                lines.append("[\(stamp)] \(record.voice ? "Pager (voice)" : "Pager"): \(record.prompt ?? "")")
                lines.append("[\(stamp)] Assistant: \(record.reply ?? "")")
            case .error:
                if let prompt = record.prompt {
                    lines.append("[\(stamp)] \(record.voice ? "Pager (voice)" : "Pager"): \(prompt)")
                }
                lines.append("[\(stamp)] Error: \(record.error ?? "unknown")")
            }
            lines.append("")
        }
        return lines.joined(separator: "\n")
    }

    /// JSON export: the records as stored (dates as Unix seconds).
    public static func exportJSON(_ records: [ChatRecord]) throws -> Data {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        encoder.outputFormatting = [.sortedKeys, .prettyPrinted]
        return try encoder.encode(records)
    }

    static func iso(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }
}

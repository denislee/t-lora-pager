import Foundation

/// One `Codable` value persisted as a JSON file, written atomically. The
/// phone-side history stores (chat, battery) keep their whole state in one
/// of these: small files, read once at launch, rewritten on every change.
///
/// A missing or unreadable file loads as nil (a corrupt store starts empty
/// rather than failing the app). Not thread-safe on its own; the stores that
/// use it hold a lock around every call.
public struct JSONFile<Value: Codable>: Sendable {
    public let url: URL

    public init(url: URL) {
        self.url = url
    }

    /// The stored value, or nil when there is none or it does not decode.
    public func load() -> Value? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return try? decoder.decode(Value.self, from: data)
    }

    /// Write `value`, creating the parent directory when needed.
    public func save(_ value: Value) throws {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(value)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }

    /// Remove the file (no error when it is already gone).
    public func remove() {
        try? FileManager.default.removeItem(at: url)
    }
}

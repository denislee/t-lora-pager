import Foundation

/// One message waiting to go to the pager: queued by the share extension
/// (which has no PagerLink of its own) and sent by the app with `notify.post`.
/// The id is minted at enqueue time, so a send retried after a dropped link
/// is deduped by the pager (`dup`) rather than shown twice.
public struct OutboxMessage: Codable, Sendable, Equatable, Identifiable {
    public var id: String
    public var title: String
    public var body: String?
    public var src: String?
    public var prio: NotifyPriority?
    public var createdAt: Date

    public init(id: String = NotifyMessage.newID(prefix: "share"), title: String, body: String? = nil,
                src: String? = nil, prio: NotifyPriority? = nil, createdAt: Date = Date()) {
        self.id = id
        self.title = title
        self.body = body
        self.src = src
        self.prio = prio
        self.createdAt = createdAt
    }

    public init(_ message: NotifyMessage, createdAt: Date = Date()) {
        self.init(id: message.id ?? NotifyMessage.newID(prefix: "share"), title: message.title, body: message.body,
                  src: message.src, prio: message.prio, createdAt: createdAt)
    }

    /// The `notify.post` it becomes (`ts` = when it was queued).
    public var notify: NotifyMessage {
        NotifyMessage(title: title, body: body, id: id, src: src,
                      ts: UInt32(clamping: Int(createdAt.timeIntervalSince1970)), prio: prio)
    }
}

/// The App Group queue the share extension writes and the app drains:
/// a JSON array under one `UserDefaults` key, oldest first, at most
/// `capacity` messages (the oldest are dropped). Both processes read and
/// write it; a write in one is seen by the other's next read.
public struct PagerOutboxStore: @unchecked Sendable {
    public static let key = "PagerOutbox.messages"
    public static let capacity = 50

    private let defaults: UserDefaults

    public init(defaults: UserDefaults) {
        self.defaults = defaults
    }

    public func pending() -> [OutboxMessage] {
        guard let data = defaults.data(forKey: Self.key) else { return [] }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return (try? decoder.decode([OutboxMessage].self, from: data)) ?? []
    }

    public func enqueue(_ message: OutboxMessage) {
        var all = pending().filter { $0.id != message.id }
        all.append(message)
        if all.count > Self.capacity { all.removeFirst(all.count - Self.capacity) }
        save(all)
    }

    public func remove(ids: Set<String>) {
        guard !ids.isEmpty else { return }
        save(pending().filter { !ids.contains($0.id) })
    }

    private func save(_ messages: [OutboxMessage]) {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        if messages.isEmpty {
            defaults.removeObject(forKey: Self.key)
        } else if let data = try? encoder.encode(messages) {
            defaults.set(data, forKey: Self.key)
        }
    }
}

/// What one outbox flush did.
public struct OutboxFlushReport: Sendable, Equatable {
    public var sent = 0
    /// Already in the pager's inbox (an earlier send got through).
    public var duplicates = 0
    /// Refused by the pager (or by the rules) for good: dropped, with the msg.
    public var dropped: [String] = []
    /// The link failed: the rest stays queued for the next connect.
    public var stoppedBy: RPCError?

    public init() {}
}

public enum PagerOutbox {
    /// Send every queued message in order. A sent or duplicate message is
    /// removed; one the pager refuses (`invalid_argument`) is dropped so it
    /// cannot block the queue; any other error stops the flush and keeps
    /// the rest.
    public static func flush(_ store: PagerOutboxStore,
                             send: @Sendable (NotifyMessage) async throws -> NotifyResult) async -> OutboxFlushReport {
        var report = OutboxFlushReport()
        for message in store.pending() {
            do {
                let result = try await send(message.notify)
                if result.dup { report.duplicates += 1 } else { report.sent += 1 }
                store.remove(ids: [message.id])
            } catch let error as RPCError where error.code == "invalid_argument" {
                report.dropped.append(error.msg)
                store.remove(ids: [message.id])
            } catch let error as RPCError {
                report.stoppedBy = error
                break
            } catch {
                report.stoppedBy = RPCError(code: "internal", msg: "\(error)")
                break
            }
        }
        return report
    }
}

/// Convenience wrappers for the Phase-1 `sys.*` methods (PROTOCOL.md §6).
public struct SysService: Sendable {
    let session: LinkSession

    public init(session: LinkSession) {
        self.session = session
    }

    /// `sys.hello` — announce this client and its capability families.
    public func hello(app: String, caps: [String]) async throws -> JSONValue {
        try await session.call("sys.hello", params: [
            "proto": .number(Double(PagerLink.protoVersion)),
            "app": .string(app),
            "caps": .array(caps.map { .string($0) }),
        ])
    }

    /// `sys.info` — pager status snapshot.
    public func info() async throws -> JSONValue {
        try await session.call("sys.info")
    }

    /// `sys.ping` — echo the round-trip timestamp.
    public func ping(t: Int) async throws -> Int {
        let result = try await session.call("sys.ping", params: ["t": .number(Double(t))])
        return result["t"]?.intValue ?? t
    }

    /// `sys.time.set` — set the pager clock.
    public func setTime(epoch: Int, tz: String) async throws {
        _ = try await session.call("sys.time.set", params: [
            "epoch": .number(Double(epoch)),
            "tz": .string(tz),
        ])
    }
}

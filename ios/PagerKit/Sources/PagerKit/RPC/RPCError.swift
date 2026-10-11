/// The error carried by an ERR message, or delivered locally on timeout or
/// disconnect (PROTOCOL.md §3.4). `code` is one of the wire codes; `msg` is
/// free-form text.
public struct RPCError: Error, Equatable, Sendable {
    public let code: String
    public let msg: String

    public init(code: String, msg: String = "") {
        self.code = code
        self.msg = msg
    }

    /// The caller's request expired locally (PROTOCOL.md §3.3). Never on the wire.
    public static func timeout(_ msg: String = "request timed out") -> RPCError {
        RPCError(code: "timeout", msg: msg)
    }

    /// The link dropped while the request was outstanding. Never on the wire.
    public static func disconnected(_ msg: String = "link dropped") -> RPCError {
        RPCError(code: "disconnected", msg: msg)
    }
}

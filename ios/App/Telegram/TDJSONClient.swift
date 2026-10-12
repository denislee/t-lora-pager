import Foundation
import os
import PagerKit
import TDLibFramework

/// TDLib's JSON interface (`td_json_client.h`) as a `TDQuerying`.
///
/// One TDLib instance per client id. `td_receive` is process-wide, so a
/// single background thread drains it: a reply carrying our `@extra` resumes
/// its waiting `query`; everything else is an update for `onUpdate`. `reset()`
/// starts a fresh instance after `authorizationStateClosed` (log out).
final class TDJSONClient: TDQuerying, @unchecked Sendable {
    private struct State {
        var clientID: Int32 = 0
        var nextExtra: UInt64 = 1
        var pending: [String: CheckedContinuation<JSONValue, Error>] = [:]
    }

    private let state = OSAllocatedUnfairLock(initialState: State())
    private let onUpdate: @Sendable (JSONValue) -> Void

    init(onUpdate: @escaping @Sendable (JSONValue) -> Void) {
        self.onUpdate = onUpdate
        // Quiet TDLib's own log (it defaults to verbose stderr).
        _ = td_execute(#"{"@type":"setLogVerbosityLevel","new_verbosity_level":1}"#)
        state.withLock { $0.clientID = td_create_client_id() }
        let thread = Thread { [weak self] in self?.receiveLoop() }
        thread.name = "tdlib-receive"
        thread.qualityOfService = .utility
        thread.start()
        // TDLib only starts an instance once it receives its first request.
        send(["@type": "getOption", "name": "version"], extra: nil)
    }

    /// Drop the closed instance and start a new one (it begins at
    /// `authorizationStateWaitTdlibParameters`).
    func reset() {
        let waiters = state.withLock { s -> [CheckedContinuation<JSONValue, Error>] in
            s.clientID = td_create_client_id()
            defer { s.pending.removeAll() }
            return Array(s.pending.values)
        }
        for waiter in waiters { waiter.resume(throwing: TDError(code: 500, message: "client reset")) }
        send(["@type": "getOption", "name": "version"], extra: nil)
    }

    func query(_ request: JSONValue) async throws -> JSONValue {
        let extra = state.withLock { s -> String in
            defer { s.nextExtra += 1 }
            return "q\(s.nextExtra)"
        }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                state.withLock { $0.pending[extra] = continuation }
                send(request, extra: extra)
            }
        } onCancel: {
            // TDLib keeps waiting (e.g. offline); free the caller now.
            let waiter = state.withLock { $0.pending.removeValue(forKey: extra) }
            waiter?.resume(throwing: CancellationError())
        }
    }

    private func send(_ request: JSONValue, extra: String?) {
        var request = request
        if let extra, case .object(var object) = request {
            object["@extra"] = .string(extra)
            request = .object(object)
        }
        guard let bytes = try? request.encoded() else { return }
        let id = state.withLock { $0.clientID }
        String(decoding: bytes, as: UTF8.self).withCString { td_send(id, $0) }
    }

    private func receiveLoop() {
        while true {
            guard let raw = td_receive(1.0) else { continue }
            let text = String(cString: raw)
            guard let value = try? JSONValue.decode(bytes: Array(text.utf8)) else { continue }
            let current = state.withLock { $0.clientID }
            if let from = value["@client_id"]?.intValue, from != Int(current) { continue }
            if let extra = value["@extra"]?.stringValue {
                let waiter = state.withLock { $0.pending.removeValue(forKey: extra) }
                if value["@type"]?.stringValue == "error" {
                    waiter?.resume(throwing: TDError(code: value["code"]?.intValue ?? 0,
                                                     message: value["message"]?.stringValue ?? ""))
                } else {
                    waiter?.resume(returning: value)
                }
            } else {
                onUpdate(value)
            }
        }
    }
}

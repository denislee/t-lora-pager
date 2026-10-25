import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// A non-2xx upstream reply, for callers that need the status.
public struct HTTPStatusError: Error, Equatable, Sendable {
    public let status: Int
    public let body: String

    public init(status: Int, body: String) {
        self.status = status
        self.body = body
    }
}

/// One HTTP exchange's outcome: status plus the whole body.
struct HTTPReply: Sendable {
    let status: Int
    let body: Data
}

/// Shared HTTP plumbing for the phone-side services. Everything goes through an
/// injected `URLSession`, so tests swap in a `URLProtocol` stub. Uses the
/// completion-handler `dataTask` (bridged to async) because it behaves the same
/// on Darwin and on Linux `FoundationNetworking`.
enum HTTP {
    /// A request with what every service sets: method, timeout, an optional
    /// bearer token, and an optional JSON body with its Content-Type.
    static func request(url: URL, method: String = "GET", timeout: TimeInterval,
                        bearer: String? = nil, json: JSONValue? = nil) throws -> URLRequest {
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.timeoutInterval = timeout
        if let bearer { request.setValue("Bearer \(bearer)", forHTTPHeaderField: "Authorization") }
        if let json {
            request.httpBody = Data(try json.encoded())
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        return request
    }

    /// Send `request` once and return the status and body.
    static func send(_ request: URLRequest, using session: URLSession) async throws -> HTTPReply {
        try await withCheckedThrowingContinuation { continuation in
            let task = session.dataTask(with: request) { data, response, error in
                if let error {
                    continuation.resume(throwing: error)
                    return
                }
                guard let http = response as? HTTPURLResponse else {
                    continuation.resume(throwing: URLError(.badServerResponse))
                    return
                }
                continuation.resume(returning: HTTPReply(status: http.statusCode, body: data ?? Data()))
            }
            task.resume()
        }
    }

    /// Port of lilyhub `httpx.DoWithRetry`: up to `attempts` tries, retrying
    /// transport errors and 429/5xx, backing off `baseBackoff` doubling per
    /// retry (capped at 2 s). Returns the final reply without judging its
    /// status; throws only when no reply was obtained.
    ///
    /// With a `deadline`, every attempt gets only the time left (its
    /// `timeoutInterval` is capped to it) and no retry starts after it; a
    /// deadline already past throws the last error, or `URLError(.timedOut)`.
    static func sendWithRetry(
        _ request: URLRequest,
        using session: URLSession,
        attempts: Int,
        baseBackoff: Duration,
        deadline: ContinuousClock.Instant? = nil
    ) async throws -> HTTPReply {
        let attempts = max(1, attempts)
        var lastError: Error = URLError(.timedOut)
        for attempt in 1...attempts {
            if attempt > 1 {
                let delay = backoff(attempt: attempt, base: baseBackoff)
                if let deadline, ContinuousClock.now + delay >= deadline { throw lastError }
                try await Task.sleep(for: delay)
            }
            var request = request
            if let deadline {
                let left = deadline - ContinuousClock.now
                guard left > .zero else { throw lastError }
                request.timeoutInterval = min(request.timeoutInterval, left / .seconds(1))
            }
            do {
                let reply = try await send(request, using: session)
                if retryable(reply.status), attempt < attempts {
                    lastError = HTTPStatusError(status: reply.status, body: truncate(text(reply.body), 200))
                    continue
                }
                return reply
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                lastError = error
            }
        }
        throw lastError
    }

    static func retryable(_ status: Int) -> Bool {
        status == 429 || status >= 500
    }

    /// `httpx.backoff`: the first retry (attempt 2) waits `base`, then doubles,
    /// capped at 2 s.
    static func backoff(attempt: Int, base: Duration) -> Duration {
        let shift = max(attempt - 2, 0)
        var delay = base
        for _ in 0..<shift { delay = delay * 2 }
        return min(delay, .seconds(2))
    }

    /// `httpx.Truncate`: cut to at most `n` UTF-8 bytes, appending "..." when
    /// it had to cut.
    static func truncate(_ s: String, _ n: Int) -> String {
        guard s.utf8.count > n else { return s }
        return String(decoding: s.utf8.prefix(n), as: UTF8.self) + "..."
    }

    static func text(_ data: Data) -> String {
        String(decoding: data, as: UTF8.self)
    }
}

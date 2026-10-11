import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

// MARK: - StubRequest

/// A captured HTTP request, suitable for assertions in tests.
struct StubRequest: Sendable {
    let method: String
    let url: URL
    let headers: [String: String]
    let body: Data

    var path: String { url.path }
    var query: String? { url.query }

    /// Case-insensitive header lookup.
    func header(_ name: String) -> String? {
        let lower = name.lowercased()
        for (key, value) in headers where key.lowercased() == lower {
            return value
        }
        return nil
    }
}

// MARK: - StubResponse

/// A canned HTTP response served by `StubURLProtocol`.
struct StubResponse: Sendable {
    var status: Int
    var headers: [String: String] = [:]
    var body: Data = Data()

    init(status: Int, headers: [String: String] = [:], body: Data = Data()) {
        self.status = status
        self.headers = headers
        self.body = body
    }

    /// A JSON response whose body is `text` encoded as UTF-8.
    static func json(_ status: Int, _ text: String) -> StubResponse {
        StubResponse(
            status: status,
            headers: ["Content-Type": "application/json"],
            body: Data(text.utf8)
        )
    }
}

// MARK: - StubServer

/// Records every request it sees and answers it with a caller-supplied handler.
final class StubServer: @unchecked Sendable {
    private let lock = NSLock()
    private var _requests: [StubRequest] = []
    private let handler: @Sendable (StubRequest) throws -> StubResponse

    init(_ handler: @escaping @Sendable (StubRequest) throws -> StubResponse) {
        self.handler = handler
    }

    /// Every request seen so far, in order.
    var requests: [StubRequest] {
        lock.lock()
        defer { lock.unlock() }
        return _requests
    }

    /// Requests whose path ends with `suffix`.
    func requests(path suffix: String) -> [StubRequest] {
        requests.filter { $0.path.hasSuffix(suffix) }
    }

    /// Records the request, then delegates to the handler.
    func handle(_ r: StubRequest) throws -> StubResponse {
        lock.lock()
        _requests.append(r)
        lock.unlock()
        return try handler(r)
    }
}

// MARK: - StubURLProtocol

/// A `URLProtocol` that routes requests through the current `StubServer`.
final class StubURLProtocol: URLProtocol, @unchecked Sendable {
    nonisolated(unsafe) private static var current: StubServer?
    private static let lock = NSLock()

    private static func server() -> StubServer? {
        lock.lock()
        defer { lock.unlock() }
        return current
    }

    private static func setServer(_ server: StubServer) {
        lock.lock()
        current = server
        lock.unlock()
    }

    /// Installs `server` as the active stub and returns a session wired to it.
    static func session(_ server: StubServer) -> URLSession {
        setServer(server)
        let c = URLSessionConfiguration.ephemeral
        c.protocolClasses = [StubURLProtocol.self]
        return URLSession(configuration: c)
    }

    override class func canInit(with request: URLRequest) -> Bool { true }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        let body: Data
        if let httpBody = request.httpBody {
            body = httpBody
        } else if let stream = request.httpBodyStream {
            body = StubURLProtocol.readFully(stream)
        } else {
            body = Data()
        }

        let headers = request.allHTTPHeaderFields ?? [:]
        let stub = StubRequest(
            method: request.httpMethod ?? "GET",
            url: request.url!,
            headers: headers,
            body: body
        )

        guard let server = StubURLProtocol.server() else {
            client?.urlProtocol(self, didFailWithError: URLError(.cannotConnectToHost))
            return
        }

        let response: StubResponse
        do {
            response = try server.handle(stub)
        } catch {
            client?.urlProtocol(self, didFailWithError: error)
            return
        }

        let http = HTTPURLResponse(
            url: request.url!,
            statusCode: response.status,
            httpVersion: "HTTP/1.1",
            headerFields: response.headers
        )!
        client?.urlProtocol(self, didReceive: http, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: response.body)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}

    /// Drains an input stream completely.
    private static func readFully(_ stream: InputStream) -> Data {
        stream.open()
        defer { stream.close() }

        var data = Data()
        let bufferSize = 4096
        var buffer = [UInt8](repeating: 0, count: bufferSize)
        while stream.hasBytesAvailable {
            let read = stream.read(&buffer, maxLength: bufferSize)
            if read <= 0 { break }
            data.append(buffer, count: read)
        }
        return data
    }
}

// MARK: - Multipart parsing

/// Extracts the boundary from a `multipart/form-data; boundary=XYZ` content type
/// and maps each part's form `name` to its raw bytes.
func multipartParts(_ body: Data, contentType: String) -> [String: Data] {
    guard let boundary = multipartBoundary(in: contentType) else { return [:] }

    let bytes = [UInt8](body)
    let delimiter = Array("--\(boundary)".utf8)
    let nameToken = Array("name=\"".utf8)
    let dispositionToken = Array("Content-Disposition".utf8)
    let crlf = Array("\r\n".utf8)

    var result: [String: Data] = [:]

    for range in byteRanges(of: delimiter, in: bytes) {
        // A part begins just after the delimiter plus its trailing CRLF.
        var index = range.upperBound
        if matches(crlf, in: bytes, at: index) {
            index += crlf.count
        } else {
            continue
        }

        // Find the blank line separating part headers from the body.
        guard let headerEnd = indexOf(crlf + crlf, in: bytes, from: index) else { continue }
        let headEnd = headerEnd
        let bodyStart = headerEnd + 2 * crlf.count

        // Stop at the CRLF preceding the next boundary (or the trailing delimiter).
        var bodyEnd = bytes.count
        if let next = byteRanges(of: delimiter, in: bytes)
            .first(where: { $0.lowerBound >= bodyStart })?.lowerBound {
            bodyEnd = next
            if bodyEnd >= crlf.count,
               matches(crlf, in: bytes, at: bodyEnd - crlf.count) {
                bodyEnd -= crlf.count
            }
        }
        if bodyEnd < bodyStart { bodyEnd = bodyStart }

        // Only multipart form parts carry a Content-Disposition with a name.
        guard indexOf(dispositionToken, in: bytes, from: index, to: headEnd) != nil,
              let nameStart = indexOf(nameToken, in: bytes, from: index, to: headEnd) else {
            continue
        }
        let valueStart = nameStart + nameToken.count
        guard let quote = indexOf([UInt8(ascii: "\"")], in: bytes, from: valueStart, to: headEnd) else {
            continue
        }
        let name = String(decoding: bytes[valueStart..<quote], as: UTF8.self)

        result[name] = Data(bytes[bodyStart..<bodyEnd])
    }

    return result
}

/// Pulls the `boundary=` value out of a multipart content type.
private func multipartBoundary(in contentType: String) -> String? {
    let lowered = contentType.lowercased()
    guard lowered.contains("multipart/form-data") else { return nil }

    for parameter in contentType.split(separator: ";") {
        let pair = parameter.split(separator: "=", maxSplits: 1)
        guard pair.count == 2 else { continue }
        let key = pair[0].trimmingCharacters(in: .whitespaces).lowercased()
        guard key == "boundary" else { continue }
        var value = pair[1].trimmingCharacters(in: .whitespaces)
        if value.hasPrefix("\""), value.hasSuffix("\""), value.count >= 2 {
            value = String(value.dropFirst().dropLast())
        }
        return value.isEmpty ? nil : value
    }
    return nil
}

/// Finds every occurrence of `pattern` in `bytes`.
private func byteRanges(of pattern: [UInt8], in bytes: [UInt8]) -> [Range<Int>] {
    guard !pattern.isEmpty, pattern.count <= bytes.count else { return [] }
    var ranges: [Range<Int>] = []
    var i = 0
    while i <= bytes.count - pattern.count {
        if bytes[i] == pattern[0], Array(bytes[i..<(i + pattern.count)]) == pattern {
            let range = i..<(i + pattern.count)
            ranges.append(range)
            i += pattern.count
        } else {
            i += 1
        }
    }
    return ranges
}

/// Whether `pattern` occurs at exactly `index`.
private func matches(_ pattern: [UInt8], in bytes: [UInt8], at index: Int) -> Bool {
    guard index >= 0, index + pattern.count <= bytes.count else { return false }
    return Array(bytes[index..<(index + pattern.count)]) == pattern
}

/// First occurrence of `pattern` within `bytes[from..<to]` (or to the end).
private func indexOf(_ pattern: [UInt8], in bytes: [UInt8], from: Int) -> Int? {
    indexOf(pattern, in: bytes, from: from, to: bytes.count)
}

/// First occurrence of `pattern` within `bytes[from..<to]`.
private func indexOf(_ pattern: [UInt8], in bytes: [UInt8], from: Int, to: Int) -> Int? {
    guard !pattern.isEmpty else { return nil }
    let start = max(0, from)
    let end = min(to, bytes.count)
    guard start <= end - pattern.count else { return nil }
    var i = start
    while i <= end - pattern.count {
        if matches(pattern, in: bytes, at: i) {
            return i
        }
        i += 1
    }
    return nil
}

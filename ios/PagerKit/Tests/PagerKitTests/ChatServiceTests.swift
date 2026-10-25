import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

/// A lock-protected request counter for stub handlers that answer differently
/// per attempt (the retry path). `@unchecked Sendable` because `NSLock` guards
/// every access.
private final class RequestCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0

    /// Increment and return the new count (1 for the first call).
    func next() -> Int {
        lock.lock()
        defer { lock.unlock() }
        value += 1
        return value
    }
}

/// Records everything a `ChatService` sends a backend, so a test can assert on
/// the exact history (including the system-prompt-free user turn) and audio
/// bytes without capturing non-`Sendable` state. Optionally fails every call
/// with `error`.
private final class FakeChatBackend: ChatBackend, @unchecked Sendable {
    private let lock = NSLock()
    private var _histories: [[ChatTurn]] = []
    private var _wavs: [[UInt8]] = []
    private var _completeCalls = 0
    private var _error: ChatBackendError?

    /// Every `history` passed to `complete`, in order.
    var histories: [[ChatTurn]] {
        lock.lock()
        defer { lock.unlock() }
        return _histories
    }

    /// Every `wav` passed to `transcribe`, in order.
    var wavs: [[UInt8]] {
        lock.lock()
        defer { lock.unlock() }
        return _wavs
    }

    /// How many times `complete` was called.
    var completeCalls: Int {
        lock.lock()
        defer { lock.unlock() }
        return _completeCalls
    }

    /// Make every subsequent backend call fail with `error` (nil clears it).
    func setError(_ error: ChatBackendError?) {
        lock.lock()
        defer { lock.unlock() }
        _error = error
    }

    func transcribe(wav: [UInt8], deadline: ContinuousClock.Instant) async throws -> String {
        let error = lock.withLock { () -> ChatBackendError? in
            _wavs.append(wav)
            return _error
        }
        if let error { throw error }
        return "ola"
    }

    func complete(history: [ChatTurn], deadline: ContinuousClock.Instant) async throws -> String {
        let (calls, error) = lock.withLock { () -> (Int, ChatBackendError?) in
            _histories.append(history)
            _completeCalls += 1
            return (_completeCalls, _error)
        }
        if let error { throw error }
        return "reply \(calls)"
    }
}

/// Covers the Groq OpenAI-compatible backend (transcribe/complete, retry,
/// missing key, request shape) and the `ChatService` actor that bridges it to
/// the pager over a real `LinkSession` pair: text asks, the history cap and
/// reset, voice uploads (`chat.audio.begin` + `chat.ask audio_xfer`), CRC
/// mismatches, busy uploads and bad params. Ports the Go originals in
/// `server/internal/chat/chat_test.go` and the mock in
/// `tools/pagerlink/cmd/pagerlink/serve.go`.
final class ChatServiceTests: XCTestCase {

    /// The deterministic byte pattern (i*7+3) & 0xFF shared with the bulk tests.
    private func pattern(_ n: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: n)
        for i in 0..<n {
            bytes[i] = UInt8((i * 7 + 3) & 0xFF)
        }
        return bytes
    }

    /// A fresh two-session harness: the phone runs the `ChatService`, the pager
    /// drives it with requests.
    private func makePair() -> (phone: LinkSession, pager: LinkSession) {
        let (a, b) = LoopbackTransport.pair(packetSize: PagerLink.tcpPacket)
        return (LinkSession(transport: a), LinkSession(transport: b, role: .pager))
    }

    private func groqBackend(
        apiKey: @escaping @Sendable () -> String?,
        server: StubServer,
        sttLanguage: String? = GroqChatBackend.defaultSTTLanguage,
        sttPrompt: String? = GroqChatBackend.defaultSTTPrompt
    ) -> GroqChatBackend {
        GroqChatBackend(
            apiKey: apiKey,
            session: StubURLProtocol.session(server),
            baseURL: URL(string: "https://groq.test/openai/v1")!,
            sttLanguage: sttLanguage,
            sttPrompt: sttPrompt,
            baseBackoff: .milliseconds(1))
    }

    // MARK: - Groq backend: transcribe

    func testTranscribeSendsMultipartAudio() async throws {
        let wav = Array(String(repeating: "RIFFfakewavdata", count: 100).utf8)
        let server = StubServer { _ in
            .json(200, "{\"text\":\"  hello world \\n\"}")
        }
        let backend = groqBackend(apiKey: { "test-key" }, server: server)

        let text = try await backend.transcribe(wav: wav)
        XCTAssertEqual(text, "hello world")

        XCTAssertEqual(server.requests.count, 1)
        let req = try XCTUnwrap(server.requests.first)
        XCTAssertTrue(req.path.hasSuffix("/audio/transcriptions"))
        XCTAssertEqual(req.header("Authorization"), "Bearer test-key")

        let contentType = try XCTUnwrap(req.header("Content-Type"))
        let parts = multipartParts(req.body, contentType: contentType)
        XCTAssertEqual(parts["file"], Data(wav))
        let model = try XCTUnwrap(parts["model"])
        XCTAssertEqual(String(decoding: model, as: UTF8.self), GroqChatBackend.sttModel)
        let language = try XCTUnwrap(parts["language"])
        XCTAssertEqual(String(decoding: language, as: UTF8.self), "pt")
        let prompt = try XCTUnwrap(parts["prompt"])
        XCTAssertEqual(String(decoding: prompt, as: UTF8.self), GroqChatBackend.defaultSTTPrompt)
        let format = try XCTUnwrap(parts["response_format"])
        XCTAssertEqual(String(decoding: format, as: UTF8.self), "json")
    }

    func testTranscribeOmitsEmptyLanguageAndPrompt() async throws {
        let wav = Array("hello".utf8)
        let server = StubServer { _ in
            .json(200, "{\"text\":\"hi\"}")
        }
        let backend = groqBackend(
            apiKey: { "test-key" }, server: server, sttLanguage: nil, sttPrompt: "")

        _ = try await backend.transcribe(wav: wav)

        let req = try XCTUnwrap(server.requests.first)
        let contentType = try XCTUnwrap(req.header("Content-Type"))
        let parts = multipartParts(req.body, contentType: contentType)
        XCTAssertNil(parts["language"])
        XCTAssertNil(parts["prompt"])
        XCTAssertNotNil(parts["model"])
        XCTAssertEqual(parts["file"], Data(wav))
    }

    // MARK: - Groq backend: complete

    func testCompleteSurfacesNon2xxAsError() async throws {
        let server = StubServer { _ in
            .json(400, "{\"error\":{\"message\":\"bad model\"}}")
        }
        let backend = groqBackend(apiKey: { "test-key" }, server: server)

        do {
            _ = try await backend.complete(history: [ChatTurn(role: "user", content: "hi")])
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            guard case .upstream(let text) = error else {
                XCTFail("expected upstream error, got \(error)")
                return
            }
            XCTAssertTrue(text.hasPrefix("upstream 400"), "text=\(text)")
        }

        XCTAssertEqual(server.requests.count, 1, "400 must not be retried")
    }

    func testCompleteRetries5xx() async throws {
        let counter = RequestCounter()
        let server = StubServer { _ in
            if counter.next() <= 2 {
                return .json(503, "{}")
            }
            return .json(200, "{\"choices\":[{\"message\":{\"content\":\" hi there \"}}]}")
        }
        let backend = groqBackend(apiKey: { "test-key" }, server: server)

        let reply = try await backend.complete(history: [ChatTurn(role: "user", content: "hi")])
        XCTAssertEqual(reply, "hi there")
        XCTAssertEqual(server.requests.count, 3)
    }

    func testCompleteSendsSystemPromptAndHistory() async throws {
        let server = StubServer { _ in
            .json(200, "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}")
        }
        let backend = groqBackend(apiKey: { "test-key" }, server: server)

        _ = try await backend.complete(history: [ChatTurn(role: "user", content: "q")])

        let req = try XCTUnwrap(server.requests.first)
        XCTAssertEqual(req.header("Authorization"), "Bearer test-key")
        XCTAssertEqual(req.header("Content-Type"), "application/json")

        let body = try JSONValue.decode(bytes: Array(req.body))
        XCTAssertEqual(body["model"]?.stringValue, GroqChatBackend.chatModel)
        let messages = try XCTUnwrap(body["messages"])
        guard case .array(let list) = messages else {
            XCTFail("messages is not an array")
            return
        }
        XCTAssertEqual(list.count, 2)
        XCTAssertEqual(list[0], ["role": "system", "content": .string(GroqChatBackend.systemPrompt)])
        XCTAssertEqual(list[1], ["role": "user", "content": "q"])
    }

    func testMissingKeyThrowsWithoutRequest() async throws {
        let server = StubServer { _ in .json(200, "{}") }
        let backend = groqBackend(apiKey: { nil }, server: server)

        do {
            _ = try await backend.complete(history: [ChatTurn(role: "user", content: "hi")])
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            XCTAssertEqual(error, .missingKey)
        }

        do {
            _ = try await backend.transcribe(wav: pattern(16))
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            XCTAssertEqual(error, .missingKey)
        }

        XCTAssertTrue(server.requests.isEmpty)
    }

    // MARK: - ChatService

    func testTextAsk() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        let result = try await pager.call("chat.ask", params: ["text": "  hi "], timeout: .seconds(5))
        XCTAssertEqual(result["reply"]?.stringValue, "reply 1")
        XCTAssertNil(result["transcript"])
        if case .object(let object) = result {
            XCTAssertEqual(Set(object.keys), ["reply"])
        } else {
            XCTFail("expected an object result")
        }

        XCTAssertEqual(fake.histories, [[ChatTurn(role: "user", content: "hi")]])
        let count = await chat.historyCount
        XCTAssertEqual(count, 2)

        await phone.stop()
        await pager.stop()
    }

    func testHistoryCapAndReset() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        for i in 1...15 {
            _ = try await pager.call("chat.ask", params: ["text": .string("q\(i)")], timeout: .seconds(5))
        }

        XCTAssertEqual(fake.completeCalls, 15)
        let last = try XCTUnwrap(fake.histories.last)
        XCTAssertEqual(last.count, ChatService.maxHistory)
        XCTAssertEqual(last.last, ChatTurn(role: "user", content: "q15"))

        let reset = try await pager.call("chat.ask", params: ["reset": true], timeout: .seconds(5))
        XCTAssertEqual(reset["reply"]?.stringValue, "")
        let count = await chat.historyCount
        XCTAssertEqual(count, 0)
        XCTAssertEqual(fake.completeCalls, 15, "reset must not call the backend")

        await phone.stop()
        await pager.stop()
    }

    func testVoiceAsk() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        let data = pattern(32_044)
        let begin = try await pager.call("chat.audio.begin", params: [
            "size": .number(Double(data.count)),
            "crc32": .number(Double(CRC32.checksum(data))),
            "fmt": "wav",
        ], timeout: .seconds(5))
        let xfer = try XCTUnwrap(begin["xfer"]?.intValue)
        XCTAssertEqual(xfer % 2, 0)

        try await pager.sendBytes(xfer: UInt16(xfer), data: data, timeout: .seconds(5))

        let result = try await pager.call(
            "chat.ask", params: ["audio_xfer": .number(Double(xfer))], timeout: .seconds(5))
        XCTAssertEqual(result["reply"]?.stringValue, "reply 1")
        XCTAssertEqual(result["transcript"]?.stringValue, "ola")

        XCTAssertEqual(fake.wavs.count, 1)
        XCTAssertEqual(fake.wavs.first, data)
        XCTAssertEqual(fake.histories, [[ChatTurn(role: "user", content: "ola")]])

        await phone.stop()
        await pager.stop()
    }

    func testUnknownAudioXfer() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        do {
            _ = try await pager.call(
                "chat.ask", params: ["audio_xfer": 40], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "unknown audio_xfer")
        }

        await phone.stop()
        await pager.stop()
    }

    func testAudioNotFinishedIsBusy() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake, audioWait: .milliseconds(200))
        await chat.register()

        let begin = try await pager.call("chat.audio.begin", params: [
            "size": 10000,
            "crc32": 1,
            "fmt": "wav",
        ], timeout: .seconds(5))
        let xfer = try XCTUnwrap(begin["xfer"]?.intValue)

        // No DATA is sent, so the upload is still running when chat.ask looks.
        do {
            _ = try await pager.call(
                "chat.ask", params: ["audio_xfer": .number(Double(xfer))], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "busy")
        }

        await phone.stop()
        await pager.stop()
    }

    func testAudioCRCMismatch() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        let data = pattern(5_000)
        let begin = try await pager.call("chat.audio.begin", params: [
            "size": .number(Double(data.count)),
            "crc32": .number(Double(CRC32.checksum(data) ^ 1)),
            "fmt": "wav",
        ], timeout: .seconds(5))
        let xfer = try XCTUnwrap(begin["xfer"]?.intValue)

        try await pager.sendBytes(xfer: UInt16(xfer), data: data, timeout: .seconds(5))

        do {
            _ = try await pager.call(
                "chat.ask", params: ["audio_xfer": .number(Double(xfer))], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertTrue(error.msg.hasPrefix("audio upload failed"), "msg=\(error.msg)")
        }

        await phone.stop()
        await pager.stop()
    }

    func testBadParams() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        do {
            _ = try await pager.call("chat.ask", params: ["text": 5], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "bad chat.ask params")
        }

        do {
            _ = try await pager.call(
                "chat.audio.begin", params: ["size": "x", "crc32": 1], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "bad chat.audio.begin params")
        }

        do {
            _ = try await pager.call("chat.ask", params: [:], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "invalid_argument")
            XCTAssertEqual(error.msg, "text or audio required")
        }

        await phone.stop()
        await pager.stop()
    }

    func testMissingKeyIsUnauthorized() async throws {
        let server = StubServer { _ in .json(200, "{}") }
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let backend = groqBackend(apiKey: { nil }, server: server)
        let chat = ChatService(session: phone, backend: backend)
        await chat.register()

        do {
            _ = try await pager.call("chat.ask", params: ["text": "hi"], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "unauthorized")
        }

        XCTAssertTrue(server.requests.isEmpty)

        await phone.stop()
        await pager.stop()
    }

    func testUpstreamFailureIsHTTPError() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        fake.setError(.upstream("upstream 500: x"))
        let chat = ChatService(session: phone, backend: fake)
        await chat.register()

        do {
            _ = try await pager.call("chat.ask", params: ["text": "hi"], timeout: .seconds(5))
            XCTFail("expected error")
        } catch let error as RPCError {
            XCTAssertEqual(error.code, "http_error")
            XCTAssertEqual(error.msg, "llm: upstream 500: x")
        }

        await phone.stop()
        await pager.stop()
    }

    // MARK: - Deadlines (P7.59) and the upload cap (P7.63)

    /// Records every deadline a `ChatService` hands it (both the STT and the LLM
    /// path) and always answers "ok". `@unchecked Sendable` because `NSLock`
    /// guards every access.
    private final class DeadlineRecorder: ChatBackend, @unchecked Sendable {
        private let lock = NSLock()
        private var _deadlines: [ContinuousClock.Instant] = []

        /// Every deadline passed to a deadline-aware method, in order.
        var deadlines: [ContinuousClock.Instant] {
            lock.lock()
            defer { lock.unlock() }
            return _deadlines
        }

        private func record(_ deadline: ContinuousClock.Instant) {
            lock.lock()
            defer { lock.unlock() }
            _deadlines.append(deadline)
        }

        func transcribe(wav: [UInt8], deadline: ContinuousClock.Instant) async throws -> String {
            record(deadline)
            return "ok"
        }

        func complete(history: [ChatTurn], deadline: ContinuousClock.Instant) async throws -> String {
            record(deadline)
            return "ok"
        }
    }

    /// A retry whose backoff would land at or after the deadline must not start:
    /// one 500 attempt, then the backend surfaces that error right away.
    func testDeadlineStopsRetries() async throws {
        let server = StubServer { _ in .json(500, "{}") }
        let backend = GroqChatBackend(
            apiKey: { "test-key" },
            session: StubURLProtocol.session(server),
            baseURL: URL(string: "https://groq.test/openai/v1")!,
            baseBackoff: .milliseconds(200))

        let start = ContinuousClock.now
        do {
            _ = try await backend.complete(
                history: [ChatTurn(role: "user", content: "hi")],
                deadline: .now + .milliseconds(150))
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            guard case .upstream = error else {
                XCTFail("expected upstream error, got \(error)")
                return
            }
        }

        let elapsed = ContinuousClock.now - start
        XCTAssertEqual(server.requests.count, 1, "the 200 ms backoff would end past the deadline")
        XCTAssertLessThan(elapsed, .seconds(1), "must give up instead of sleeping past the deadline")
    }

    /// With room to spare in the budget all three attempts happen, so the final
    /// 500 is what surfaces.
    func testDeadlineAllowsRetriesWithinBudget() async throws {
        let server = StubServer { _ in .json(500, "{}") }
        let backend = GroqChatBackend(
            apiKey: { "test-key" },
            session: StubURLProtocol.session(server),
            baseURL: URL(string: "https://groq.test/openai/v1")!,
            baseBackoff: .milliseconds(10))

        do {
            _ = try await backend.complete(
                history: [ChatTurn(role: "user", content: "hi")],
                deadline: .now + .seconds(5))
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            guard case .upstream = error else {
                XCTFail("expected upstream error, got \(error)")
                return
            }
        }

        XCTAssertEqual(server.requests.count, GroqChatBackend.maxAttempts)
    }

    /// A deadline already past throws without touching the network.
    func testPastDeadlineSendsNothing() async throws {
        let server = StubServer { _ in .json(200, "{}") }
        let backend = GroqChatBackend(
            apiKey: { "test-key" },
            session: StubURLProtocol.session(server),
            baseURL: URL(string: "https://groq.test/openai/v1")!,
            baseBackoff: .milliseconds(10))

        do {
            _ = try await backend.complete(
                history: [ChatTurn(role: "user", content: "hi")],
                deadline: .now - .milliseconds(1))
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            guard case .upstream = error else {
                XCTFail("expected upstream error, got \(error)")
                return
            }
        }

        do {
            _ = try await backend.transcribe(wav: [1, 2, 3], deadline: .now - .milliseconds(1))
            XCTFail("expected error")
        } catch let error as ChatBackendError {
            guard case .upstream = error else {
                XCTFail("expected upstream error, got \(error)")
                return
            }
        }

        XCTAssertTrue(server.requests.isEmpty)
    }

    /// `chat.ask` computes one deadline up front (now + askBudget) and hands the
    /// same instant to the backend.
    func testAskPassesOneDeadline() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let recorder = DeadlineRecorder()
        let chat = ChatService(session: phone, backend: recorder, askBudget: .seconds(7))
        await chat.register()

        let result = try await pager.call("chat.ask", params: ["text": "hi"], timeout: .seconds(5))
        XCTAssertEqual(result["reply"]?.stringValue, "ok")

        let deadlines = recorder.deadlines
        XCTAssertEqual(deadlines.count, 1)
        let deadline = try XCTUnwrap(deadlines.first)
        let remaining = deadline - ContinuousClock.now
        XCTAssertGreaterThan(remaining, .seconds(5))
        XCTAssertLessThanOrEqual(remaining, .seconds(7))

        await phone.stop()
        await pager.stop()
    }

    /// A third `chat.audio.begin` drops the oldest upload, so WAVs the pager
    /// never asks about don't pile up.
    func testUploadsCappedAtTwo() async throws {
        let (phone, pager) = makePair()
        await phone.start()
        await pager.start()

        let fake = FakeChatBackend()
        let chat = ChatService(session: phone, backend: fake)

        for _ in 0..<3 {
            _ = try await chat.audioBegin([
                "size": .number(10_000),
                "crc32": .number(1),
                "fmt": "wav",
            ])
        }

        let count = await chat.uploadCount
        XCTAssertEqual(count, ChatService.maxUploads)
        XCTAssertEqual(count, 2)

        await phone.stop()
        await pager.stop()
    }
}

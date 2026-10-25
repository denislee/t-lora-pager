import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// One turn of chat history, as the OpenAI-compatible API names them.
public struct ChatTurn: Sendable, Equatable {
    public let role: String
    public let content: String

    public init(role: String, content: String) {
        self.role = role
        self.content = content
    }
}

/// Why a chat backend call failed.
public enum ChatBackendError: Error, Equatable, Sendable {
    /// No API key is configured on the phone.
    case missingKey
    /// The upstream failed; the text mirrors lilyhub's error strings
    /// (`upstream 400: …`, `groq: …`, `parse: …`, `no choices in response`).
    case upstream(String)
}

/// Speech-to-text plus chat completion, each finishing (retries included)
/// by `deadline` (P7.59). `complete` gets the conversation so far (oldest
/// first, ending with the user's new turn) without the system prompt, which
/// is the backend's business.
public protocol ChatBackend: Sendable {
    func transcribe(wav: [UInt8], deadline: ContinuousClock.Instant) async throws -> String
    func complete(history: [ChatTurn], deadline: ContinuousClock.Instant) async throws -> String
}

/// Groq's OpenAI-compatible API: Whisper for speech, Llama for the reply. A
/// port of lilyhub `server/internal/chat` (same models, prompt, STT language
/// and vocabulary hint, retry policy). The key comes from `apiKey` on every
/// call and is never logged or stored here.
public struct GroqChatBackend: ChatBackend {
    public static let defaultBaseURL = URL(string: "https://api.groq.com/openai/v1")!
    public static let sttModel = "whisper-large-v3-turbo"
    public static let chatModel = "llama-3.3-70b-versatile"
    public static let systemPrompt = "You are a concise assistant on a small pager screen. "
        + "Answer in plain text, no markdown, ≤120 words unless asked."
    /// ISO-639-1 hint for Whisper (lilyhub `STT_LANG` default).
    public static let defaultSTTLanguage = "pt"
    /// Vocabulary hint nudging Whisper toward pt-BR (lilyhub `STT_PROMPT`
    /// default).
    public static let defaultSTTPrompt = "Olá, tudo bem? Estou usando um celular, "
        + "ônibus, trem, geladeira, abacaxi, açaí, cafezinho, computador."
    /// Upstream attempts per call (lilyhub `httpx.DoWithRetry(…, 3, …)`).
    public static let maxAttempts = 3
    /// Per-request timeout (lilyhub `clientTimeout`); a deadline caps it.
    public static let requestTimeout: TimeInterval = 60
    /// Budget for a call made without a deadline.
    public static let defaultBudget: Duration = .seconds(55)

    private let apiKey: @Sendable () -> String?
    private let session: URLSession
    private let baseURL: URL
    private let sttLanguage: String?
    private let sttPrompt: String?
    private let baseBackoff: Duration

    /// `sttLanguage` / `sttPrompt` nil or empty leave the field out (Whisper
    /// auto-detects / no hint). `baseBackoff` is the first retry delay.
    public init(
        apiKey: @escaping @Sendable () -> String?,
        session: URLSession = .shared,
        baseURL: URL = GroqChatBackend.defaultBaseURL,
        sttLanguage: String? = GroqChatBackend.defaultSTTLanguage,
        sttPrompt: String? = GroqChatBackend.defaultSTTPrompt,
        baseBackoff: Duration = .milliseconds(200)
    ) {
        self.apiKey = apiKey
        self.session = session
        self.baseURL = baseURL
        self.sttLanguage = sttLanguage
        self.sttPrompt = sttPrompt
        self.baseBackoff = baseBackoff
    }

    private func key() throws -> String {
        guard let key = apiKey(), !key.isEmpty else { throw ChatBackendError.missingKey }
        return key
    }

    /// POST the WAV to `/audio/transcriptions` as multipart form data (file
    /// named `audio.wav` so Groq's sniffer accepts it) and return the trimmed
    /// text.
    public func transcribe(wav: [UInt8]) async throws -> String {
        try await transcribe(wav: wav, deadline: .now + Self.defaultBudget)
    }

    public func transcribe(wav: [UInt8], deadline: ContinuousClock.Instant) async throws -> String {
        let key = try key()
        let boundary = "pagerkit-" + UUID().uuidString
        var fields: [(String, String)] = [("model", Self.sttModel), ("response_format", "json")]
        if let lang = sttLanguage, !lang.isEmpty { fields.append(("language", lang)) }
        if let prompt = sttPrompt, !prompt.isEmpty { fields.append(("prompt", prompt)) }

        var body = Data()
        for (name, value) in fields {
            body.append(Data("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"\r\n\r\n".utf8))
            body.append(Data(value.utf8))
            body.append(Data("\r\n".utf8))
        }
        body.append(Data(("--\(boundary)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
            + "Content-Type: application/octet-stream\r\n\r\n").utf8))
        body.append(contentsOf: wav)
        body.append(Data("\r\n--\(boundary)--\r\n".utf8))

        var request = try HTTP.request(url: baseURL.appendingPathComponent("audio/transcriptions"),
                                       method: "POST", timeout: Self.requestTimeout, bearer: key)
        request.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        request.httpBody = body

        let value = try await exchange(request, deadline: deadline)
        return (value["text"]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// POST `{model, messages:[system] + history}` to `/chat/completions` and
    /// return the first choice's trimmed content.
    public func complete(history: [ChatTurn]) async throws -> String {
        try await complete(history: history, deadline: .now + Self.defaultBudget)
    }

    public func complete(history: [ChatTurn], deadline: ContinuousClock.Instant) async throws -> String {
        let key = try key()
        var messages: [JSONValue] = [["role": "system", "content": .string(Self.systemPrompt)]]
        for turn in history {
            messages.append(["role": .string(turn.role), "content": .string(turn.content)])
        }
        let payload: JSONValue = ["model": .string(Self.chatModel), "messages": .array(messages)]

        let request = try HTTP.request(url: baseURL.appendingPathComponent("chat/completions"),
                                       method: "POST", timeout: Self.requestTimeout, bearer: key, json: payload)

        let value = try await exchange(request, deadline: deadline)
        guard case .array(let choices)? = value["choices"], let first = choices.first else {
            throw ChatBackendError.upstream("no choices in response")
        }
        return (first["message"]?["content"]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// lilyhub `do`: retried exchange; non-2xx becomes `upstream N: <body>`,
    /// and a 2xx carrying Groq's `error.message` becomes `groq: <message>`.
    private func exchange(_ request: URLRequest, deadline: ContinuousClock.Instant) async throws -> JSONValue {
        let reply: HTTPReply
        do {
            reply = try await HTTP.sendWithRetry(request, using: session, attempts: Self.maxAttempts,
                                                 baseBackoff: baseBackoff, deadline: deadline)
        } catch let error as HTTPStatusError {
            throw ChatBackendError.upstream("upstream \(error.status): \(error.body)")
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            throw ChatBackendError.upstream("\(error)")
        }
        guard reply.status / 100 == 2 else {
            throw ChatBackendError.upstream("upstream \(reply.status): \(HTTP.truncate(HTTP.text(reply.body), 200))")
        }
        let value: JSONValue
        do {
            value = try JSONValue.decode(bytes: Array(reply.body))
        } catch {
            throw ChatBackendError.upstream("parse: \(error)")
        }
        if let message = value["error"]?["message"]?.stringValue {
            throw ChatBackendError.upstream("groq: \(message)")
        }
        return value
    }
}

/// Answers the pager's `chat.audio.begin` and `chat.ask` (PROTOCOL.md §6) on
/// one `LinkSession`, with the semantics of the emulator's mock phone
/// (`tools/pagerlink/cmd/pagerlink/serve.go`) and the history rules of lilyhub's
/// chat handler: one conversation per pager (this service), at most 20 turns,
/// each capped at 4 KiB, `reset` clears it.
public actor ChatService {
    /// History cap in turns (lilyhub `maxHistory`).
    public static let maxHistory = 20
    /// Per-turn cap in UTF-8 bytes (lilyhub `maxContent`).
    public static let maxContent = 4 * 1024
    /// How long `chat.ask` waits for a voice upload still arriving.
    public static let defaultAudioWait: Duration = .seconds(30)
    /// Idle bound on a voice upload's DATA (the mock's `wait(60 s)`).
    public static let uploadTimeout: Duration = .seconds(60)
    /// One deadline per `chat.ask` covering the upload wait, STT and LLM with
    /// their retries, under the pager's 60 s `CHAT_ASK_TIMEOUT_MS` (P7.59), so
    /// the phone stops spending quota once the pager has given up.
    public static let defaultAskBudget: Duration = .seconds(55)
    /// Voice uploads kept for a `chat.ask`; an older one is dropped (P7.63),
    /// so WAVs the pager never asked about don't pile up.
    public static let maxUploads = 2

    /// One `chat.audio.begin` upload: its outcome once known, and any
    /// `chat.ask` waiting for it.
    private final class Upload {
        var result: Result<[UInt8], RPCError>?
        var waiters: [UUID: CheckedContinuation<Result<[UInt8], RPCError>?, Never>] = [:]
    }

    private let session: LinkSession
    private let backend: ChatBackend
    private let audioWait: Duration
    private let askBudget: Duration
    private var history: [ChatTurn] = []
    private var uploads: [UInt16: Upload] = [:]
    private var uploadOrder: [UInt16] = []

    public init(session: LinkSession, backend: ChatBackend, audioWait: Duration = ChatService.defaultAudioWait,
                askBudget: Duration = ChatService.defaultAskBudget) {
        self.session = session
        self.backend = backend
        self.audioWait = audioWait
        self.askBudget = askBudget
    }

    /// Install the `chat.*` handlers on the session.
    public func register() async {
        await session.handle("chat.audio.begin") { [weak self] params in
            guard let self else { throw RPCError(code: "unsupported", msg: "chat service gone") }
            return try await self.audioBegin(params)
        }
        await session.handle("chat.ask") { [weak self] params in
            guard let self else { throw RPCError(code: "unsupported", msg: "chat service gone") }
            return try await self.ask(params)
        }
    }

    /// Turns currently held (user + assistant).
    public var historyCount: Int { history.count }

    // MARK: - chat.audio.begin

    func audioBegin(_ params: JSONValue) async throws -> JSONValue {
        guard let size = Params.uint32(params["size"]), let crc = Params.uint32(params["crc32"]),
              params["fmt"] == nil || params["fmt"]?.stringValue != nil else {
            throw RPCError(code: "invalid_argument", msg: "bad chat.audio.begin params")
        }
        let incoming = await session.acceptBytes(size: size, crc32: crc)
        uploads[incoming.xfer] = Upload()
        uploadOrder.removeAll { $0 == incoming.xfer }
        uploadOrder.append(incoming.xfer)
        while uploadOrder.count > Self.maxUploads {
            dropUpload(uploadOrder.removeFirst())
        }
        Task { [weak self] in
            let outcome: Result<[UInt8], RPCError>
            do {
                outcome = .success(try await incoming.wait(timeout: ChatService.uploadTimeout))
            } catch let error as RPCError {
                outcome = .failure(error)
            } catch {
                outcome = .failure(RPCError(code: "internal", msg: "\(error)"))
            }
            await self?.finishUpload(incoming.xfer, outcome)
        }
        return ["xfer": .number(Double(incoming.xfer))]
    }

    /// Forget an upload; a `chat.ask` still waiting for it sees "not finished".
    private func dropUpload(_ xfer: UInt16) {
        uploadOrder.removeAll { $0 == xfer }
        guard let upload = uploads.removeValue(forKey: xfer) else { return }
        let waiters = upload.waiters
        upload.waiters.removeAll()
        for (_, waiter) in waiters { waiter.resume(returning: nil) }
    }

    /// Uploads currently held (tests).
    public var uploadCount: Int { uploads.count }

    private func finishUpload(_ xfer: UInt16, _ outcome: Result<[UInt8], RPCError>) {
        guard let upload = uploads[xfer] else { return }
        upload.result = outcome
        let waiters = upload.waiters
        upload.waiters.removeAll()
        for (_, waiter) in waiters { waiter.resume(returning: outcome) }
    }

    /// The upload's outcome, or nil when it is still running after `audioWait`.
    private func awaitUpload(_ xfer: UInt16) async -> Result<[UInt8], RPCError>? {
        guard let upload = uploads[xfer] else { return nil }
        if let result = upload.result { return result }
        let token = UUID()
        let wait = audioWait
        return await withCheckedContinuation { continuation in
            addWaiter(xfer, token: token, continuation: continuation)
            Task { [weak self] in
                try? await Task.sleep(for: wait)
                await self?.expireWaiter(xfer, token: token)
            }
        }
    }

    private func addWaiter(_ xfer: UInt16, token: UUID,
                           continuation: CheckedContinuation<Result<[UInt8], RPCError>?, Never>) {
        guard let upload = uploads[xfer] else {
            continuation.resume(returning: nil)
            return
        }
        if let result = upload.result {
            continuation.resume(returning: result)
            return
        }
        upload.waiters[token] = continuation
    }

    private func expireWaiter(_ xfer: UInt16, token: UUID) {
        guard let upload = uploads[xfer], let waiter = upload.waiters.removeValue(forKey: token) else { return }
        waiter.resume(returning: nil)
    }

    // MARK: - chat.ask

    func ask(_ params: JSONValue) async throws -> JSONValue {
        let deadline = ContinuousClock.now + askBudget
        let text = params["text"]
        let audio = params["audio_xfer"]
        let reset = params["reset"]
        guard text == nil || text?.stringValue != nil,
              audio == nil || Params.uint16(audio) != nil,
              reset == nil || reset?.boolValue != nil else {
            throw RPCError(code: "invalid_argument", msg: "bad chat.ask params")
        }

        if reset?.boolValue == true {
            history.removeAll()
            return ["reply": ""]
        }

        var prompt = (text?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        var transcript = ""
        if let xfer = Params.uint16(audio), xfer != 0 {
            guard uploads[xfer] != nil else {
                throw RPCError(code: "invalid_argument", msg: "unknown audio_xfer")
            }
            guard let outcome = await awaitUpload(xfer) else {
                throw RPCError(code: "busy", msg: "audio upload not finished")
            }
            dropUpload(xfer)
            let wav: [UInt8]
            switch outcome {
            case .success(let bytes): wav = bytes
            case .failure(let error):
                throw RPCError(code: "invalid_argument", msg: "audio upload failed: \(error.msg)")
            }
            do {
                transcript = try await backend.transcribe(wav: wav, deadline: deadline)
            } catch {
                throw Self.backendFailure(error, stage: "stt")
            }
            prompt = transcript.trimmingCharacters(in: .whitespacesAndNewlines)
        }

        guard !prompt.isEmpty else {
            throw RPCError(code: "invalid_argument", msg: "text or audio required")
        }

        history.append(ChatTurn(role: "user", content: HTTP.truncate(prompt, Self.maxContent)))
        if history.count > Self.maxHistory {
            history.removeFirst(history.count - Self.maxHistory)
        }
        let snapshot = history

        let reply: String
        do {
            reply = try await backend.complete(history: snapshot, deadline: deadline)
        } catch {
            throw Self.backendFailure(error, stage: "llm")
        }
        history.append(ChatTurn(role: "assistant", content: HTTP.truncate(reply, Self.maxContent)))

        var result: [String: JSONValue] = ["reply": .string(reply)]
        if !transcript.isEmpty { result["transcript"] = .string(transcript) }
        return .object(result)
    }

    /// A missing key is `unauthorized` (§3.4's credential error); anything
    /// else upstream is `http_error` with lilyhub's `stt: …` / `llm: …` text.
    static func backendFailure(_ error: Error, stage: String) -> RPCError {
        switch error {
        case ChatBackendError.missingKey:
            return RPCError(code: "unauthorized", msg: "Groq API key not set on the phone")
        case ChatBackendError.upstream(let text):
            return RPCError(code: "http_error", msg: "\(stage): \(text)")
        case let rpc as RPCError:
            return rpc
        default:
            return RPCError(code: "http_error", msg: "\(stage): \(error)")
        }
    }
}

import Crypto
import Foundation

/// `ota.status` (PROTOCOL.md §6.7).
public struct OTAStatus: Sendable, Equatable {
    /// The running and the next app slot (`app0`/`app1`; `emu` on the emulator).
    public let running: String
    public let next: String
    /// The running image has not confirmed itself yet (60 s of uptime).
    public let pending: Bool
    /// Unconfirmed boots so far; the third rolls back.
    public let boots: Int
    /// The largest image the next slot takes.
    public let maxSize: UInt32

    public init(running: String, next: String, pending: Bool, boots: Int, maxSize: UInt32) {
        self.running = running
        self.next = next
        self.pending = pending
        self.boots = boots
        self.maxSize = maxSize
    }

    public init?(_ value: JSONValue) {
        guard let running = value["running"]?.stringValue, let next = value["next"]?.stringValue,
              let maxSize = Params.uint32(value["max_size"]) else { return nil }
        self.init(running: running, next: next, pending: value["pending"]?.boolValue ?? false,
                  boots: value["boots"]?.intValue ?? 0, maxSize: maxSize)
    }
}

/// Where an install is.
public enum OTAStage: String, Sendable, Equatable {
    /// `fs.write` of the image to `/ota.bin`.
    case upload
    /// The pager checks SHA-256 and the ESP image header.
    case verify
    /// The pager writes the next slot.
    case write
}

public struct OTAProgress: Sendable, Equatable {
    public let stage: OTAStage
    public let done: UInt32
    public let total: UInt32

    public init(stage: OTAStage, done: UInt32, total: UInt32) {
        self.stage = stage
        self.done = done
        self.total = total
    }

    public var fraction: Double { total == 0 ? 0 : Double(done) / Double(total) }
}

/// How an install ended.
public enum OTAOutcome: Sendable, Equatable {
    /// The pager is restarting into the new image (it then boots *pending*
    /// and rolls back after three unconfirmed boots).
    case reboot
    /// The emulator copied the image instead of rebooting.
    case installed
}

public enum OTAError: Error, Equatable, Sendable {
    case empty
    /// The first byte is not the ESP image magic `0xE9`.
    case notAnImage
    case tooLarge(max: UInt32)
    /// The pager's `evt.ota` `error` msg.
    case failed(String)
    /// No `evt.ota` for the job for the idle timeout.
    case timeout
    /// The link dropped before the job ended.
    case disconnected
}

/// Firmware update over PagerLink (PROTOCOL.md §6.7, Go `otaJob` in
/// `smoke_messaging.go`): check the image (ESP magic, size ≤ `max_size`),
/// upload it to `int:/ota.bin` with `fs.write` (overwrite), `ota.apply` it
/// with its SHA-256, then follow `evt.ota` for that job to `reboot` /
/// `installed` / `error`.
///
/// `/ota.bin` is deleted after `installed` or `error`. After `reboot` the
/// pager restarts before anything can be deleted, so `removeLeftover()`
/// (run on the next connect) deletes it then.
///
/// The session's one event stream belongs to its owner, which feeds every
/// event to `handleEvent` (as it does for settings) and calls `linkClosed`
/// when the link drops.
public actor OTAService {
    public static let volume = "int"
    public static let path = "/ota.bin"
    /// ESP32 app image header magic.
    public static let imageMagic: UInt8 = 0xE9

    private struct Event: Sendable {
        let job: String
        let phase: String
        let done: UInt32
        let total: UInt32
        let msg: String?
    }

    private enum Item: Sendable {
        case event(Event)
        case timeout
        case closed
    }

    private let session: LinkSession
    private let idleTimeout: Duration
    private let uploadTimeout: Duration
    private var listener: AsyncStream<Item>.Continuation?
    private var lastEvent = ContinuousClock.now

    public init(session: LinkSession, idleTimeout: Duration = .seconds(90), uploadTimeout: Duration = .seconds(30)) {
        self.session = session
        self.idleTimeout = idleTimeout
        self.uploadTimeout = uploadTimeout
    }

    public func status() async throws -> OTAStatus {
        let reply = try await session.call("ota.status", timeout: .seconds(10))
        guard let status = OTAStatus(reply) else { throw RPCError(code: "internal", msg: "ota.status: bad response") }
        return status
    }

    /// 64 lowercase hex digits.
    public static func sha256Hex(_ bytes: [UInt8]) -> String {
        SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
    }

    /// The checks the pager would fail later, done before any upload.
    public static func check(_ image: [UInt8], maxSize: UInt32) throws {
        guard let first = image.first else { throw OTAError.empty }
        guard first == imageMagic else { throw OTAError.notAnImage }
        guard image.count <= Int(maxSize) else { throw OTAError.tooLarge(max: maxSize) }
    }

    /// Feed every session event here; only `evt.ota` is used.
    public func handleEvent(name: String, params: JSONValue) {
        guard name == "evt.ota", let listener,
              let job = ScreenCaptureService.jobID(params["job"]),
              let phase = params["phase"]?.stringValue else { return }
        lastEvent = .now
        listener.yield(.event(Event(job: job, phase: phase, done: Params.uint32(params["done"]) ?? 0,
                                    total: Params.uint32(params["total"]) ?? 0, msg: params["msg"]?.stringValue)))
    }

    /// The link dropped: an install waiting for events ends `disconnected`.
    public func linkClosed() {
        listener?.yield(.closed)
    }

    /// Upload, apply and follow one image. Throws `OTAError` for the local
    /// checks and the job's outcome, `RPCError` for a refused upload or
    /// `ota.apply` (`busy`, `invalid_argument`).
    public func install(_ image: [UInt8],
                        progress: @escaping @Sendable (OTAProgress) -> Void = { _ in }) async throws -> OTAOutcome {
        let status = try await status()
        try Self.check(image, maxSize: status.maxSize)
        let total = UInt32(image.count)
        let sha = Self.sha256Hex(image)

        try await session.writeFile(vol: Self.volume, path: Self.path, data: image, overwrite: true,
                                    timeout: uploadTimeout) { done, all in
            progress(OTAProgress(stage: .upload, done: done, total: all))
        }

        // Listen before applying: an evt.ota may overtake the ota.apply reply.
        let (stream, continuation) = AsyncStream.makeStream(of: Item.self)
        listener = continuation
        lastEvent = .now
        let watchdog = startWatchdog()
        defer {
            watchdog.cancel()
            continuation.finish()
            listener = nil
        }

        let reply = try await session.call("ota.apply", params: [
            "vol": .string(Self.volume), "path": .string(Self.path),
            "size": .number(Double(total)), "sha256": .string(sha),
        ], timeout: .seconds(15))
        guard let job = ScreenCaptureService.jobID(reply["job"]) else {
            throw RPCError(code: "internal", msg: "ota.apply: bad response")
        }

        for await item in stream {
            switch item {
            case .timeout:
                throw OTAError.timeout
            case .closed:
                throw OTAError.disconnected
            case .event(let event) where event.job == job:
                switch event.phase {
                case "verify":
                    progress(OTAProgress(stage: .verify, done: event.done, total: event.total))
                case "write":
                    progress(OTAProgress(stage: .write, done: event.done, total: event.total))
                case "reboot":
                    return .reboot
                case "installed":
                    _ = await removeLeftover()
                    return .installed
                case "error":
                    _ = await removeLeftover()
                    throw OTAError.failed(event.msg ?? "error")
                default:
                    continue
                }
            case .event:
                continue
            }
        }
        throw OTAError.disconnected
    }

    private func startWatchdog() -> Task<Void, Never> {
        let limit = idleTimeout
        return Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: limit / 4)
                guard let self, !Task.isCancelled else { return }
                if await self.idleExpired(limit) { return }
            }
        }
    }

    private func idleExpired(_ limit: Duration) -> Bool {
        guard ContinuousClock.now - lastEvent >= limit else { return false }
        listener?.yield(.timeout)
        return true
    }

    /// Delete `int:/ota.bin` if it is there (an image left by an install that
    /// rebooted). True when one was removed.
    @discardableResult
    public func removeLeftover() async -> Bool {
        let fs = FileService(session: session, timeout: .seconds(10))
        guard (try? await fs.stat(vol: Self.volume, path: Self.path)) != nil else { return false }
        return (try? await fs.delete(vol: Self.volume, path: Self.path)) != nil
    }
}

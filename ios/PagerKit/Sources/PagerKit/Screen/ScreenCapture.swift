import Foundation

/// The `rgb565-rle` screen format (PROTOCOL.md §6.6): 4-byte runs
/// `u16 count, u16 pixel`, little-endian, row-major from the top-left;
/// counts 1…65535, a run may cross rows, and they add up to exactly w × h.
/// Mirrors the Go `EncodeRLE565` / `DecodeRLE565`; `vectors/screen.json`
/// pins both.
public enum RLE565 {
    public static let format = "rgb565-rle"

    public enum DecodeError: Error, Equatable, Sendable {
        case lengthNotMultipleOf4
        case zeroLengthRun
        case morePixelsThanImage
        case fewerPixelsThanImage
    }

    public static func encode(_ pixels: [UInt16]) -> [UInt8] {
        var out: [UInt8] = []
        var i = 0
        while i < pixels.count {
            let v = pixels[i]
            var run = 1
            while i + run < pixels.count, pixels[i + run] == v, run < 0xFFFF { run += 1 }
            out += [UInt8(run & 0xFF), UInt8(run >> 8), UInt8(v & 0xFF), UInt8(v >> 8)]
            i += run
        }
        return out
    }

    public static func decode(_ data: [UInt8], width: Int, height: Int) throws -> [UInt16] {
        guard data.count % 4 == 0 else { throw DecodeError.lengthNotMultipleOf4 }
        let total = width * height
        var out: [UInt16] = []
        out.reserveCapacity(total)
        var i = 0
        while i < data.count {
            let count = Int(data[i]) | Int(data[i + 1]) << 8
            let px = UInt16(data[i + 2]) | UInt16(data[i + 3]) << 8
            guard count > 0 else { throw DecodeError.zeroLengthRun }
            guard out.count + count <= total else { throw DecodeError.morePixelsThanImage }
            out.append(contentsOf: repeatElement(px, count: count))
            i += 4
        }
        guard out.count == total else { throw DecodeError.fewerPixelsThanImage }
        return out
    }

    /// RGBA8 (alpha 255) from RGB565, each channel `v * 255 / max` (integer
    /// division), as the emulator's screenshot sweep writes PPM.
    public static func rgba(_ pixels: [UInt16]) -> [UInt8] {
        var out = [UInt8](repeating: 255, count: pixels.count * 4)
        for (n, p) in pixels.enumerated() {
            let r = Int(p >> 11) & 0x1F, g = Int(p >> 5) & 0x3F, b = Int(p) & 0x1F
            out[n * 4] = UInt8(r * 255 / 31)
            out[n * 4 + 1] = UInt8(g * 255 / 63)
            out[n * 4 + 2] = UInt8(b * 255 / 31)
        }
        return out
    }
}

/// One decoded capture of the pager's screen.
public struct ScreenImage: Sendable, Equatable {
    public let width: Int
    public let height: Int
    /// RGB565, row-major.
    public let pixels: [UInt16]
    public let capturedAt: Date

    public init(width: Int, height: Int, pixels: [UInt16], capturedAt: Date = Date()) {
        self.width = width
        self.height = height
        self.pixels = pixels
        self.capturedAt = capturedAt
    }

    /// RGBA8 bytes, `width * 4` per row (for a CGImage).
    public var rgba: [UInt8] { RLE565.rgba(pixels) }
}

/// Screen capture (PROTOCOL.md §6.6) on one `LinkSession`: `capture()` asks
/// with `screen.capture` (`{"job"}` at once), then the pager calls
/// `screen.data` (`{"job","w","h","fmt","size","crc32"}`), which this
/// service answers like `chat.audio.begin`: it allocates the `xfer`, takes
/// the DATA, checks the CRC-32 and decodes the runs. A `screen.data` that
/// arrives before the `screen.capture` reply is kept for its job.
public actor ScreenCaptureService {
    /// How long `capture()` waits for the pixels after the job id.
    public static let defaultTimeout: Duration = .seconds(20)
    /// Idle bound on the DATA of one capture.
    public static let dataTimeout: Duration = .seconds(10)

    private let session: LinkSession
    private let timeout: Duration
    private var results: [String: Result<ScreenImage, RPCError>] = [:]
    private var resultOrder: [String] = []
    private var waiters: [String: CheckedContinuation<Result<ScreenImage, RPCError>, Never>] = [:]

    public init(session: LinkSession, timeout: Duration = ScreenCaptureService.defaultTimeout) {
        self.session = session
        self.timeout = timeout
    }

    /// Install the `screen.data` handler.
    public func register() async {
        await session.handle("screen.data") { [weak self] params in
            guard let self else { throw RPCError(code: "unsupported", msg: "screen service gone") }
            return try await self.screenData(params)
        }
    }

    /// The job id as a string (the firmware sends a string; a number is accepted).
    static func jobID(_ value: JSONValue?) -> String? {
        switch value {
        case .string(let s)? where !s.isEmpty: return s
        case .number(let n)? where n == n.rounded(): return String(Int(n))
        default: return nil
        }
    }

    func screenData(_ params: JSONValue) async throws -> JSONValue {
        guard let job = Self.jobID(params["job"]),
              let w = params["w"]?.intValue, let h = params["h"]?.intValue, w > 0, h > 0, w <= 4096, h <= 4096,
              let size = Params.uint32(params["size"]), let crc = Params.uint32(params["crc32"]) else {
            throw RPCError(code: "invalid_argument", msg: "bad screen.data params")
        }
        guard params["fmt"]?.stringValue == RLE565.format else {
            throw RPCError(code: "invalid_argument", msg: "fmt: unsupported")
        }
        // At most one 4-byte run per pixel.
        guard size % 4 == 0, Int(size) <= w * h * 4 else {
            throw RPCError(code: "invalid_argument", msg: "size: out of range")
        }
        let incoming = await session.acceptBytes(size: size, crc32: crc)
        Task { [weak self] in
            let outcome: Result<ScreenImage, RPCError>
            do {
                let bytes = try await incoming.wait(timeout: ScreenCaptureService.dataTimeout)
                let pixels = try RLE565.decode(bytes, width: w, height: h)
                outcome = .success(ScreenImage(width: w, height: h, pixels: pixels))
            } catch let error as RPCError {
                outcome = .failure(error)
            } catch {
                outcome = .failure(RPCError(code: "invalid_argument", msg: "screen: \(error)"))
            }
            await self?.deliver(job, outcome)
        }
        return ["xfer": .number(Double(incoming.xfer))]
    }

    private func deliver(_ job: String, _ outcome: Result<ScreenImage, RPCError>) {
        if let waiter = waiters.removeValue(forKey: job) {
            waiter.resume(returning: outcome)
            return
        }
        results[job] = outcome
        resultOrder.append(job)
        while resultOrder.count > 4 { results.removeValue(forKey: resultOrder.removeFirst()) }
    }

    /// Capture the pager's screen. Throws the pager's error (`busy` while
    /// another capture runs), `timeout` when no pixels arrive, or the
    /// transfer / decode failure.
    public func capture() async throws -> ScreenImage {
        let reply = try await session.call("screen.capture", timeout: .seconds(10))
        guard let job = Self.jobID(reply["job"]) else {
            throw RPCError(code: "internal", msg: "screen.capture: bad response")
        }
        let outcome = await wait(job)
        return try outcome.get()
    }

    private func wait(_ job: String) async -> Result<ScreenImage, RPCError> {
        if let ready = results.removeValue(forKey: job) {
            resultOrder.removeAll { $0 == job }
            return ready
        }
        let limit = timeout
        return await withCheckedContinuation { continuation in
            waiters[job] = continuation
            Task { [weak self] in
                try? await Task.sleep(for: limit)
                await self?.expire(job)
            }
        }
    }

    private func expire(_ job: String) {
        waiters.removeValue(forKey: job)?.resume(returning: .failure(RPCError.timeout("screen: no pixels")))
    }
}

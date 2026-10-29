import Crypto
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

@testable import PagerKit

/// Collects install progress.
private final class Stages: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [OTAProgress] = []
    func add(_ p: OTAProgress) { lock.withLock { items.append(p) } }
    var all: [OTAProgress] { lock.withLock { items } }
}

/// `OTAService` against a fake pager: `ota.status`, the `fs.write` upload
/// (CompanionFakeFs), `ota.apply` with the pager's up-front checks, and an
/// `evt.ota` job like the emulator's (including an event that overtakes the
/// `ota.apply` reply and events of other jobs), plus `FirmwareReleases`.
final class OTAServiceTests: XCTestCase {
    /// How the fake pager ends a job.
    enum Ending { case installed, reboot, error(String), silent }

    private func setUp(ending: Ending, maxSize: UInt32 = 1_000_000) async
        -> (OTAService, CompanionFakeFs, LinkSession) {
        let fs = CompanionFakeFs()
        let (phone, pager) = await fs.makeSessions()
        await pager.handle("ota.status") { _ in
            ["running": "app0", "next": "app1", "pending": false, "boots": 0, "max_size": .number(Double(maxSize))]
        }
        await pager.handle("ota.apply") { p in
            let path = p["path"]?.stringValue ?? ""
            guard let bytes = fs.bytes("int", path) else { throw RPCError(code: "invalid_argument", msg: "not found") }
            guard Params.uint32(p["size"]) == UInt32(bytes.count) else {
                throw RPCError(code: "invalid_argument", msg: "size mismatch")
            }
            guard let sha = p["sha256"]?.stringValue, sha.count == 64 else {
                throw RPCError(code: "invalid_argument", msg: "bad sha256")
            }
            let total = Double(bytes.count)
            // Another job's noise, and a verify that overtakes the reply.
            await pager.emit("evt.ota", params: ["job": "other", "phase": "error", "done": 0, "total": 0, "msg": "x"])
            await pager.emit("evt.ota", params: ["job": "j1", "phase": "verify", "done": 0, "total": .number(total)])
            Task {
                try? await Task.sleep(for: .milliseconds(50))
                await pager.emit("evt.ota", params: ["job": "j1", "phase": "write", "done": .number(total / 2),
                                                     "total": .number(total)])
                switch ending {
                case .installed:
                    await pager.emit("evt.ota", params: ["job": "j1", "phase": "installed", "done": .number(total),
                                                         "total": .number(total)])
                case .reboot:
                    await pager.emit("evt.ota", params: ["job": "j1", "phase": "reboot", "done": .number(total),
                                                         "total": .number(total)])
                case .error(let msg):
                    await pager.emit("evt.ota", params: ["job": "j1", "phase": "error", "done": 0, "total": 0,
                                                         "msg": .string(msg)])
                case .silent:
                    break
                }
            }
            return ["job": "j1"]
        }
        let ota = OTAService(session: phone, idleTimeout: .milliseconds(400), uploadTimeout: .seconds(5))
        // The owner feeds the session's events, as PagerSession does.
        Task { for await e in phone.events { await ota.handleEvent(name: e.name, params: e.params) } }
        return (ota, fs, phone)
    }

    private func image(_ count: Int) -> [UInt8] {
        [0xE9] + (1..<count).map { UInt8(truncatingIfNeeded: $0 &* 7) }
    }

    func testInstallOnEmulatorEndsInstalledAndCleansUp() async throws {
        let (ota, fs, _) = await setUp(ending: .installed)
        let img = image(20_000)
        let stages = Stages()
        let outcome = try await ota.install(img) { stages.add($0) }
        XCTAssertEqual(outcome, .installed)
        let seen = stages.all.map(\.stage)
        XCTAssertEqual(seen.first, .upload)
        XCTAssertEqual(stages.all.last(where: { $0.stage == .upload })?.done, 20_000)
        XCTAssertTrue(seen.contains(.verify) && seen.contains(.write))
        XCTAssertNil(fs.bytes("int", "/ota.bin"), "image removed after installed")
        XCTAssertEqual(OTAService.sha256Hex(img), SHA256.hash(data: img).map { String(format: "%02x", $0) }.joined())
    }

    func testRebootLeavesTheFileForTheNextConnect() async throws {
        let (ota, fs, _) = await setUp(ending: .reboot)
        let img = image(5000)
        let outcome = try await ota.install(img)
        XCTAssertEqual(outcome, .reboot)
        XCTAssertEqual(fs.bytes("int", "/ota.bin"), img)
        let removed = await ota.removeLeftover()
        XCTAssertTrue(removed)
        XCTAssertNil(fs.bytes("int", "/ota.bin"))
        let again = await ota.removeLeftover()
        XCTAssertFalse(again)
    }

    func testJobErrorAndTimeout() async throws {
        let (ota, fs, _) = await setUp(ending: .error("sha256 mismatch"))
        do {
            _ = try await ota.install(image(3000))
            XCTFail("expected error")
        } catch let error as OTAError {
            XCTAssertEqual(error, .failed("sha256 mismatch"))
        }
        XCTAssertNil(fs.bytes("int", "/ota.bin"))

        let (silent, _, _) = await setUp(ending: .silent)
        do {
            _ = try await silent.install(image(3000))
            XCTFail("expected timeout")
        } catch let error as OTAError {
            XCTAssertEqual(error, .timeout)
        }
    }

    func testLocalChecksBeforeUpload() async throws {
        let (ota, fs, _) = await setUp(ending: .installed, maxSize: 4096)
        for (bytes, expected) in [([UInt8](), OTAError.empty), ([0x7F, 0, 0], .notAnImage),
                                  (image(5000), .tooLarge(max: 4096))] {
            do {
                _ = try await ota.install(bytes)
                XCTFail("accepted \(expected)")
            } catch let error as OTAError {
                XCTAssertEqual(error, expected)
            }
        }
        XCTAssertFalse(fs.methods.contains("fs.write.begin"), "nothing uploaded")
        let status = try await ota.status()
        XCTAssertEqual(status, OTAStatus(running: "app0", next: "app1", pending: false, boots: 0, maxSize: 4096))
    }

    func testLinkDropEndsTheWait() async throws {
        let (ota, _, _) = await setUp(ending: .silent)
        let img = image(3000)
        let task = Task { try await ota.install(img) }
        try await Task.sleep(for: .milliseconds(150))
        await ota.linkClosed()
        do {
            _ = try await task.value
            XCTFail("expected disconnected")
        } catch let error as OTAError {
            XCTAssertEqual(error, .disconnected)
        }
    }

    // MARK: - GitHub releases

    private static let releases = #"""
        [{"tag_name":"v1.2","name":"Pager 1.2","draft":false,"prerelease":false,"published_at":"2026-10-20T10:00:00Z",
          "assets":[{"name":"firmware.bin","size":4,"browser_download_url":"https://github.com/o/r/releases/download/v1.2/firmware.bin",
                     "digest":"sha256:DIGEST"},
                    {"name":"notes.txt","size":3,"browser_download_url":"https://x/notes.txt"}]},
         {"tag_name":"v1.3-rc","draft":true,"assets":[{"name":"d.bin","size":1,"browser_download_url":"https://x/d.bin"}]},
         {"tag_name":"v1.1","prerelease":true,"assets":[{"name":"old.BIN","size":2,"browser_download_url":"https://x/old.BIN"}]}]
        """#

    func testReleasesListAndVerifiedDownload() async throws {
        let payload: [UInt8] = [0xE9, 1, 2, 3]
        let body = Self.releases.replacingOccurrences(of: "DIGEST", with: OTAService.sha256Hex(payload))
        let server = StubServer { request in
            if request.path == "/repos/o/r/releases" {
                XCTAssertEqual(request.header("Accept"), "application/vnd.github+json")
                return .json(200, body)
            }
            if request.path.hasSuffix("/firmware.bin") { return StubResponse(status: 200, body: Data(payload)) }
            return StubResponse(status: 404)
        }
        let session = StubURLProtocol.session(server)
        let assets = try await FirmwareReleases.fetch(repo: "o/r", urlSession: session)
        XCTAssertEqual(assets.map(\.name), ["firmware.bin", "old.BIN"], "drafts and non-.bin skipped")
        XCTAssertEqual(assets[0].sha256, OTAService.sha256Hex(payload))
        XCTAssertNil(assets[1].sha256)
        XCTAssertTrue(assets[1].prerelease)
        XCTAssertNotNil(assets[0].publishedAt)
        let bytes = try await FirmwareReleases.download(assets[0], urlSession: session)
        XCTAssertEqual(bytes, payload)

        let tampered = FirmwareAsset(tag: "v", releaseName: "v", prerelease: false, name: "firmware.bin", size: 4,
                                     url: assets[0].url, sha256: String(repeating: "0", count: 64), publishedAt: nil)
        do {
            _ = try await FirmwareReleases.download(tampered, urlSession: session)
            XCTFail("digest mismatch accepted")
        } catch let error as FirmwareReleaseError {
            guard case .digestMismatch = error else { return XCTFail("\(error)") }
        }
        XCTAssertNil(FirmwareReleases.parseRepo("o/r/x"))
        XCTAssertNil(FirmwareReleases.parseRepo("o/"))
        XCTAssertNil(FirmwareReleases.parseRepo("o r/x"))
        XCTAssertEqual(FirmwareReleases.parseRepo(" lilygo/t-lora-pager ")?.repo, "t-lora-pager")
    }
}

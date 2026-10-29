import Foundation
import PagerKit

/// The Firmware screen's state (S12, PROTOCOL.md §6.7): a GitHub repo's
/// release images (`.bin` assets, public API, no token), one downloaded and
/// checked against GitHub's `digest` when there is one, the pager's
/// `ota.status`, and the install (`OTAService` through `PagerSession`) with
/// its progress. Needs the pager's `"ota"` cap.
@MainActor
final class FirmwareModel: ObservableObject {
    @Published var repo = UserDefaults.standard.string(forKey: FirmwareModel.repoDefault) ?? "" {
        didSet { UserDefaults.standard.set(repo, forKey: Self.repoDefault) }
    }
    @Published private(set) var assets: [FirmwareAsset] = []
    @Published private(set) var loading = false
    @Published private(set) var status: OTAStatus?
    /// The image ready to install, with the asset it came from.
    @Published private(set) var image: (asset: FirmwareAsset, bytes: [UInt8], verified: Bool)?
    @Published private(set) var downloading: FirmwareAsset?
    @Published private(set) var progress: OTAProgress?
    @Published private(set) var installing = false
    @Published private(set) var result: String?
    @Published var errorText: String?

    private weak var session: PagerSession?
    var log: (String) -> Void = { _ in }

    nonisolated private static let repoDefault = "Firmware.repo"

    var canInstall: Bool { image != nil && !installing && (session?.has("ota") ?? false) }

    func attach(_ session: PagerSession) {
        self.session = session
        guard session.has("ota") else { return }
        Task { await refreshStatus() }
    }

    func detach() {
        status = nil
        if installing { progress = nil }
    }

    func refreshStatus() async {
        guard let session else { return }
        do {
            status = try await session.otaStatus()
        } catch {
            errorText = "ota.status failed: \(NotesModel.describe(error))"
        }
    }

    func loadReleases() async {
        loading = true
        defer { loading = false }
        do {
            assets = try await FirmwareReleases.fetch(repo: repo)
            errorText = assets.isEmpty ? "No .bin assets in that repo's releases." : nil
        } catch {
            assets = []
            errorText = Self.describe(error)
        }
    }

    func download(_ asset: FirmwareAsset) async {
        downloading = asset
        defer { downloading = nil }
        do {
            let bytes = try await FirmwareReleases.download(asset)
            if let status { try OTAService.check(bytes, maxSize: status.maxSize) }
            image = (asset, bytes, asset.sha256 != nil)
            result = nil
            errorText = nil
            log("firmware: downloaded \(asset.name) (\(bytes.count) B, digest \(asset.sha256 == nil ? "none" : "ok"))")
        } catch {
            image = nil
            errorText = Self.describe(error)
        }
    }

    func install() async {
        guard let session, let image, canInstall else { return }
        installing = true
        result = nil
        errorText = nil
        defer {
            installing = false
            progress = nil
        }
        do {
            let outcome = try await session.installFirmware(image.bytes) { [weak self] step in
                Task { @MainActor in self?.progress = step }
            }
            result = outcome == .reboot
                ? "\(image.asset.name) is written. The pager is restarting into it; it confirms itself after 60 s "
                    + "of running, and goes back to the previous firmware after 3 boots that don't."
                : "Installed on the emulator (no reboot)."
        } catch {
            errorText = "Install failed: \(Self.describe(error))"
        }
    }

    nonisolated static func describe(_ error: Error) -> String {
        switch error {
        case OTAError.empty: return "The image is empty."
        case OTAError.notAnImage: return "That file is not an ESP32 app image (first byte is not 0xE9)."
        case OTAError.tooLarge(let max): return "The image is larger than the pager's update slot (\(max) bytes)."
        case OTAError.failed(let msg): return "The pager refused it: \(msg)"
        case OTAError.timeout: return "The pager stopped reporting progress."
        case OTAError.disconnected: return "The link dropped during the install."
        case FirmwareReleaseError.badRepo: return "Enter the repository as owner/name."
        case FirmwareReleaseError.http(let status): return "GitHub answered HTTP \(status)."
        case FirmwareReleaseError.badResponse: return "GitHub's reply could not be read."
        case FirmwareReleaseError.digestMismatch: return "The download does not match GitHub's SHA-256 digest."
        case FirmwareReleaseError.sizeMismatch: return "The download is not the size GitHub lists."
        default: return NotesModel.describe(error)
        }
    }
}

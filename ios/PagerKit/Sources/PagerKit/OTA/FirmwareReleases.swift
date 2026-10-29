import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// One firmware image offered by a GitHub release.
public struct FirmwareAsset: Sendable, Equatable, Identifiable, Hashable {
    public let tag: String
    public let releaseName: String
    public let prerelease: Bool
    public let name: String
    public let size: Int
    public let url: URL
    /// The asset's SHA-256 (lowercase hex) when GitHub provides `digest`
    /// (`"sha256:…"`); nil for older assets.
    public let sha256: String?
    public let publishedAt: Date?

    public var id: String { url.absoluteString }

    public init(tag: String, releaseName: String, prerelease: Bool, name: String, size: Int, url: URL,
                sha256: String?, publishedAt: Date?) {
        self.tag = tag
        self.releaseName = releaseName
        self.prerelease = prerelease
        self.name = name
        self.size = size
        self.url = url
        self.sha256 = sha256
        self.publishedAt = publishedAt
    }
}

public enum FirmwareReleaseError: Error, Equatable, Sendable {
    case badRepo
    case http(Int)
    case badResponse
    /// The download's SHA-256 is not the asset's `digest`.
    case digestMismatch(expected: String, actual: String)
    case sizeMismatch(expected: Int, actual: Int)
}

/// The Firmware screen's GitHub side: list a public repo's release assets
/// ending in `.bin` (`GET /repos/{owner}/{repo}/releases`, no token) and
/// download one, checked against its `digest` when GitHub has one.
public enum FirmwareReleases {
    public static let apiBase = URL(string: "https://api.github.com")!

    /// `owner/name` → (owner, name), both non-empty and URL-path safe.
    public static func parseRepo(_ text: String) -> (owner: String, repo: String)? {
        let parts = text.trimmingCharacters(in: .whitespaces).split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2 else { return nil }
        let ok = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        for part in parts where part.isEmpty || part.unicodeScalars.contains(where: { !ok.contains($0) }) {
            return nil
        }
        return (String(parts[0]), String(parts[1]))
    }

    /// Every `.bin` asset of every published release, newest release first
    /// (drafts skipped).
    public static func parse(_ data: Data) throws -> [FirmwareAsset] {
        struct Release: Decodable {
            struct Asset: Decodable {
                let name: String
                let size: Int
                let browser_download_url: String
                let digest: String?
            }
            let tag_name: String
            let name: String?
            let draft: Bool?
            let prerelease: Bool?
            let published_at: String?
            let assets: [Asset]
        }
        let releases: [Release]
        do {
            releases = try JSONDecoder().decode([Release].self, from: data)
        } catch {
            throw FirmwareReleaseError.badResponse
        }
        let iso = ISO8601DateFormatter()
        var out: [FirmwareAsset] = []
        for release in releases where release.draft != true {
            for asset in release.assets where asset.name.lowercased().hasSuffix(".bin") {
                guard let url = URL(string: asset.browser_download_url) else { continue }
                var sha: String?
                if let digest = asset.digest?.lowercased(), digest.hasPrefix("sha256:") {
                    let hex = String(digest.dropFirst(7))
                    if hex.count == 64, hex.allSatisfy(\.isHexDigit) { sha = hex }
                }
                out.append(FirmwareAsset(
                    tag: release.tag_name, releaseName: release.name ?? release.tag_name,
                    prerelease: release.prerelease ?? false, name: asset.name, size: asset.size, url: url,
                    sha256: sha, publishedAt: release.published_at.flatMap { iso.date(from: $0) }))
            }
        }
        return out
    }

    public static func fetch(repo: String, urlSession: URLSession = .shared) async throws -> [FirmwareAsset] {
        guard let (owner, name) = parseRepo(repo) else { throw FirmwareReleaseError.badRepo }
        let url = apiBase.appendingPathComponent("repos/\(owner)/\(name)/releases")
        var request = try HTTP.request(url: url, timeout: 15)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let reply = try await HTTP.send(request, using: urlSession)
        guard reply.status == 200 else { throw FirmwareReleaseError.http(reply.status) }
        return try parse(reply.body)
    }

    /// Download `asset`; its size and (when known) SHA-256 must match.
    public static func download(_ asset: FirmwareAsset, urlSession: URLSession = .shared) async throws -> [UInt8] {
        var request = try HTTP.request(url: asset.url, timeout: 120)
        request.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
        let reply = try await HTTP.send(request, using: urlSession)
        guard reply.status == 200 else { throw FirmwareReleaseError.http(reply.status) }
        let bytes = [UInt8](reply.body)
        guard bytes.count == asset.size else {
            throw FirmwareReleaseError.sizeMismatch(expected: asset.size, actual: bytes.count)
        }
        if let expected = asset.sha256 {
            let actual = OTAService.sha256Hex(bytes)
            guard actual == expected else { throw FirmwareReleaseError.digestMismatch(expected: expected, actual: actual) }
        }
        return bytes
    }
}

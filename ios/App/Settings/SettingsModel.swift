import Combine
import Foundation
import PagerKit

/// The Settings tab's state (IOS_SETTINGS_PLAN.md §4.4), owned by
/// `PagerSession`, attached in `bringUp`. All catalog and value state lives in
/// `SettingsStore` (PagerKit); this only binds it to the link, republishes it to
/// SwiftUI, and logs ids. Offline it shows the last values read this run,
/// read-only.
@MainActor
final class SettingsModel: ObservableObject {
    /// The pager's catalog and values, mirrored from the link.
    let store = SettingsStore()

    /// Whether the tab is worth showing: the pager advertised `"settings"` or a
    /// catalog from an earlier link is still cached.
    @Published private(set) var available = false

    /// Session-log sink (setting ids only, never values).
    var log: (String) -> Void = { _ in }

    private var service: SettingsService?

    init() {
        store.onChange = { [weak self] in self?.objectWillChange.send() }
    }

    // MARK: - Session wiring (PagerSession)

    /// Bind the link. A pager without the `"settings"` cap gets no tab unless a
    /// catalog was cached earlier (then it stays readable, read-only).
    func attach(_ session: LinkSession, pagerCaps: [String]) async {
        available = pagerCaps.contains(SettingsService.cap) || store.schema != nil
        guard pagerCaps.contains(SettingsService.cap) else { return }
        let service = SettingsService(session: session, cachedSchema: store.schema)
        self.service = service
        await store.attach(service)
        log("settings: \(store.schema?.items.count ?? 0) items, rev \(store.rev)")
    }

    func detach() {
        service = nil
        store.detach()
    }

    /// Feed every pager event here (the session owns the one events stream).
    func handleEvent(name: String, params: JSONValue) async {
        guard let service else { return }
        if let change = await service.handleEvent(name: name, params: params) {
            log("settings changed on the pager: \(change.ids.joined(separator: ", "))")
            await store.pagerChanged(change)
        }
    }

    // MARK: - Actions

    /// Validate and send one value; true once the pager accepted it.
    func commit(_ id: String, _ value: SettingValue) async -> Bool {
        log("settings.set \(id)")
        return await store.commit(id, value)
    }

    /// Run a one-shot command and, for a long one, wait for its job.
    func run(_ action: String, params: [String: JSONValue] = [:]) async throws -> JSONValue {
        guard let service else {
            throw RPCError(code: "disconnected", msg: "not connected")
        }
        log("settings.action \(action)")
        return try await service.run(action, params: params)
    }

    /// Write the notes repo, branch and token to the pager's `nsync.*` settings,
    /// in that order, stopping at the first refusal. The token is sent, never
    /// logged.
    func copyGitHub(_ config: GitHubNotesConfig) async -> Bool {
        guard await commit("nsync.repo", .string(config.repo)) else { return false }
        guard await commit("nsync.branch", .string(config.branch)) else { return false }
        return await commit("nsync.token", .string(config.token))
    }
}

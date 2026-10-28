import Foundation

/// The UI-free state behind the app's Settings tab (IOS_SETTINGS_PLAN.md W9).
///
/// Rules:
/// - The pager owns both the catalog and the values; this store only mirrors
///   them. `attach` binds a backend (the link) and loads everything; `detach`
///   drops it but keeps the last schema and values, so the tab still renders
///   read-only while offline.
/// - An edit starts as a draft: `beginEdit`/`draft` stage it without touching
///   the pager, `cancelEdit` drops it, and `commit` first validates it against
///   the schema, then sends it with the last known `rev`. `value(_:)` is what
///   the UI shows: the draft when there is one, else the base value.
/// - A stale `rev` is not thrown away: the store reloads, and if the pager
///   changed only OTHER settings it retries the write once. If the pager
///   changed the same setting, the user's edit is refused and `lastError` says
///   so (their value is never silently overwritten).
/// - Pager-initiated changes (`pagerChanged`) update the base values and `rev`.
///   An id the user is editing keeps its draft: only the base value moves, so
///   the user's in-progress text is not dropped.
/// - `onChange` runs after every state change; the app's ObservableObject
///   republishes from it.
@MainActor
public final class SettingsStore {
    // MARK: - Observable state

    /// The pager's catalog, nil until the first load.
    public private(set) var schema: SettingsSchema?
    /// Base values as last read from or written to the pager.
    public private(set) var values: [String: SettingValue] = [:]
    /// The `rev` every write is conditional on (`if_rev`).
    public private(set) var rev: UInt32 = 0
    /// Whether a backend is attached (the link is up).
    public private(set) var connected = false
    /// Local edits not yet committed, keyed by setting id.
    public private(set) var drafts: [String: SettingValue] = [:]
    /// Settings whose `commit` is in flight.
    public private(set) var committing: Set<String> = []
    /// A committed setting asked the pager to reboot (`reboot: true`).
    public private(set) var rebootNeeded = false
    /// The last failure, already human-readable (nil when all is well).
    public private(set) var lastError: String?

    /// Called after every state change; the app republishes from it.
    public var onChange: (() -> Void)?

    // MARK: - Private state

    private var backend: (any SettingsBackend)?
    /// Ids the user has started editing (even before a draft value exists).
    private var editing: Set<String> = []

    public init() {}

    // MARK: - Connection

    /// Bind the link and load the catalog and values. A load failure leaves the
    /// store attached with `lastError` set.
    public func attach(_ backend: any SettingsBackend) async {
        self.backend = backend
        connected = true
        await reload()
    }

    /// Drop the link. The schema and values stay so the tab remains readable
    /// (read-only); edits in flight are discarded.
    public func detach() {
        backend = nil
        connected = false
        editing.removeAll()
        drafts.removeAll()
        committing.removeAll()
        changed()
    }

    /// Whether a value may be written: attached and with a catalog to validate
    /// against.
    public var canEdit: Bool { connected && schema != nil }

    // MARK: - Loading

    /// Read every value, fetching the catalog first when its revision is
    /// unknown or changed. Values are read a second time with the catalog in
    /// hand so each one decodes by its item's type (a secret's `{"set": …}`,
    /// an enum's string or integer).
    public func reload() async {
        guard let backend else { return }
        do {
            var snapshot = try await backend.get(ids: nil)
            if schema == nil || schema?.revSchema != snapshot.revSchema {
                schema = try await backend.schema(expected: snapshot.revSchema)
                snapshot = try await backend.get(ids: nil)
            }
            values = snapshot.values
            rev = snapshot.rev
            lastError = nil
        } catch {
            lastError = String(describing: error)
        }
        changed()
    }

    // MARK: - Editing

    /// The value the UI shows for `id`: the draft when there is one.
    public func value(_ id: String) -> SettingValue? { drafts[id] ?? values[id] }

    /// Mark `id` as being edited (the row becomes an editor).
    public func beginEdit(_ id: String) {
        editing.insert(id)
        changed()
    }

    /// Stage a local value for `id`; nothing is sent until `commit`.
    public func draft(_ id: String, _ value: SettingValue) {
        editing.insert(id)
        drafts[id] = value
        changed()
    }

    /// Drop the draft for `id` and leave the editor.
    public func cancelEdit(_ id: String) {
        editing.remove(id)
        drafts.removeValue(forKey: id)
        changed()
    }

    /// Validate and send one value. Returns true once the pager accepted it.
    ///
    /// A stale `if_rev` triggers a reload and one retry; the retry is refused
    /// when the pager changed this same setting in the meantime. Every failure
    /// lands in `lastError`.
    @discardableResult
    public func commit(_ id: String, _ value: SettingValue) async -> Bool {
        guard canEdit, let backend, let item = schema?.item(id) else { return false }
        if let problem = item.problem(with: value) {
            lastError = "\(item.title): \(problem)"
            changed()
            return false
        }
        committing.insert(id)
        changed()
        defer {
            committing.remove(id)
            editing.remove(id)
            drafts.removeValue(forKey: id)
            changed()
        }
        do {
            let result = try await backend.set([id: value], ifRev: rev)
            apply(result, id: id, item: item, value: value)
            return true
        } catch let error as SettingsError where error == .changed {
            let before = values[id]
            await reload()
            if before != values[id] {
                lastError = "\(item.title) was changed on the pager"
                return false
            }
            do {
                let result = try await backend.set([id: value], ifRev: rev)
                apply(result, id: id, item: item, value: value)
                return true
            } catch {
                lastError = message(for: error, id: id, item: item)
                return false
            }
        } catch {
            lastError = message(for: error, id: id, item: item)
            return false
        }
    }

    /// Fold in a pager-initiated change (`evt.settings.changed`). Our own write
    /// is ignored by revision; a changed catalog reloads everything.
    public func pagerChanged(_ change: SettingsChanged) async {
        if change.rev == rev { return }  // our own write echoing back
        guard let backend else { return }
        let snapshot: SettingsSnapshot
        do {
            snapshot = try await backend.get(ids: change.ids)
        } catch {
            await reload()
            return
        }
        if snapshot.revSchema != schema?.revSchema {
            await reload()
            return
        }
        for id in change.ids {
            if let value = snapshot.values[id] {
                // Only the base value moves: an id in `editing` keeps its draft,
                // so the user's in-progress edit is not dropped.
                values[id] = value
            } else {
                values.removeValue(forKey: id)
            }
        }
        rev = snapshot.rev
        changed()
    }

    // MARK: - Presentation

    /// Schema groups that still have something to show (an item or an action),
    /// in catalog order.
    public func groups() -> [SettingGroup] {
        guard let schema else { return [] }
        return schema.groups.filter { group in
            schema.items.contains { $0.group == group.id }
                || schema.actions.contains { $0.group == group.id }
        }
    }

    // MARK: - Internals

    /// The success tail shared by the first write and the retry.
    private func apply(_ result: SettingsSetResult, id: String, item: SettingItem, value: SettingValue) {
        rev = result.rev
        rebootNeeded = rebootNeeded || result.reboot
        // A secret is write-only: remember whether one is set, not its text.
        values[id] = item.secret ? .secret(set: !(value.stringValue ?? "").isEmpty) : value
        lastError = nil
    }

    /// A human-readable failure; an `RPCError` usually repeats the setting id
    /// (`"display.timeout_s: out of range"`), so only prefix the title when it
    /// does not.
    private func message(for error: Error, id: String, item: SettingItem) -> String {
        if let rpc = error as? RPCError {
            return rpc.msg.hasPrefix(id) ? rpc.msg : "\(item.title): \(rpc.msg)"
        }
        return String(describing: error)
    }

    private func changed() {
        onChange?()
    }
}

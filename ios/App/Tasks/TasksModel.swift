import Foundation
import PagerKit

/// The Tasks tab's state: the pager's Tasks list (`/tasks.txt`, the
/// firmware's `ui_tasks.cpp`), editable from the phone. Every change is
/// written back at once, as the pager's own app does; a save is refused when
/// the pager's file changed since it was read, and the list then reloads.
///
/// Owned by `PagerSession`, which attaches the live link in `bringUp` and
/// detaches it in `disconnect`. Nothing is cached: offline, the tab shows the
/// last list read this run, read-only.
@MainActor
final class TasksModel: ObservableObject {
    @Published private(set) var items: [TaskItem] = []
    @Published private(set) var connected = false
    /// The pager advertised `"fs"` (the Tasks tab needs it).
    @Published private(set) var pagerHasFS = false
    @Published private(set) var loaded = false
    @Published private(set) var loading = false
    @Published private(set) var saving = false
    @Published var errorText: String?

    private var service: TasksService?
    /// CRC-32 of the file last read or written (nil: none on the pager).
    private var base: UInt32?
    /// A change arrived while a save was in flight: save again after it.
    private var resave = false
    /// Session-log sink (counts only, never task text).
    var log: (String) -> Void = { _ in }

    var canEdit: Bool { connected && pagerHasFS && loaded }

    // MARK: - Session wiring (PagerSession)

    func attach(_ session: LinkSession, pagerCaps: [String]) async {
        pagerHasFS = pagerCaps.contains("fs")
        connected = true
        guard pagerHasFS else { return }
        service = TasksService(session: session)
        await reload()
    }

    func detach() {
        service = nil
        connected = false
    }

    /// `evt.fs.changed`: reread unless the phone is mid-save (its own write
    /// raised the event; the save sets the new base).
    func fsChanged() {
        guard !saving else { return }
        Task { await reload(quiet: true) }
    }

    // MARK: - Loading

    /// Read the list from the pager. `quiet` keeps the current list (and its
    /// row identities) when the file did not change.
    func reload(quiet: Bool = false) async {
        guard let service else { return }
        loading = true
        defer { loading = false }
        do {
            let snapshot = try await service.load()
            if !(quiet && loaded && snapshot.base == base) {
                items = snapshot.items
            }
            base = snapshot.base
            loaded = true
            errorText = nil
        } catch {
            errorText = "Load failed: \(NotesModel.describe(error))"
        }
    }

    // MARK: - Editing (each change is saved at once)

    func toggle(_ id: TaskItem.ID) {
        change { items in
            guard let index = items.firstIndex(where: { $0.id == id }), items[index].isTask else { return }
            items[index].checked.toggle()
        }
    }

    func add(_ text: String) {
        let trimmed = Self.clean(text)
        guard !trimmed.isEmpty else { return }
        change { $0.append(TaskItem(text: trimmed)) }
    }

    func rename(_ id: TaskItem.ID, to text: String) {
        let trimmed = Self.clean(text)
        guard !trimmed.isEmpty else { return }
        change { items in
            guard let index = items.firstIndex(where: { $0.id == id }) else { return }
            items[index].text = trimmed
        }
    }

    func delete(at offsets: IndexSet) {
        change { items in
            for index in offsets.sorted(by: >) { items.remove(at: index) }
        }
    }

    func move(from source: IndexSet, to destination: Int) {
        change { items in
            let moving = source.sorted().map { items[$0] }
            for index in source.sorted(by: >) { items.remove(at: index) }
            let shift = source.filter { $0 < destination }.count
            items.insert(contentsOf: moving, at: destination - shift)
        }
    }

    /// Drop every done task (plain lines stay).
    func clearDone() {
        change { $0.removeAll { $0.isTask && $0.checked } }
    }

    var doneCount: Int { items.filter { $0.isTask && $0.checked }.count }

    private func change(_ edit: (inout [TaskItem]) -> Void) {
        guard canEdit else { return }
        edit(&items)
        Task { await persist() }
    }

    /// Write `items`, coalescing changes made while a save runs.
    private func persist() async {
        guard let service else { return }
        if saving {
            resave = true
            return
        }
        saving = true
        defer { saving = false }
        repeat {
            resave = false
            do {
                let snapshot = try await service.save(items, base: base)
                base = snapshot.base
                errorText = nil
                log("tasks: saved \(snapshot.items.count) items")
            } catch {
                errorText = "Save failed: \(NotesModel.describe(error))"
                resave = false
                saving = false
                await reload()
                return
            }
        } while resave
    }

    /// One line of text: the pager stores one item per line.
    private static func clean(_ text: String) -> String {
        text.replacingOccurrences(of: "\r", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespaces)
    }
}

import PagerKit
import SwiftUI

/// The pager's Tasks list: tap to check off, swipe to delete, Edit to
/// reorder, the field at the bottom to add. Every change is saved to the
/// pager at once.
struct TasksView: View {
    @EnvironmentObject private var tasks: TasksModel

    @State private var newTask = ""
    @State private var renaming: TaskItem?
    @State private var renameText = ""
    @FocusState private var addFocused: Bool

    var body: some View {
        NavigationStack {
            List {
                statusSection
                itemsSection
                if tasks.canEdit { addSection }
            }
            .navigationTitle("Tasks")
            .refreshable { await tasks.reload() }
            .toolbar {
                if tasks.canEdit {
                    ToolbarItem(placement: .navigationBarLeading) {
                        if tasks.doneCount > 0 {
                            Button("Clear done") { tasks.clearDone() }
                        }
                    }
                    ToolbarItem(placement: .navigationBarTrailing) {
                        EditButton()
                    }
                }
                if tasks.saving || tasks.loading {
                    ToolbarItem(placement: .principal) { ProgressView() }
                }
            }
            .alert("Rename", isPresented: Binding(
                get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Task", text: $renameText)
                Button("Save") {
                    if let item = renaming { tasks.rename(item.id, to: renameText) }
                }
                Button("Cancel", role: .cancel) {}
            }
        }
    }

    @ViewBuilder
    private var statusSection: some View {
        if let error = tasks.errorText {
            Section {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }
        }
        if !tasks.connected && tasks.loaded {
            Section {
                Label("Offline: showing the last list read, read-only", systemImage: "wifi.slash")
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private var itemsSection: some View {
        if tasks.items.isEmpty {
            Section {
                Text(emptyText).foregroundStyle(.secondary)
            }
        } else {
            Section {
                ForEach(tasks.items) { item in
                    row(item)
                }
                .onDelete(perform: tasks.canEdit ? { tasks.delete(at: $0) } : nil)
                .onMove(perform: tasks.canEdit ? { tasks.move(from: $0, to: $1) } : nil)
            } footer: {
                let open = tasks.items.filter { $0.isTask && !$0.checked }.count
                Text("\(open) open · \(tasks.doneCount) done")
            }
        }
    }

    private var emptyText: String {
        if !tasks.connected { return "Connect to the pager to see its tasks." }
        if !tasks.loaded { return "Loading…" }
        return "No tasks on the pager."
    }

    @ViewBuilder
    private func row(_ item: TaskItem) -> some View {
        if item.isTask {
            Button {
                tasks.toggle(item.id)
            } label: {
                HStack(spacing: 12) {
                    Image(systemName: item.checked ? "checkmark.circle.fill" : "circle")
                        .foregroundStyle(item.checked ? Color.accentColor : .secondary)
                        .imageScale(.large)
                    Text(item.text)
                        .strikethrough(item.checked)
                        .foregroundStyle(item.checked ? .secondary : .primary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!tasks.canEdit)
            .contextMenu { renameButton(item) }
        } else {
            // A plain line the pager keeps verbatim (a heading or note).
            Text(item.text)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .contextMenu { renameButton(item) }
        }
    }

    @ViewBuilder
    private func renameButton(_ item: TaskItem) -> some View {
        if tasks.canEdit {
            Button {
                renameText = item.text
                renaming = item
            } label: {
                Label("Rename", systemImage: "pencil")
            }
        }
    }

    private var addSection: some View {
        Section {
            HStack {
                TextField("New task", text: $newTask)
                    .focused($addFocused)
                    .submitLabel(.done)
                    .onSubmit(add)
                Button(action: add) {
                    Image(systemName: "plus.circle.fill").imageScale(.large)
                }
                .disabled(newTask.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
    }

    private func add() {
        tasks.add(newTask)
        newTask = ""
        addFocused = true
    }
}

#Preview {
    TasksView()
        .environmentObject(PagerSession().tasks)
}

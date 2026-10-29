import PagerKit
import SwiftUI

/// The pager's Journal on the phone (S9): the same notes as the Notes tab,
/// grouped by the day in their names the way the pager's Journal titles them
/// (`ui_journal.cpp`), a calendar to jump to one day, each entry opening in
/// the note viewer, and "Append to today", which extends today's newest
/// entry or starts today's first one, written with `fs.write` like any note
/// edit. Encrypted entries stay locked until the notes are unlocked.
struct JournalView: View {
    @EnvironmentObject private var notes: NotesModel

    @State private var showCalendar = false
    @State private var day = Date()
    @State private var filterByDay = false
    @State private var showAppend = false
    @State private var showUnlock = false

    var body: some View {
        NavigationStack {
            List {
                if showCalendar {
                    Section {
                        DatePicker("Day", selection: $day, displayedComponents: .date)
                            .datePickerStyle(.graphical)
                        Toggle("Only this day", isOn: $filterByDay)
                    }
                }
                if notes.rows.contains(where: \.locked) {
                    Section {
                        Button {
                            showUnlock = true
                        } label: {
                            Label("Unlock notes to read encrypted entries", systemImage: "lock.open")
                        }
                    }
                }
                ForEach(days) { day in
                    Section {
                        ForEach(day.entries, id: \.id) { entry in
                            if let row = notes.rows.first(where: { $0.id == entry.id }) {
                                NavigationLink(value: row.id) {
                                    JournalRow(row: row)
                                }
                            }
                        }
                    } header: {
                        Text(dayTitle(day))
                    }
                }
                if !filterByDay, !grouped.undated.isEmpty {
                    Section("Other notes") {
                        ForEach(grouped.undated, id: \.id) { entry in
                            if let row = notes.rows.first(where: { $0.id == entry.id }) {
                                NavigationLink(value: row.id) {
                                    JournalRow(row: row)
                                }
                            }
                        }
                    }
                }
                if days.isEmpty && (filterByDay || grouped.undated.isEmpty) {
                    Text(filterByDay ? "No entries that day." : "No journal entries yet.")
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Journal")
            .refreshable { await notes.refresh() }
            .navigationDestination(for: String.self) { id in
                if let row = notes.rows.first(where: { $0.id == id }) {
                    NoteDetailView(row: row)
                }
            }
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button {
                        withAnimation { showCalendar.toggle() }
                    } label: {
                        Label("Calendar", systemImage: showCalendar ? "calendar.circle.fill" : "calendar")
                    }
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        showAppend = true
                    } label: {
                        Label("Append to today", systemImage: "square.and.pencil")
                    }
                    .disabled(!notes.canCreate)
                }
            }
            .sheet(isPresented: $showAppend) {
                AppendToTodayView()
                    .environmentObject(notes)
            }
            .sheet(isPresented: $showUnlock) {
                UnlockNotesView()
                    .environmentObject(notes)
            }
        }
    }

    private var grouped: Journal.Grouped { Journal.group(notes.rows.map(\.entry)) }

    private var days: [Journal.Day] {
        let all = grouped.days
        guard filterByDay else { return all }
        let c = Calendar.current.dateComponents([.year, .month, .day], from: day)
        return all.filter { $0.year == c.year && $0.month == c.month && $0.day == c.day }
    }

    private func dayTitle(_ day: Journal.Day) -> String {
        guard let date = Calendar.current.date(from: day.components) else { return day.id }
        let count = day.entries.count
        return date.formatted(date: .complete, time: .omitted) + (count > 1 ? " · \(count)" : "")
    }
}

/// One entry: its time (from the name) and the first line, or a lock.
private struct JournalRow: View {
    let row: NotesModel.Row

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(time)
                .font(.subheadline.weight(.semibold))
            if row.locked {
                Label("Encrypted", systemImage: "lock.fill")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Text(row.preview ?? " ")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
        }
    }

    private var time: String {
        guard let t = Journal.timestamp(ofNoteName: row.entry.name), let h = t.hour, let m = t.minute else {
            return row.entry.name
        }
        return String(format: "%02d:%02d", h, m)
    }
}

/// "Append to today": a text editor whose content is added to today's
/// newest entry (or becomes today's first one).
private struct AppendToTodayView: View {
    @EnvironmentObject private var notes: NotesModel
    @Environment(\.dismiss) private var dismiss

    @State private var text = ""
    @State private var saving = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextEditor(text: $text)
                        .frame(minHeight: 180)
                } footer: {
                    Text(footer)
                }
                if let error {
                    Section {
                        Text(error)
                            .foregroundStyle(.red)
                            .font(.footnote)
                    }
                }
            }
            .navigationTitle("Append to today")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if saving {
                        ProgressView()
                    } else {
                        Button("Save") { save() }
                            .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
            }
            .interactiveDismissDisabled(saving)
        }
    }

    private var footer: String {
        let today = Calendar.current.dateComponents([.year, .month, .day], from: Date())
        if let entry = Journal.todayEntry(notes.rows.map(\.entry), today: today) {
            return "Added to \(Journal.title(ofNoteName: entry.name)) on the pager."
        }
        return "Starts today's first entry on the pager."
    }

    private func save() {
        saving = true
        error = nil
        Task {
            do {
                try await notes.appendToToday(text)
                dismiss()
            } catch {
                self.error = "Save failed: \(NotesModel.describe(error))"
            }
            saving = false
        }
    }
}

#Preview {
    JournalView()
        .environmentObject(PagerSession().notes)
}

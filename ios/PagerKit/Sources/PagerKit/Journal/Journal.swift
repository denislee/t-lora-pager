import Foundation

/// The pager Journal's view of notes, for the phone's Journal tab (S9).
///
/// The pager's Journal (`src/apps/ui_journal.cpp`) lists every note
/// (`NotesIndexer`'s merged list, name descending) and titles each with the
/// timestamp in its name: the text editor saves a new note as
/// `/notes/YYYYMMDD_HHMMSS.txt` in local time (`ui_text_editor.cpp`), and
/// `parse_filename_to_human` reads the first 15 characters of the leaf back.
/// A name that does not parse keeps its file name as the title; here it goes
/// to `undated`.
public enum Journal {
    /// One calendar day of entries.
    public struct Day: Sendable, Equatable, Identifiable {
        public let year: Int
        public let month: Int
        public let day: Int
        /// Newest first (Journal order).
        public var entries: [NoteEntry]

        /// `"YYYY-MM-DD"`.
        public var id: String { String(format: "%04d-%02d-%02d", year, month, day) }

        public init(year: Int, month: Int, day: Int, entries: [NoteEntry]) {
            self.year = year
            self.month = month
            self.day = day
            self.entries = entries
        }

        /// The day as components (for `Calendar.date(from:)`).
        public var components: DateComponents { DateComponents(year: year, month: month, day: day) }
    }

    /// The days with entries (newest day first) and the notes whose name has
    /// no timestamp.
    public struct Grouped: Sendable, Equatable {
        public var days: [Day]
        public var undated: [NoteEntry]
    }

    /// The local timestamp in a note's leaf name: `YYYYMMDD_HHMMSS` as its
    /// first 15 characters (anything may follow, e.g. `_2.txt`), with a valid
    /// month, day, hour, minute and second. Nil otherwise.
    public static func timestamp(ofNoteName name: String) -> DateComponents? {
        let chars = Array(name.utf8)
        guard chars.count >= 15, chars[8] == UInt8(ascii: "_") else { return nil }
        func number(_ range: Range<Int>) -> Int? {
            var value = 0
            for i in range {
                let c = chars[i]
                guard c >= UInt8(ascii: "0"), c <= UInt8(ascii: "9") else { return nil }
                value = value * 10 + Int(c - UInt8(ascii: "0"))
            }
            return value
        }
        guard let y = number(0..<4), let mo = number(4..<6), let d = number(6..<8),
              let h = number(9..<11), let mi = number(11..<13), let s = number(13..<15),
              (1...12).contains(mo), (1...31).contains(d), h < 24, mi < 60, s < 60 else { return nil }
        return DateComponents(year: y, month: mo, day: d, hour: h, minute: mi, second: s)
    }

    /// The leaf name the pager's editor would give a note saved at `date`
    /// in `calendar`'s zone: `YYYYMMDD_HHMMSS.txt`.
    public static func noteName(for date: Date, calendar: Calendar = .current) -> String {
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: date)
        return String(format: "%04d%02d%02d_%02d%02d%02d.txt",
                      c.year ?? 0, c.month ?? 0, c.day ?? 0, c.hour ?? 0, c.minute ?? 0, c.second ?? 0)
    }

    /// `noteName`, with `_2`, `_3`, … before `.txt` until `taken` says no
    /// (the title still parses: only the first 15 characters count).
    public static func uniqueNoteName(for date: Date, calendar: Calendar = .current,
                                      taken: (String) -> Bool) -> String {
        let base = noteName(for: date, calendar: calendar)
        guard taken(base) else { return base }
        let stem = String(base.dropLast(4))
        var n = 2
        while taken("\(stem)_\(n).txt") { n += 1 }
        return "\(stem)_\(n).txt"
    }

    /// The pager's title for an entry: `"Apr 18, 2026 - 12:05:30"`, or the
    /// file name when it has no timestamp.
    public static func title(ofNoteName name: String) -> String {
        guard let t = timestamp(ofNoteName: name), let y = t.year, let mo = t.month, let d = t.day,
              let h = t.hour, let mi = t.minute, let s = t.second else { return name }
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        return months[mo - 1] + String(format: " %02d, %04d - %02d:%02d:%02d", d, y, h, mi, s)
    }

    /// Group `entries` by the day in their names; days newest first, each
    /// day's entries in Journal order (name descending).
    public static func group(_ entries: [NoteEntry]) -> Grouped {
        var byDay: [String: Day] = [:]
        var undated: [NoteEntry] = []
        for entry in entries {
            guard let t = timestamp(ofNoteName: entry.name), let y = t.year, let m = t.month, let d = t.day else {
                undated.append(entry)
                continue
            }
            let day = Day(year: y, month: m, day: d, entries: [])
            byDay[day.id, default: day].entries.append(entry)
        }
        let days = byDay.values
            .map { day -> Day in
                var sorted = day
                sorted.entries.sort(by: NotesIndexer.journalOrder)
                return sorted
            }
            .sorted { $0.id > $1.id }
        return Grouped(days: days, undated: undated.sorted(by: NotesIndexer.journalOrder))
    }

    /// The entry "Append to today" extends: the newest entry dated `today`
    /// (year, month, day), or nil when today has none yet.
    public static func todayEntry(_ entries: [NoteEntry], today: DateComponents) -> NoteEntry? {
        entries
            .filter { entry in
                guard let t = timestamp(ofNoteName: entry.name) else { return false }
                return t.year == today.year && t.month == today.month && t.day == today.day
            }
            .sorted(by: NotesIndexer.journalOrder)
            .first
    }

    /// `existing` with `addition` appended as a new paragraph: trailing
    /// whitespace of `existing` trimmed, one blank line, then `addition`
    /// (trimmed) and a final newline. An empty `existing` is just `addition`.
    public static func appending(_ addition: String, to existing: String) -> String {
        let add = addition.trimmingCharacters(in: .whitespacesAndNewlines)
        var head = existing
        while let last = head.unicodeScalars.last, CharacterSet.whitespacesAndNewlines.contains(last) {
            head.unicodeScalars.removeLast()
        }
        if head.isEmpty { return add + "\n" }
        return head + "\n\n" + add + "\n"
    }
}

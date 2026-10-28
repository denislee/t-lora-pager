import Foundation

/// One line of the pager's Tasks list (`src/apps/ui_tasks.cpp`): a task with
/// a checkbox, or a plain line kept verbatim.
public struct TaskItem: Sendable, Equatable, Hashable, Identifiable {
    public var id: UUID
    public var isTask: Bool
    public var checked: Bool
    public var text: String

    public init(id: UUID = UUID(), isTask: Bool = true, checked: Bool = false, text: String) {
        self.id = id
        self.isTask = isTask
        self.checked = checked
        self.text = text
    }
}


/// The Tasks app's file format, byte for byte what the firmware reads and
/// writes: one item per line ("\n" or "\r\n", the last may lack it), blank
/// lines dropped; "[ ] text" is an open task, "[x] text" / "[X] text" a done
/// one, anything else a plain line. Saving writes "[ ] " / "[x] " + text, or
/// the plain text, each followed by "\n".
public enum TasksFile {
    /// The file at the internal root (the pager mirrors it to the SD root).
    public static let path = "/tasks.txt"

    public static func parse(_ content: String) -> [TaskItem] {
        var items: [TaskItem] = []
        // Split on bytes: Swift reads "\r\n" as one Character, so a
        // Character split on "\n" would miss CRLF line ends.
        for raw in Array(content.utf8).split(separator: 0x0A, omittingEmptySubsequences: false) {
            var bytes = Array(raw)
            if bytes.last == 0x0D { bytes.removeLast() }
            guard !bytes.isEmpty else { continue }
            items.append(parseLine(String(decoding: bytes, as: UTF8.self)))
        }
        return items
    }

    static func parseLine(_ line: String) -> TaskItem {
        if line.hasPrefix("[ ] ") {
            return TaskItem(isTask: true, checked: false, text: String(line.dropFirst(4)))
        }
        if line.hasPrefix("[x] ") || line.hasPrefix("[X] ") {
            return TaskItem(isTask: true, checked: true, text: String(line.dropFirst(4)))
        }
        return TaskItem(isTask: false, checked: false, text: line)
    }

    public static func serialize(_ items: [TaskItem]) -> String {
        var out = ""
        for item in items {
            if item.isTask { out += item.checked ? "[x] " : "[ ] " }
            // A newline inside an item would split it into two on the pager.
            out += item.text.replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
            out += "\n"
        }
        return out
    }
}

/// Reads and writes the pager's `/tasks.txt` over the file service the way
/// its storage layer does: reads prefer the SD copy (`hw_read_file`,
/// SD first), saves go to internal and, when the card is in, to SD
/// (`hw_save_preferred_file`). A missing file is an empty list.
public struct TasksService: Sendable {
    public struct Snapshot: Sendable, Equatable {
        public var items: [TaskItem]
        /// CRC-32 of the bytes read (nil: no file), the base of the next save.
        public var base: UInt32?
        public var sdMounted: Bool

        public init(items: [TaskItem], base: UInt32?, sdMounted: Bool) {
            self.items = items
            self.base = base
            self.sdMounted = sdMounted
        }
    }

    public enum TasksError: Error, Equatable, Sendable {
        /// The pager's file changed since it was read; reload and retry.
        case changedOnPager
    }

    public let session: LinkSession
    public let timeout: Duration

    public init(session: LinkSession, timeout: Duration = .seconds(30)) {
        self.session = session
        self.timeout = timeout
    }

    /// The pager's tasks, from the copy the pager itself reads.
    public func load() async throws -> Snapshot {
        let (bytes, sdMounted) = try await current()
        guard let bytes else { return Snapshot(items: [], base: nil, sdMounted: sdMounted) }
        return Snapshot(items: TasksFile.parse(String(decoding: bytes, as: UTF8.self)),
                        base: CRC32.checksum(bytes), sdMounted: sdMounted)
    }

    /// Save `items` if the pager's file is still the one `base` describes
    /// (nil: there was none); otherwise throw `changedOnPager` and write
    /// nothing. Returns the new snapshot.
    public func save(_ items: [TaskItem], base: UInt32?) async throws -> Snapshot {
        let (bytes, sdMounted) = try await current()
        guard bytes.map({ CRC32.checksum($0) }) == base else { throw TasksError.changedOnPager }

        let data = Array(TasksFile.serialize(items).utf8)
        try await session.writeFile(vol: "int", path: TasksFile.path, data: data,
                                    overwrite: true, timeout: timeout)
        if sdMounted {
            try await session.writeFile(vol: "sd", path: TasksFile.path, data: data,
                                        overwrite: true, timeout: timeout)
        }
        return Snapshot(items: TasksFile.parse(String(decoding: data, as: UTF8.self)),
                        base: CRC32.checksum(data), sdMounted: sdMounted)
    }

    /// The bytes the pager reads (SD copy first, then internal; nil when
    /// neither exists) and whether the SD card is in.
    private func current() async throws -> ([UInt8]?, Bool) {
        let fs = FileService(session: session, timeout: timeout)
        var sdMounted = true
        do {
            _ = try await fs.stat(vol: "sd", path: TasksFile.path)
            let (bytes, _) = try await session.readFile(vol: "sd", path: TasksFile.path, timeout: timeout)
            return (bytes, true)
        } catch let error as RPCError where error.code == "invalid_argument" {
            sdMounted = !error.isSDNotMounted
        }
        do {
            _ = try await fs.stat(vol: "int", path: TasksFile.path)
        } catch let error as RPCError where error.code == "invalid_argument" && error.msg == "not found" {
            return (nil, sdMounted)
        }
        let (bytes, _) = try await session.readFile(vol: "int", path: TasksFile.path, timeout: timeout)
        return (bytes, sdMounted)
    }
}

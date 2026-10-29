/// The pager's `ssh.hosts` setting (a `string`, at most 16 entries): the saved
/// hosts the pager's Connect screen offers first, entries separated by `;` as
/// `name=user@host[:port]` — the `name=` part optional, the name defaulting to
/// the host. Each entry has its own user; all share the `ssh.key` /
/// `ssh.password` credentials.
///
/// Entries are trimmed of spaces and tabs; empty entries are skipped but still
/// counted for numbering. This is a port of
/// `hal::ssh::parse_hosts` / `format_host` (`src/hal/ssh_hosts.cpp`), pinned
/// by `test/test_ssh_hosts`: the limits (16 hosts, 32/64/128 bytes) and the
/// `entry N: <reason>` messages are the pager's own words, shown verbatim.
public enum SSHHostList {
    /// Hosts the list keeps.
    public static let maxHosts = 16

    /// Longest accepted name / user / host, in bytes.
    private static let nameMax = 32
    private static let userMax = 64
    private static let hostMax = 128

    /// One saved host. `name` is what the picker shows (defaults to `host`).
    public struct Host: Equatable, Hashable, Sendable {
        /// What the pager's Connect screen lists; the host when none was given.
        public var name: String
        /// The login user of this entry.
        public var user: String
        /// Host name or address (no space, tab, `/`, `@`, `:` or `;`).
        public var host: String
        /// TCP port; 22 when the entry gives none.
        public var port: Int

        public init(name: String, user: String, host: String, port: Int = 22) {
            self.name = name
            self.user = user
            self.host = host
            self.port = port
        }
    }

    /// Why the pager refused the text: its own message, `entry N: <reason>`.
    public struct ParseError: Error, Equatable, Sendable {
        public let message: String

        public init(message: String) {
            self.message = message
        }
    }

    /// Parses the pager's `ssh.hosts` text; `.failure` carries the pager's
    /// message. The first bad entry wins and no hosts come back.
    public static func parse(_ text: String) -> Result<[Host], ParseError> {
        var hosts: [Host] = []
        for (index, rawEntry) in entries(text).enumerated() {
            let number = index + 1
            let entry = trim(rawEntry)
            if entry.isEmpty { continue }

            var host = Host(name: "", user: "", host: "", port: 22)
            var rest = entry[...]
            var haveName = false
            if let eq = entry.firstIndex(of: equals) {
                host.name = makeString(trim(Array(entry[..<eq]))[...])
                rest = entry[(eq + 1)...]
                if host.name.isEmpty { return failure(number, "empty name") }
                if host.name.utf8.count > nameMax { return failure(number, "name too long") }
                if host.name.contains("@") { return failure(number, "bad name") }
                for scalar in host.name.unicodeScalars
                where scalar.value < 0x20 || scalar.value == 0x7F {
                    return failure(number, "bad name")
                }
                haveName = true
            }

            /* Exactly one '@' splits the user from the host. */
            let ats = rest.indices.filter { rest[$0] == atSign }
            guard ats.count == 1, let at = ats.first else {
                return failure(number, "missing user@")
            }
            host.user = makeString(rest[rest.startIndex..<at])
            if host.user.isEmpty || host.user.utf8.count > userMax {
                return failure(number, "bad user")
            }
            for scalar in host.user.unicodeScalars
            where scalar == space || scalar == tab || scalar == colon || scalar == semicolon
                || scalar.value < 0x20 || scalar.value == 0x7F {
                return failure(number, "bad user")
            }

            let hostport = rest[(at + 1)...]
            if let colonAt = hostport.lastIndex(of: colon) {
                let portText = hostport[(colonAt + 1)...]
                if portText.isEmpty { return failure(number, "bad port") }
                var value: UInt32 = 0
                for scalar in portText {
                    guard scalar.value >= 0x30, scalar.value <= 0x39 else {
                        return failure(number, "bad port")
                    }
                    value = value * 10 + (scalar.value - 0x30)
                    if value > 65535 { return failure(number, "bad port") }
                }
                if value < 1 { return failure(number, "bad port") }
                host.port = Int(value)
                host.host = makeString(hostport[hostport.startIndex..<colonAt])
            } else {
                host.host = makeString(hostport)
            }
            if host.host.isEmpty || host.host.utf8.count > hostMax {
                return failure(number, "bad host")
            }
            for scalar in host.host.unicodeScalars
            where scalar == space || scalar == tab || scalar == slash
                || scalar == atSign || scalar == colon || scalar == semicolon
                || scalar.value < 0x20 || scalar.value == 0x7F {
                return failure(number, "bad host")
            }

            if !haveName { host.name = host.host }

            if hosts.count >= maxHosts { return failure(number, "more than 16 hosts") }
            if hosts.contains(where: { $0.name == host.name }) {
                return failure(number, "duplicate name")
            }
            hosts.append(host)
        }
        return .success(hosts)
    }

    /// The canonical entry: `user@host` plus `:port` when `port != 22`, prefixed
    /// by `name=` when `name != host` — the pager's `format_host`.
    public static func line(_ h: Host) -> String {
        var s = h.name == h.host ? "" : h.name + "="
        s += h.user + "@" + h.host
        if h.port != 22 { s += ":" + String(h.port) }
        return s
    }

    /// `hosts` as `ssh.hosts` text: one `line` each, joined by `; `, with no
    /// trailing separator.
    public static func text(_ hosts: [Host]) -> String {
        hosts.map { line($0) }.joined(separator: "; ")
    }

    // MARK: - Internals

    private static let space: Unicode.Scalar = " "
    private static let tab: Unicode.Scalar = "\t"
    private static let equals: Unicode.Scalar = "="
    private static let atSign: Unicode.Scalar = "@"
    private static let colon: Unicode.Scalar = ":"
    private static let semicolon: Unicode.Scalar = ";"
    private static let slash: Unicode.Scalar = "/"

    private static func failure(_ entry: Int, _ reason: String) -> Result<[Host], ParseError> {
        .failure(ParseError(message: "entry \(entry): \(reason)"))
    }

    /// `text` split on `;` only; a trailing `;` yields a final empty entry.
    private static func entries(_ text: String) -> [[Unicode.Scalar]] {
        var entries: [[Unicode.Scalar]] = []
        var entry: [Unicode.Scalar] = []
        for scalar in text.unicodeScalars {
            if scalar == semicolon {
                entries.append(entry)
                entry = []
            } else {
                entry.append(scalar)
            }
        }
        entries.append(entry)
        return entries
    }

    /// `entry` without leading and trailing spaces and tabs (and nothing else).
    private static func trim(_ entry: [Unicode.Scalar]) -> [Unicode.Scalar] {
        var begin = 0, end = entry.count
        while begin < end, entry[begin] == space || entry[begin] == tab { begin += 1 }
        while end > begin, entry[end - 1] == space || entry[end - 1] == tab { end -= 1 }
        return Array(entry[begin..<end])
    }

    private static func makeString(_ scalars: ArraySlice<Unicode.Scalar>) -> String {
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars)
        return String(view)
    }
}

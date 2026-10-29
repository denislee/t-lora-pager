import XCTest

@testable import PagerKit

/// Covers `SSHHostList` — the Swift port of `hal::ssh::parse_hosts` and
/// `format_host`, case by case from `test/test_ssh_hosts/test_main.cpp`: the
/// defaults, the skipped entries, the limits, every rejection with the pager's
/// exact message, and the canonical entry round trip.
final class SSHHostListTests: XCTestCase {

    // MARK: - Helpers

    private func parse(_ text: String) -> [SSHHostList.Host] {
        switch SSHHostList.parse(text) {
        case .success(let hosts):
            return hosts
        case .failure(let error):
            XCTFail("unexpected \(error.message) for \(text.debugDescription)")
            return []
        }
    }

    /// The pager's message for a rejected text ("" when it parses).
    private func parseError(_ text: String) -> String {
        guard case .failure(let error) = SSHHostList.parse(text) else { return "" }
        return error.message
    }

    // MARK: - Parsing

    func testParseNamedHostWithPort() {
        let hosts = parse("lab=pi@10.0.0.2:2222")
        XCTAssertEqual(hosts.count, 1)
        XCTAssertEqual(hosts[0].name, "lab")
        XCTAssertEqual(hosts[0].user, "pi")
        XCTAssertEqual(hosts[0].host, "10.0.0.2")
        XCTAssertEqual(hosts[0].port, 2222)
    }

    func testParseDefaultNameAndPort() {
        let hosts = parse("me@example.com")
        XCTAssertEqual(hosts.count, 1)
        XCTAssertEqual(hosts[0].name, "example.com")
        XCTAssertEqual(hosts[0].user, "me")
        XCTAssertEqual(hosts[0].host, "example.com")
        XCTAssertEqual(hosts[0].port, 22)
    }

    func testParseSkipsBlankEntriesAndSpaces() {
        let text = "; \t lab=pi@10.0.0.2 ;  me@example.com\t; ;"
        let hosts = parse(text)
        XCTAssertEqual(hosts.count, 2)
        XCTAssertEqual(hosts[0].name, "lab")
        XCTAssertEqual(hosts[0].user, "pi")
        XCTAssertEqual(hosts[0].host, "10.0.0.2")
        XCTAssertEqual(hosts[1].name, "example.com")
    }

    func testNewlineInsideEntryIsBadHost() {
        XCTAssertEqual(parseError("u@h\nx"), "entry 1: bad host")
    }

    func testMissingUserAt() {
        // A bad entry leaves no hosts at all: the failure carries none.
        XCTAssertEqual(parseError("ok@a;bad"), "entry 2: missing user@")
    }

    func testDoubleAtFails() {
        XCTAssertEqual(parseError("u@@h"), "entry 1: missing user@")
    }

    func testBadPortZero() {
        XCTAssertEqual(parseError("u@h:0"), "entry 1: bad port")
    }

    func testBadPortTooLarge() {
        XCTAssertEqual(parseError("u@h:99999"), "entry 1: bad port")
    }

    func testBadPortNotANumber() {
        XCTAssertEqual(parseError("u@h:x"), "entry 1: bad port")
    }

    func testBadHost() {
        XCTAssertEqual(parseError("u@a/b"), "entry 1: bad host")
    }

    func testEmptyName() {
        XCTAssertEqual(parseError("=u@h"), "entry 1: empty name")
    }

    func testNameTooLong() {
        let name = String(repeating: "n", count: 33)
        XCTAssertEqual(parseError("\(name)=u@h"), "entry 1: name too long")
        XCTAssertEqual(parse("\(String(repeating: "n", count: 32))=u@h").count, 1)
    }

    func testNameWithAtFails() {
        XCTAssertEqual(parseError("a@b=u@h"), "entry 1: bad name")
    }

    func testTooManyHosts() {
        let text = (0..<17).map { "u@h\($0)" }.joined(separator: ";")
        XCTAssertEqual(parseError(text), "entry 17: more than 16 hosts")
    }

    func testDuplicateName() {
        XCTAssertEqual(parseError("a=p@x;b=p@y;a=p@z"), "entry 3: duplicate name")
    }

    func testDuplicateDefaultName() {
        XCTAssertEqual(parseError("u@h;v@h"), "entry 2: duplicate name")
    }

    func testEmptyText() {
        // Like the C++ leaving *err untouched, success simply has no hosts.
        let hosts = parse("")
        XCTAssertEqual(hosts.count, 0)
    }

    // MARK: - Formatting

    func testFormatRoundTrip() {
        let hosts = parse("lab=pi@10.0.0.2:2222;me@example.com;web=me@example.com")
        XCTAssertEqual(hosts.count, 3)
        // Port 22 is omitted; the name is omitted when it equals the host.
        XCTAssertEqual(SSHHostList.line(hosts[0]), "lab=pi@10.0.0.2:2222")
        XCTAssertEqual(SSHHostList.line(hosts[1]), "me@example.com")
        XCTAssertEqual(SSHHostList.line(hosts[2]), "web=me@example.com")

        // Formatting every parsed host parses back to the same list.
        XCTAssertEqual(parse(SSHHostList.text(hosts)), hosts)
    }

    func testTextParsesBackTwoHosts() {
        let text = "lab=pi@10.0.0.2:2222; box=me@example.com"
        let hosts = parse(text)
        XCTAssertEqual(hosts.count, 2)
        XCTAssertEqual(SSHHostList.text(hosts), text)
        XCTAssertEqual(parse(SSHHostList.text(hosts)), hosts)
    }

    func testEmptyListText() {
        XCTAssertEqual(SSHHostList.text([]), "")
        XCTAssertEqual(parse(SSHHostList.text([])).count, 0)
    }

    // The C++ suite's test_host_label (the picker label stays on the pager) and
    // test_null_out_and_err (no pointers in Swift) have no counterpart here.
}

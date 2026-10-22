import XCTest

@testable import PagerKit

/// Covers `JSONValue` decoding from `JSONSerialization` output, encoding back
/// to compact UTF-8 bytes, and the small accessor surface used by handlers.
final class JSONValueTests: XCTestCase {

    // MARK: - Helpers

    private func dec(_ s: String) throws -> JSONValue {
        try JSONValue.decode(bytes: Array(s.utf8))
    }

    private func text(_ bytes: [UInt8]) -> String {
        String(decoding: bytes, as: UTF8.self)
    }

    // MARK: - Scalars

    func testBoolsAreNotNumbers() throws {
        XCTAssertEqual(try dec("true"), .bool(true))
        XCTAssertEqual(try dec("false"), .bool(false))
        XCTAssertEqual(try dec("1"), .number(1))
        XCTAssertEqual(try dec("0"), .number(0))
        XCTAssertEqual(try dec("[true,1,false,0]"),
                       .array([.bool(true), .number(1), .bool(false), .number(0)]))
    }

    func testNumbers() throws {
        XCTAssertEqual(try dec("1.5"), .number(1.5))
        XCTAssertEqual(try dec("-3"), .number(-3))
        XCTAssertEqual(try dec("1e3"), .number(1000))
        XCTAssertEqual(try dec("9007199254740992"), .number(9007199254740992))

        XCTAssertEqual(text(try JSONValue.number(9007199254740992).encodedSorted()), "9007199254740992")
        XCTAssertEqual(text(try JSONValue.number(3).encodedSorted()), "3")
        XCTAssertEqual(text(try JSONValue.number(0.1).encodedSorted()), "0.1")
        XCTAssertEqual(text(try JSONValue.number(-3).encodedSorted()), "-3")
    }

    func testNullAndStrings() throws {
        XCTAssertEqual(try dec("null"), .null)

        // JSON text: "héllo \"q\" \n"
        let input = #""héllo \"q\" \n""#
        XCTAssertEqual(try dec(input), .string("héllo \"q\" \n"))
    }

    // MARK: - Containers

    func testNestedRoundTrip() throws {
        let v: JSONValue = [
            "a": [1, 2.5, "x", nil, true],
            "b": ["c": ["d": false]],
            "e": []
        ]

        XCTAssertEqual(try JSONValue.decode(bytes: try v.encoded()), v)
        XCTAssertEqual(try JSONValue.decode(bytes: try v.encodedSorted()), v)
    }

    func testEncodedSortedIsDeterministic() throws {
        let v: JSONValue = ["b": [true, nil], "a": 1]
        XCTAssertEqual(text(try v.encodedSorted()), #"{"a":1,"b":[true,null]}"#)
    }

    func testEmptyContainers() throws {
        XCTAssertEqual(try dec("{}"), .object([:]))
        XCTAssertEqual(try dec("[]"), .array([]))
    }

    func testInvalidJSONThrows() {
        XCTAssertThrowsError(try dec("{"))
        XCTAssertThrowsError(try dec(""))
        // Not "[1,]": Linux Foundation accepts a trailing comma (old JSONDecoder path too).
        XCTAssertThrowsError(try dec("[1 2]"))
    }

    // MARK: - Accessors

    func testAccessors() throws {
        let v = try dec(#"{"n":7,"s":"x","t":true}"#)

        XCTAssertEqual(v["n"]?.intValue, 7)
        XCTAssertEqual(v["s"]?.stringValue, "x")
        XCTAssertEqual(v["t"]?.boolValue, true)
        XCTAssertNil(v["t"]?.intValue)
        XCTAssertNil(v["n"]?.boolValue)
    }
}

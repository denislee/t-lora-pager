import Foundation
import XCTest

@testable import PagerKit

/// Replays the golden vectors under `protocol/vectors/` through the PagerKit
/// codec, asserting exactly what the C++ firmware tests assert
/// (`test/test_link_frame/test_main.cpp`).
final class VectorTests: XCTestCase {

    // MARK: - Vector loading

    /// The vectors live at `<repo-root>/protocol/vectors/<name>.json`. From this
    /// file (`Tests/PagerKitTests/VectorTests.swift`) that is five path
    /// components up.
    private func vectorURL(_ name: String) -> URL {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        return url
            .appendingPathComponent("protocol")
            .appendingPathComponent("vectors")
            .appendingPathComponent("\(name).json")
    }

    private func load<T: Decodable>(_ type: T.Type, _ name: String) throws -> T {
        let data = try Data(contentsOf: vectorURL(name))
        return try JSONDecoder().decode(T.self, from: data)
    }

    // MARK: - Vector shapes

    private struct MessageVectors: Decodable {
        struct Valid: Decodable {
            let name: String
            let type: UInt8
            let id: UInt16
            let payload_hex: String
            let bytes_hex: String
        }
        struct Malformed: Decodable {
            let name: String
            let bytes_hex: String
            let reason: String
        }
        let valid: [Valid]
        let malformed: [Malformed]
    }

    private struct SegmentVector: Decodable {
        let name: String
        let packet_size: Int
        let start_seq: UInt8
        let message_hex: String
        let packets_hex: [String]
        let next_seq: UInt8
    }

    private struct ReassembleVector: Decodable {
        struct Event: Decodable {
            let kind: String
            let hex: String?
            let reason: String?
        }
        let name: String
        let packets_hex: [String]
        let events: [Event]
    }

    private struct BulkVectors: Decodable {
        struct Data: Decodable {
            let offset: UInt32
            let bytes_hex: String
            let payload_hex: String
        }
        struct Ack: Decodable {
            let next: UInt32
            let credits: UInt16
            let payload_hex: String
        }
        struct Bad: Decodable {
            let name: String
            let payload_hex: String
        }
        struct CRC: Decodable {
            let name: String
            let input_hex: String
            let crc32: UInt32
        }
        let data: [Data]
        let ack: [Ack]
        let bad_data: [Bad]
        let bad_ack: [Bad]
        let crc32: [CRC]
    }

    // MARK: - Hex helpers

    private func hexToBytes(_ hex: String) -> [UInt8] {
        var out: [UInt8] = []
        var high: UInt8?
        for scalar in hex.unicodeScalars {
            guard let nibble = UInt8(String(scalar), radix: 16) else { continue }
            if let current = high {
                out.append((current << 4) | nibble)
                high = nil
            } else {
                high = nibble
            }
        }
        return out
    }

    private func bytesToHex(_ bytes: [UInt8]) -> String {
        let digits = Array("0123456789abcdef")
        var out = ""
        out.reserveCapacity(bytes.count * 2)
        for byte in bytes {
            out.append(digits[Int(byte >> 4)])
            out.append(digits[Int(byte & 0x0F)])
        }
        return out
    }

    // MARK: - Tests

    func testMessageValidRoundTrip() throws {
        let vectors = try load(MessageVectors.self, "message")
        for vector in vectors.valid {
            let bytes = hexToBytes(vector.bytes_hex)
            let message = try Message.decode(bytes)
            XCTAssertEqual(message.type.rawValue, vector.type, vector.name)
            XCTAssertEqual(message.id, vector.id, vector.name)
            XCTAssertEqual(bytesToHex(message.payload), vector.payload_hex, vector.name)
            XCTAssertEqual(bytesToHex(try message.encoded()), vector.bytes_hex, vector.name)
        }
    }

    func testMessageMalformedReasons() throws {
        let vectors = try load(MessageVectors.self, "message")
        for vector in vectors.malformed {
            do {
                _ = try Message.decode(hexToBytes(vector.bytes_hex))
                XCTFail("expected decode failure for \(vector.name)")
            } catch let error as DecodeError {
                XCTAssertEqual(error.rawValue, vector.reason, vector.name)
            } catch {
                XCTFail("unexpected error type for \(vector.name): \(error)")
            }
        }
    }

    func testSegmentPackets() throws {
        let vectors = try load([SegmentVector].self, "segment")
        for vector in vectors {
            var segmenter = Segmenter()
            segmenter.seq = vector.start_seq
            let packets = segmenter.segment(hexToBytes(vector.message_hex), packetSize: vector.packet_size)
            XCTAssertEqual(packets.count, vector.packets_hex.count, vector.name)
            for (packet, expected) in zip(packets, vector.packets_hex) {
                XCTAssertEqual(bytesToHex(packet), expected, vector.name)
            }
            XCTAssertEqual(segmenter.seq, vector.next_seq, vector.name)
        }
    }

    func testSegmentReassembles() throws {
        let vectors = try load([SegmentVector].self, "segment")
        for vector in vectors where vector.start_seq == 0 {
            var reassembler = Reassembler()
            var delivered: [UInt8]?
            for hex in vector.packets_hex {
                for event in reassembler.feed(hexToBytes(hex)) {
                    if case .message(let bytes) = event { delivered = bytes }
                }
            }
            XCTAssertEqual(delivered.map(bytesToHex), vector.message_hex, vector.name)
        }
    }

    func testReassembleEvents() throws {
        let vectors = try load([ReassembleVector].self, "reassemble")
        for vector in vectors {
            var reassembler = Reassembler()
            var events: [ReassemblyEvent] = []
            for hex in vector.packets_hex {
                events.append(contentsOf: reassembler.feed(hexToBytes(hex)))
            }
            XCTAssertEqual(events.count, vector.events.count, vector.name)
            for (event, expected) in zip(events, vector.events) {
                switch event {
                case .message(let bytes):
                    XCTAssertEqual(expected.kind, "message", vector.name)
                    XCTAssertEqual(bytesToHex(bytes), expected.hex, vector.name)
                case .reset(let reason):
                    XCTAssertEqual(expected.kind, "reset", vector.name)
                    XCTAssertEqual(reason.rawValue, expected.reason, vector.name)
                }
            }
        }
    }

    func testBulkDataRoundTrip() throws {
        let vectors = try load(BulkVectors.self, "bulk")
        for vector in vectors.data {
            let bytes = hexToBytes(vector.bytes_hex)
            let payload = try Bulk.encodeData(offset: vector.offset, bytes: bytes)
            XCTAssertEqual(bytesToHex(payload), vector.payload_hex)
            let decoded = Bulk.decodeData(payload)
            XCTAssertEqual(decoded?.offset, vector.offset)
            XCTAssertEqual(decoded.map { bytesToHex($0.bytes) }, vector.bytes_hex)
        }
    }

    func testBulkAckRoundTrip() throws {
        let vectors = try load(BulkVectors.self, "bulk")
        for vector in vectors.ack {
            let payload = Bulk.encodeAck(next: vector.next, credits: vector.credits)
            XCTAssertEqual(bytesToHex(payload), vector.payload_hex)
            let decoded = Bulk.decodeAck(payload)
            XCTAssertEqual(decoded?.next, vector.next)
            XCTAssertEqual(decoded?.credits, vector.credits)
        }
    }

    func testBulkBadRejected() throws {
        let vectors = try load(BulkVectors.self, "bulk")
        for vector in vectors.bad_data {
            XCTAssertNil(Bulk.decodeData(hexToBytes(vector.payload_hex)), vector.name)
        }
        for vector in vectors.bad_ack {
            XCTAssertNil(Bulk.decodeAck(hexToBytes(vector.payload_hex)), vector.name)
        }
    }

    func testBulkCRC32Vectors() throws {
        let vectors = try load(BulkVectors.self, "bulk")
        for vector in vectors.crc32 {
            XCTAssertEqual(CRC32.checksum(hexToBytes(vector.input_hex)), vector.crc32, vector.name)
        }
    }
}

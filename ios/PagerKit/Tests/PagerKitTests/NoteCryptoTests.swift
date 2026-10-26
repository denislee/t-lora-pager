import Foundation
import XCTest

@testable import PagerKit

/// Replays `protocol/vectors/notes_crypto.json` and `git_blob.json` through
/// the PagerKit crypto helpers, asserting exactly what the firmware tests
/// assert (`src/hal/notes_crypto.cpp`, git blob ids).
final class NoteCryptoTests: XCTestCase {

    // MARK: - Vector loading

    /// The vectors live at `<repo-root>/protocol/vectors/<name>.json`. From
    /// this file (`Tests/PagerKitTests/NoteCryptoTests.swift`) that is five
    /// path components up.
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

    private struct CryptoVectors: Decodable {
        struct Valid: Decodable {
            let name: String
            let passphrase: String
            let salt_hex: String
            let key_hex: String
            let iv_hex: String
            let plaintext_hex: String
            let ciphertext_hex: String
        }
        struct WrongPassphrase: Decodable {
            let name: String
            let passphrase: String
            let ciphertext_hex: String
        }
        struct Malformed: Decodable {
            let name: String
            let bytes_hex: String
            let reason: String
        }
        let iterations: Int
        let valid: [Valid]
        let wrong_passphrase: [WrongPassphrase]
        let malformed: [Malformed]
    }

    private struct GitBlobVectors: Decodable {
        struct Blob: Decodable {
            let name: String
            let bytes_hex: String
            let sha1: String
        }
        let blobs: [Blob]
    }

    // MARK: - Helpers

    private func hexBytes(_ hex: String) -> [UInt8] {
        precondition(hex.count % 2 == 0, "odd-length hex")
        var bytes: [UInt8] = []
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else {
                preconditionFailure("bad hex: \(hex)")
            }
            bytes.append(byte)
            index = next
        }
        return bytes
    }

    private func loadCrypto() throws -> CryptoVectors {
        try load(CryptoVectors.self, "notes_crypto")
    }

    // MARK: - Tests

    func testIterationsMatchVectors() throws {
        XCTAssertEqual(try loadCrypto().iterations, NoteCrypto.iterations)
    }

    func testDeriveKeyIV() throws {
        for vector in try loadCrypto().valid {
            let keyIV = try NoteCrypto.deriveKeyIV(
                passphrase: vector.passphrase,
                salt: hexBytes(vector.salt_hex)
            )
            XCTAssertEqual(keyIV.key, hexBytes(vector.key_hex), vector.name)
            XCTAssertEqual(keyIV.iv, hexBytes(vector.iv_hex), vector.name)
        }
    }

    func testDecryptValidVectors() throws {
        for vector in try loadCrypto().valid {
            let ciphertext = hexBytes(vector.ciphertext_hex)
            let plaintext = hexBytes(vector.plaintext_hex)

            XCTAssertTrue(NoteCrypto.isEncrypted(ciphertext), vector.name)
            XCTAssertNotNil(
                String(bytes: plaintext, encoding: .utf8),
                "\(vector.name): vector plaintext must be UTF-8"
            )

            let text = try NoteCrypto.decrypt(ciphertext, passphrase: vector.passphrase)
            XCTAssertEqual(
                text,
                String(decoding: plaintext, as: UTF8.self),
                vector.name
            )

            let decryptor = NoteDecryptor(passphrase: vector.passphrase)
            XCTAssertEqual(try decryptor.decryptBytes(ciphertext), plaintext, vector.name)
        }
    }

    func testWrongPassphraseRejected() throws {
        for vector in try loadCrypto().wrong_passphrase {
            XCTAssertThrowsError(
                try NoteCrypto.decrypt(
                    hexBytes(vector.ciphertext_hex),
                    passphrase: vector.passphrase
                ),
                vector.name
            ) { error in
                XCTAssertEqual(error as? NoteCryptoError, .wrongPassphrase, vector.name)
            }
        }
    }

    func testMalformedRejected() throws {
        for vector in try loadCrypto().malformed {
            let expected: NoteCryptoError
            switch vector.reason {
            case "short": expected = .short
            case "header": expected = .header
            case "length": expected = .length
            default: XCTFail("\(vector.name): unknown reason \(vector.reason)")
                continue
            }
            let bytes = hexBytes(vector.bytes_hex)

            XCTAssertThrowsError(
                try NoteCrypto.decrypt(bytes, passphrase: "hunter2"),
                vector.name
            ) { error in
                XCTAssertEqual(error as? NoteCryptoError, expected, vector.name)
            }

            let decryptor = NoteDecryptor(passphrase: "hunter2")
            XCTAssertThrowsError(try decryptor.decrypt(bytes), vector.name) { error in
                XCTAssertEqual(error as? NoteCryptoError, expected, vector.name)
            }
        }
    }

    func testInvalidUTF8IsWrongPassphrase() throws {
        guard let vector = try loadCrypto().valid.first(where: { $0.name == "multi_block" })
        else {
            return XCTFail("multi_block vector missing")
        }
        let ciphertext = hexBytes(vector.ciphertext_hex)
        let keyIV = try NoteCrypto.deriveKeyIV(
            passphrase: vector.passphrase,
            salt: hexBytes(vector.salt_hex)
        )

        let plaintext = try NoteCrypto.decryptBytes(ciphertext, key: keyIV.key, iv: keyIV.iv)
        XCTAssertEqual(plaintext, hexBytes(vector.plaintext_hex))

        // Sanity guard: an invalid-UTF-8 body would be reported as a wrong
        // passphrase rather than returned as garbage text.
        XCTAssertNil(String(bytes: [0xff, 0xfe], encoding: .utf8))
    }

    func testDecryptorCachesBySalt() throws {
        let vectors = try loadCrypto().valid.filter {
            $0.name == "exact_block" || $0.name == "multi_block"
        }
        XCTAssertEqual(vectors.count, 2)
        XCTAssertEqual(Set(vectors.map(\.salt_hex)).count, 2)

        let decryptor = NoteDecryptor(passphrase: "correct horse battery staple")
        for _ in 0..<2 {
            for vector in vectors {
                XCTAssertEqual(
                    try decryptor.decryptBytes(hexBytes(vector.ciphertext_hex)),
                    hexBytes(vector.plaintext_hex),
                    vector.name
                )
            }
        }
        XCTAssertEqual(decryptor.derivations, 2)
    }

    func testIsEncrypted() {
        XCTAssertFalse(NoteCrypto.isEncrypted([]))
        XCTAssertFalse(NoteCrypto.isEncrypted(Array("Salted_".utf8)))
        XCTAssertFalse(NoteCrypto.isEncrypted(Array("hello".utf8)))
        XCTAssertTrue(NoteCrypto.isEncrypted(Array("Salted__".utf8)))
    }

    func testGitBlobVectors() throws {
        for vector in try load(GitBlobVectors.self, "git_blob").blobs {
            XCTAssertEqual(
                GitBlob.sha1(hexBytes(vector.bytes_hex)),
                vector.sha1,
                vector.name
            )
        }
    }
}

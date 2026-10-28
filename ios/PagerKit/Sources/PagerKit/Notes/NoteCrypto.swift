import Foundation
import Crypto
import _CryptoExtras

/// Why a note's ciphertext could not be read. `.wrongPassphrase` also covers
/// a body that decrypts to something that is not UTF-8.
public enum NoteCryptoError: Error, Equatable, Sendable {
    /// Fewer than 16 bytes, or an empty ciphertext body.
    case short
    /// Missing the `Salted__` magic.
    case header
    /// Body is not a whole number of AES blocks.
    case length
    /// Bad padding, or plaintext that is not valid UTF-8.
    case wrongPassphrase
}

/// Read side of the firmware's `src/hal/notes_crypto.cpp`: OpenSSL `enc`
/// format — `Salted__` + 8-byte salt + AES-256-CBC/PKCS7 ciphertext, with
/// key/IV the first 32+16 bytes of PBKDF2-HMAC-SHA256(passphrase, salt,
/// 10000). Equivalent to
/// `openssl enc -d -aes-256-cbc -pbkdf2 -iter 10000 -md sha256`.
public enum NoteCrypto {
    /// The 8 bytes that mark an OpenSSL `enc` file.
    public static let magic: [UInt8] = Array("Salted__".utf8)

    /// PBKDF2 rounds, fixed by the firmware's on-disk format.
    public static let iterations = 10_000

    /// True when bytes start with the `Salted__` magic (what the pager flags
    /// `protected`).
    public static func isEncrypted(_ bytes: [UInt8]) -> Bool {
        bytes.count >= magic.count && bytes.prefix(magic.count).elementsEqual(magic)
    }

    /// Checks the header and returns the 8-byte salt.
    public static func salt(of bytes: [UInt8]) throws -> [UInt8] {
        guard bytes.count >= 16 else { throw NoteCryptoError.short }
        guard bytes.prefix(magic.count).elementsEqual(magic) else {
            throw NoteCryptoError.header
        }
        return Array(bytes[8..<16])
    }

    /// Validates the ciphertext body and returns it with the header and salt
    /// stripped.
    static func validatedBody(of bytes: [UInt8]) throws -> [UInt8] {
        _ = try salt(of: bytes)
        let body = Array(bytes[16...])
        guard !body.isEmpty else { throw NoteCryptoError.short }
        guard body.count % 16 == 0 else { throw NoteCryptoError.length }
        return body
    }

    /// Derives the 32-byte AES key and 16-byte IV the firmware uses.
    public static func deriveKeyIV(
        passphrase: String,
        salt: [UInt8]
    ) throws -> (key: [UInt8], iv: [UInt8]) {
        // The firmware format fixes 10_000 rounds, so the checked `rounds:`
        // overload (which refuses fewer than 210_000) cannot be used.
        let derived = try KDF.Insecure.PBKDF2.deriveKey(
            from: Array(passphrase.utf8),
            salt: salt,
            using: .sha256,
            outputByteCount: 48,
            unsafeUncheckedRounds: iterations
        )
        let bytes = derived.withUnsafeBytes { Array($0) }
        return (key: Array(bytes[0..<32]), iv: Array(bytes[32..<48]))
    }

    /// Decrypts an OpenSSL `enc` body with an already-derived key and IV.
    public static func decryptBytes(
        _ bytes: [UInt8],
        key: [UInt8],
        iv: [UInt8]
    ) throws -> [UInt8] {
        let body = try validatedBody(of: bytes)
        do {
            // swift-crypto checks every PKCS7 pad byte, so a wrong key fails
            // here (or, ~1 in 256, as non-UTF-8 text in `decrypt`).
            let plaintext = try AES._CBC.decrypt(
                body,
                using: SymmetricKey(data: key),
                iv: try AES._CBC.IV(ivBytes: iv)
            )
            return Array(plaintext)
        } catch {
            throw NoteCryptoError.wrongPassphrase
        }
    }

    /// Decrypts a note with a passphrase. Never returns garbage text: bad
    /// padding and non-UTF-8 plaintext both surface as `.wrongPassphrase`.
    /// Plaintext must never be logged or sent over the link.
    public static func decrypt(_ bytes: [UInt8], passphrase: String) throws -> String {
        let salt = try salt(of: bytes)
        let keyIV = try deriveKeyIV(passphrase: passphrase, salt: salt)
        let plaintext = try decryptBytes(bytes, key: keyIV.key, iv: keyIV.iv)
        guard let text = String(bytes: plaintext, encoding: .utf8) else {
            throw NoteCryptoError.wrongPassphrase
        }
        return text
    }
}

extension NoteCrypto {
    /// A fresh random 8-byte salt.
    public static func randomSalt() -> [UInt8] {
        var generator = SystemRandomNumberGenerator()
        return (0..<8).map { _ in UInt8.random(in: .min ... .max, using: &generator) }
    }

    /// Encrypts with an already-derived key and IV into the firmware's
    /// on-disk format: `Salted__` + `salt` + AES-256-CBC/PKCS7 ciphertext
    /// (what `notes_crypto_encrypt_buffer` writes).
    public static func encryptBytes(
        _ plaintext: [UInt8],
        salt: [UInt8],
        key: [UInt8],
        iv: [UInt8]
    ) throws -> [UInt8] {
        precondition(salt.count == 8, "salt is 8 bytes")
        let body = try AES._CBC.encrypt(
            plaintext,
            using: SymmetricKey(data: key),
            iv: try AES._CBC.IV(ivBytes: iv)
        )
        return magic + salt + Array(body)
    }

    /// Encrypts a note's text with a passphrase and `salt` (default: a fresh
    /// random one, as the firmware does on every save). `decrypt` with the
    /// same passphrase returns `text`.
    public static func encrypt(_ text: String, passphrase: String,
                               salt: [UInt8] = randomSalt()) throws -> [UInt8] {
        let keyIV = try deriveKeyIV(passphrase: passphrase, salt: salt)
        return try encryptBytes(Array(text.utf8), salt: salt, key: keyIV.key, iv: keyIV.iv)
    }
}

/// Decrypts notes with one passphrase, keeping each note's derived (key, IV)
/// by salt (the firmware's MRU key cache: each note has its own salt, so
/// PBKDF2 runs once per note). Thread-safe; drop the instance to forget the
/// passphrase. Run off the main actor.
public final class NoteDecryptor: @unchecked Sendable {
    private let passphrase: String
    private let lock = NSLock()
    private var cache: [[UInt8]: (key: [UInt8], iv: [UInt8])] = [:]
    private var derived = 0

    public init(passphrase: String) {
        self.passphrase = passphrase
    }

    /// Decrypts a note, reusing the key and IV already derived for its salt.
    public func decrypt(_ bytes: [UInt8]) throws -> String {
        let plaintext = try decryptBytes(bytes)
        guard let text = String(bytes: plaintext, encoding: .utf8) else {
            throw NoteCryptoError.wrongPassphrase
        }
        return text
    }

    /// Decrypts a note's ciphertext to raw bytes, reusing the key and IV
    /// already derived for its salt.
    public func decryptBytes(_ bytes: [UInt8]) throws -> [UInt8] {
        let salt = try NoteCrypto.salt(of: bytes)

        lock.lock()
        let cached = cache[salt]
        lock.unlock()

        let keyIV: (key: [UInt8], iv: [UInt8])
        if let cached {
            keyIV = cached
        } else {
            // Derive outside the lock: PBKDF2 is slow and the lock only
            // guards the cache.
            keyIV = try NoteCrypto.deriveKeyIV(passphrase: passphrase, salt: salt)
            lock.lock()
            cache[salt] = keyIV
            derived += 1
            lock.unlock()
        }

        return try NoteCrypto.decryptBytes(bytes, key: keyIV.key, iv: keyIV.iv)
    }

    /// Encrypts a note's text under a fresh salt (one PBKDF2 run; the key is
    /// kept for that salt, so reading the note back costs none).
    public func encrypt(_ text: String) throws -> [UInt8] {
        let salt = NoteCrypto.randomSalt()
        let keyIV = try NoteCrypto.deriveKeyIV(passphrase: passphrase, salt: salt)
        lock.lock()
        cache[salt] = keyIV
        derived += 1
        lock.unlock()
        return try NoteCrypto.encryptBytes(Array(text.utf8), salt: salt, key: keyIV.key, iv: keyIV.iv)
    }

    /// PBKDF2 runs so far (tests).
    public var derivations: Int {
        lock.lock()
        defer { lock.unlock() }
        return derived
    }
}

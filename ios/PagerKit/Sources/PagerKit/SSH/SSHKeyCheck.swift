import Foundation

/// The pager's `ssh.key` setting (a write-only `secret`, max 3000 bytes):
/// an unencrypted OpenSSH or PEM private key, tried before the password.
/// The pager cannot ask for a key passphrase, so an encrypted key is refused
/// here with a clear message rather than failing on the pager later.
public enum SSHKeyCheck {
    public static let maxBytes = 3000

    /// `key` trimmed, with `\r\n` line ends made `\n` and one final newline.
    public static func normalize(_ key: String) -> String {
        let text = key.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? "" : text + "\n"
    }

    /// Why the pager can't use `key` (already `normalize`d), or nil.
    public static func problem(_ key: String) -> String? {
        if key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return "The key is empty." }
        if key.utf8.count > maxBytes {
            return "The key is \(key.utf8.count) bytes; the pager takes at most \(maxBytes). Use an ed25519 key."
        }
        guard key.contains("-----BEGIN"), key.contains("PRIVATE KEY-----") else {
            return "This is not a private key (no \"-----BEGIN … PRIVATE KEY-----\" block). "
                + "Public keys (.pub) don't go on the pager."
        }
        if key.contains("ENCRYPTED") || openSSHCipher(key).map({ $0 != "none" }) == true {
            return "The key is protected by a passphrase, which the pager can't ask for. "
                + "Export an unencrypted copy (ssh-keygen -p -N \"\" -f <copy>) first."
        }
        return nil
    }

    /// The cipher name of an `OPENSSH PRIVATE KEY` (`"none"` when
    /// unencrypted), or nil when `key` is not one or does not parse.
    static func openSSHCipher(_ key: String) -> String? {
        let begin = "-----BEGIN OPENSSH PRIVATE KEY-----", end = "-----END OPENSSH PRIVATE KEY-----"
        guard let b = key.range(of: begin), let e = key.range(of: end), b.upperBound <= e.lowerBound else { return nil }
        let base64 = key[b.upperBound..<e.lowerBound].filter { !$0.isWhitespace }
        guard let data = Data(base64Encoded: String(base64)) else { return nil }
        let bytes = [UInt8](data)
        let magic = Array("openssh-key-v1".utf8) + [0]
        guard bytes.count >= magic.count + 4, Array(bytes.prefix(magic.count)) == magic else { return nil }
        var i = magic.count
        let length = Int(bytes[i]) << 24 | Int(bytes[i + 1]) << 16 | Int(bytes[i + 2]) << 8 | Int(bytes[i + 3])
        i += 4
        guard length <= 64, bytes.count >= i + length else { return nil }
        return String(decoding: bytes[i..<(i + length)], as: UTF8.self)
    }
}

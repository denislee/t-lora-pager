import Crypto

/// The git blob object id of raw bytes: SHA-1("blob <len>\0" + bytes),
/// lowercase hex — what GitHub's trees API reports for a file with exactly
/// these bytes, so it proves a note reached the repo byte for byte.
public enum GitBlob {
    public static func sha1(_ bytes: [UInt8]) -> String {
        var hasher = Insecure.SHA1()
        hasher.update(data: Array("blob \(bytes.count)\0".utf8))
        hasher.update(data: bytes)
        let digest = hasher.finalize()

        let hexDigits = Array("0123456789abcdef".utf8)
        var hex: [UInt8] = []
        hex.reserveCapacity(Insecure.SHA1.byteCount * 2)
        for byte in digest {
            hex.append(hexDigits[Int(byte >> 4)])
            hex.append(hexDigits[Int(byte & 0x0f)])
        }
        return String(decoding: hex, as: UTF8.self)
    }
}

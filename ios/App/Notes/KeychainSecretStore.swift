import Foundation
import PagerKit
import Security

/// `SecretStore` over the Keychain: generic passwords under one service,
/// readable after first unlock, this device only (never synced to iCloud,
/// never in a backup restored elsewhere). Holds the GitHub PAT
/// (`IOS_NOTES_VIEWER_PLAN.md` §3.7).
struct KeychainSecretStore: SecretStore {
    let service: String

    func read(_ key: String) -> String? {
        var query = baseQuery(key)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    func write(_ key: String, _ value: String) throws {
        let data = Data(value.utf8)
        let status = SecItemUpdate(baseQuery(key) as CFDictionary,
                                   [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var add = baseQuery(key)
            add[kSecValueData as String] = data
            add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            let added = SecItemAdd(add as CFDictionary, nil)
            guard added == errSecSuccess else { throw KeychainError(status: added) }
        } else if status != errSecSuccess {
            throw KeychainError(status: status)
        }
    }

    func delete(_ key: String) {
        SecItemDelete(baseQuery(key) as CFDictionary)
    }

    private func baseQuery(_ key: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: key,
            kSecAttrSynchronizable as String: false,
        ]
    }
}

/// A Keychain call failed; carries the `OSStatus`, never the secret.
struct KeychainError: Error, CustomStringConvertible {
    let status: OSStatus

    var description: String { "keychain error \(status)" }
}

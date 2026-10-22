import Foundation

/// A JSON value modelled as a Swift enum (PROTOCOL.md §3.1 payloads). Encodes
/// with a shared `JSONEncoder` and decodes from `JSONSerialization` output.
public enum JSONValue: Codable, Equatable, Sendable, Hashable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
            return
        }
        if let value = try? container.decode(Bool.self) {
            self = .bool(value)
            return
        }
        if let value = try? container.decode(Double.self) {
            self = .number(value)
            return
        }
        if let value = try? container.decode(String.self) {
            self = .string(value)
            return
        }
        if let value = try? container.decode([JSONValue].self) {
            self = .array(value)
            return
        }
        if let value = try? container.decode([String: JSONValue].self) {
            self = .object(value)
            return
        }
        throw DecodingError.dataCorruptedError(in: container, debugDescription: "unknown JSON value")
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        }
    }
}

extension JSONValue {
    /// Member access for object values; nil for anything else or a missing key.
    public subscript(key: String) -> JSONValue? {
        guard case .object(let object) = self else { return nil }
        return object[key]
    }

    public var intValue: Int? {
        guard case .number(let value) = self else { return nil }
        return Int(value)
    }

    public var doubleValue: Double? {
        guard case .number(let value) = self else { return nil }
        return value
    }

    public var stringValue: String? {
        guard case .string(let value) = self else { return nil }
        return value
    }

    public var boolValue: Bool? {
        guard case .bool(let value) = self else { return nil }
        return value
    }

    // One encoder each, configured once and never mutated, so sharing them
    // across tasks is safe (P7.56: a new JSONEncoder per call cost 58 µs/KB).
    nonisolated(unsafe) private static let compactEncoder = JSONEncoder()
    nonisolated(unsafe) private static let sortedEncoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }()

    /// Compact UTF-8 JSON bytes. Key order is unspecified: no peer depends on
    /// it and the golden vectors carry frame bytes, not JSON key order.
    public func encoded() throws -> [UInt8] {
        Array(try Self.compactEncoder.encode(self))
    }

    /// Compact UTF-8 JSON bytes with sorted keys, for deterministic output.
    public func encodedSorted() throws -> [UInt8] {
        Array(try Self.sortedEncoder.encode(self))
    }

    /// Decode UTF-8 JSON bytes. Built from `JSONSerialization` output rather
    /// than `JSONDecoder` (P7.56: ~6x cheaper, no thrown error per node).
    public static func decode(bytes: [UInt8]) throws -> JSONValue {
        let object = try JSONSerialization.jsonObject(with: Data(bytes), options: [.fragmentsAllowed])
        return try JSONValue(serialized: object)
    }

    /// A value as `JSONSerialization` returns it.
    init(serialized object: Any) throws {
        switch object {
        case is NSNull:
            self = .null
        case let number as NSNumber:
            // `NSNumber(1) is Bool` is true, so tell booleans apart by type.
            self = Self.isBoolean(number) ? .bool(number.boolValue) : .number(number.doubleValue)
        case let string as String:
            self = .string(string)
        case let array as [Any]:
            self = .array(try array.map { try JSONValue(serialized: $0) })
        case let dictionary as [String: Any]:
            var result: [String: JSONValue] = [:]
            result.reserveCapacity(dictionary.count)
            for (key, value) in dictionary { result[key] = try JSONValue(serialized: value) }
            self = .object(result)
        default:
            throw DecodingError.dataCorrupted(.init(codingPath: [], debugDescription: "unknown JSON value"))
        }
    }

    #if canImport(Darwin)
    private static func isBoolean(_ number: NSNumber) -> Bool {
        CFGetTypeID(number) == CFBooleanGetTypeID()
    }
    #else
    // swift-corelibs-foundation returns its `__NSCFBoolean` singletons for
    // `true`/`false`, the same class as `NSNumber(value: true)`.
    nonisolated(unsafe) private static let booleanClass: AnyClass = type(of: NSNumber(value: true))
    private static func isBoolean(_ number: NSNumber) -> Bool {
        ObjectIdentifier(type(of: number)) == ObjectIdentifier(booleanClass)
    }
    #endif
}

extension JSONValue: ExpressibleByNilLiteral {
    public init(nilLiteral: ()) { self = .null }
}

extension JSONValue: ExpressibleByBooleanLiteral {
    public init(booleanLiteral value: Bool) { self = .bool(value) }
}

extension JSONValue: ExpressibleByIntegerLiteral {
    public init(integerLiteral value: Int) { self = .number(Double(value)) }
}

extension JSONValue: ExpressibleByFloatLiteral {
    public init(floatLiteral value: Double) { self = .number(value) }
}

extension JSONValue: ExpressibleByStringLiteral {
    public init(stringLiteral value: String) { self = .string(value) }
}

extension JSONValue: ExpressibleByArrayLiteral {
    public init(arrayLiteral elements: JSONValue...) { self = .array(elements) }
}

extension JSONValue: ExpressibleByDictionaryLiteral {
    public init(dictionaryLiteral elements: (String, JSONValue)...) {
        var object: [String: JSONValue] = [:]
        for (key, value) in elements { object[key] = value }
        self = .object(object)
    }
}

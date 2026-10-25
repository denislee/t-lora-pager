/// Strict JSON-number readers for request params. `JSONValue` keeps numbers as
/// `Double`, so "integer" means integral and in range (a fraction, a string or
/// a bool is rejected).
enum Params {
    static func int64(_ value: JSONValue?) -> Int64? {
        guard case .number(let number)? = value, number.isFinite, number == number.rounded(),
              number >= -9_007_199_254_740_992, number <= 9_007_199_254_740_992 else { return nil }
        return Int64(number)
    }

    static func uint32(_ value: JSONValue?) -> UInt32? {
        guard let n = int64(value), n >= 0, n <= Int64(UInt32.max) else { return nil }
        return UInt32(n)
    }

    static func uint16(_ value: JSONValue?) -> UInt16? {
        guard let n = int64(value), n >= 0, n <= Int64(UInt16.max) else { return nil }
        return UInt16(n)
    }
}

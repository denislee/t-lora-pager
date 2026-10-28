/**
 * @file      link_json.h
 * @brief     Minimal JSON writer/reader for PagerLink payloads (cJSON is not available in the emulator build).
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test): no Arduino / ESP-IDF headers, so this compiles on the host for
 * the unit tests as well as on the pager. The PagerLink envelopes exchanged
 * with the phone are small and shaped by our own code on both ends, so a full
 * JSON DOM (ArduinoJson, cJSON) is overkill — and cJSON is not linked into the
 * desktop emulator. This pair of small classes covers the two things the link
 * layer needs:
 *
 *  - JsonWriter: build a compact (no-whitespace) JSON document with automatic
 *    commas/colons, without hand-rolling string escaping at every call site.
 *  - JsonObject: validate an incoming JSON object per RFC 8259 and read
 *    individual top-level members, keeping the raw text of each value so the
 *    caller can decode only what it actually consumes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pagerlink {

/**
 * Incremental writer for compact JSON. Values are appended as they are built;
 * separators are inserted automatically. Example:
 *
 *   JsonWriter w;
 *   w.begin_object().key("t").int_val(42)
 *    .key("a").begin_array().int_val(1).int_val(2).end_array()
 *    .end_object();
 *   // w.str() == {"t":42,"a":[1,2]}
 *
 * The container stack records whether each open object/array has written its
 * first element, so exactly one comma is emitted between siblings; `after_key_`
 * records that the next token is a value and must be prefixed with ':'.
 */
class JsonWriter {
public:
    /** Open an object ('{'). */
    JsonWriter &begin_object();
    /** Close the current object ('}'). */
    JsonWriter &end_object();
    /** Open an array ('['). */
    JsonWriter &begin_array();
    /** Close the current array (']'). */
    JsonWriter &end_array();

    /** Write an object key (escaped); the next value emits ':' first. */
    JsonWriter &key(const char *k);

    /** Write a string value; nullptr is written as `null`. */
    JsonWriter &str_val(const char *s);
    /** Write a string value. */
    JsonWriter &str_val(const std::string &s);

    /** Write a signed integer value. */
    JsonWriter &int_val(int64_t v);
    /** Write an unsigned integer value. */
    JsonWriter &uint_val(uint64_t v);
    /** Write a boolean value (`true` / `false`). */
    JsonWriter &bool_val(bool v);
    /** Write a `null` value. */
    JsonWriter &null_val();

    /** Insert pre-encoded JSON verbatim as one value (nullptr -> `null`). */
    JsonWriter &raw_val(const char *json);

    /** The document built so far. */
    const std::string &str() const { return buf_; }
    /** True when nothing has been written. */
    bool empty() const { return buf_.empty(); }
    /** Reset to an empty document. */
    void clear();

private:
    /** Emit the separator/':'-prefix that precedes a key or value token. */
    void pre_value();
    /** Append `s` as an escaped, quoted JSON string. */
    void write_escaped(const char *s);

    std::string       buf_;
    std::vector<bool> first_;    /* per open container: no element yet? */
    bool              after_key_ = false;
};

/** Byte span [off, off + len) of one value inside a caller's text. */
struct JsonSpan {
    size_t off;
    size_t len;
};

/**
 * A validated JSON object. Only the top-level members are indexed; nested
 * objects are indexed on demand via get_object(). parse() copies the input
 * text into an owned buffer; parse_view() borrows the caller's buffer instead
 * (P7.50). Every member stores the byte span of its raw value text, so values
 * are decoded lazily (and exactly) by the getters, which always return owned
 * copies.
 *
 * Duplicate keys are allowed by parse(); lookups return the last one.
 */
class JsonObject {
public:
    /**
     * Validate `text` as exactly one JSON object (surrounding whitespace
     * allowed) and index its top-level members. Accepts strings with all
     * escapes including \uXXXX, numbers with sign/fraction/exponent,
     * true/false/null and nested objects/arrays up to depth 16. Rejects
     * trailing garbage, leading zeros (`01`), raw control bytes inside
     * strings and any unterminated construct. Returns false without modifying
     * the previous state on failure.
     */
    bool parse(const char *text, size_t len);
    /** Convenience overload for a std::string. */
    bool parse(const std::string &s) { return parse(s.data(), s.size()); }
    /**
     * parse() without the copy (P7.50): same validation, but the object
     * BORROWS `text` — the bytes must stay valid and unchanged for as long as
     * this object (or any copy of it, or any get_object_view() child) is used.
     * Meant for a parse-and-read loop over a buffer the caller owns.
     */
    bool parse_view(const char *text, size_t len);

    /** Number of top-level members recorded. */
    size_t size() const { return members_.size(); }
    /** True when a top-level member with this key exists. */
    bool has(const char *key) const;
    /** Decoded top-level keys in document order (duplicates included). */
    std::vector<std::string> keys() const;

    /** Raw JSON text of the member's value (exact bytes). */
    bool get_raw(const char *key, std::string &out) const;
    /**
     * Decode a string member: all escapes are resolved and \uXXXX (including
     * surrogate pairs) becomes UTF-8. Fails if the value is not a string or
     * contains a lone surrogate.
     */
    bool get_string(const char *key, std::string &out) const;
    /** Integer member: fails on fraction/exponent or when it overflows int64. */
    bool get_int(const char *key, int64_t &out) const;
    /** Numeric member: any valid JSON number via strtod. */
    bool get_double(const char *key, double &out) const;
    /** Boolean member. */
    bool get_bool(const char *key, bool &out) const;
    /** True when the member's value is the literal `null`. */
    bool is_null(const char *key) const;
    /** Parse an object member into `out` (recursive); `out` owns a copy. */
    bool get_object(const char *key, JsonObject &out) const;
    /** get_object() with `out` borrowing this object's text (P7.50): `out` is
     * valid only while this object's backing bytes are (its own buffer, or
     * the caller's buffer for a parse_view() object). */
    bool get_object_view(const char *key, JsonObject &out) const;
    /** The exact text of the object last parsed ("" before any parse). A
     * parse_view() object copies the borrowed bytes out on every call. */
    const std::string &text() const;

private:
    struct Member {
        std::string key; /* decoded */
        size_t      off; /* offset of raw value text in text_ */
        size_t      len; /* length of raw value text */
    };

    /** Last member with this key, or nullptr. */
    const Member *find(const char *key) const;
    /** Advance `i` past spaces, tabs, CR and LF. */
    void skip_ws(size_t &i) const;

    bool parse_value(size_t &i, int depth);
    bool parse_object(size_t &i, int depth, bool top);
    bool parse_array(size_t &i, int depth);
    bool parse_number(size_t &i) const;
    bool parse_literal(size_t &i, const char *lit) const;
    /** Validate the string at `i`; decode into *out when not null. */
    bool parse_string(size_t &i, std::string *out, bool strict) const;

    /** Read-only byte view (the hardware build is gnu++11: no std::string_view). */
    struct Text {
        const char *p;
        size_t      n;
        size_t size() const { return n; }
        const char *data() const { return p; }
        char operator[](size_t i) const { return p[i]; }
        std::string substr(size_t pos, size_t len) const { return std::string(p + pos, len); }
        /** std::string::compare(pos, len, lit) semantics, as far as == 0 goes. */
        int compare(size_t pos, size_t len, const char *lit) const {
            const size_t avail = pos <= n ? n - pos : 0;
            const size_t cnt = len < avail ? len : avail;
            const size_t sl = std::strlen(lit);
            if (cnt != sl) return cnt < sl ? -1 : 1;
            return std::memcmp(p + pos, lit, cnt);
        }
    };
    /** The text being read: the owned copy, or the borrowed bytes. */
    Text src() const {
        return borrowed_ ? Text{view_, view_len_} : Text{text_.data(), text_.size()};
    }

    friend bool json_array_spans(const char *text, size_t len, std::vector<JsonSpan> &out);

    mutable std::string text_; /* owned text; filled lazily by text() when borrowed */
    const char         *view_ = nullptr;
    size_t              view_len_ = 0;
    bool                borrowed_ = false;
    std::vector<Member> members_;
};

/**
 * Split a JSON array into the raw text of each top-level element (exact bytes,
 * surrounding whitespace trimmed). The whole text must be one valid JSON array
 * (surrounding whitespace allowed; validated with the same rules and depth
 * limit as JsonObject::parse). Returns false, leaving `out` cleared, otherwise.
 */
bool json_array_items(const std::string &text, std::vector<std::string> &out);

/**
 * json_array_items() without the copies (P7.50): the same validation, but
 * each element is returned as its byte span inside `text` (exact value bytes,
 * no surrounding whitespace). Returns false, leaving `out` cleared, when
 * `text` is not exactly one valid JSON array. Pair with
 * JsonObject::parse_view() to read object elements in place.
 */
bool json_array_spans(const char *text, size_t len, std::vector<JsonSpan> &out);

}  // namespace pagerlink

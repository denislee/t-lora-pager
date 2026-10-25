/**
 * @file      link_json.cpp
 * @brief     Implementation of the minimal PagerLink JSON writer/reader.
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test), no Arduino / ESP-IDF headers (see link_json.h). The reader is a
 * recursive-descent validator over an owned copy of the input (parse) or a
 * borrowed one (parse_view, json_array_spans; P7.50); it records the raw text
 * span of every top-level member so values are decoded lazily.
 */
#include "hal/link/link_json.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pagerlink {

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------

void JsonWriter::pre_value()
{
    if (after_key_) { buf_.push_back(':'); after_key_ = false; return; }
    if (!first_.empty()) {
        if (!first_.back()) buf_.push_back(',');
        first_.back() = false;
    }
}

void JsonWriter::write_escaped(const char *s)
{
    buf_.push_back('"');
    for (const char *p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':  buf_ += "\\\""; break;
        case '\\': buf_ += "\\\\"; break;
        case '\n': buf_ += "\\n";  break;
        case '\r': buf_ += "\\r";  break;
        case '\t': buf_ += "\\t";  break;
        case '\b': buf_ += "\\b";  break;
        case '\f': buf_ += "\\f";  break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                buf_ += esc;
            } else {
                buf_.push_back(static_cast<char>(c)); /* UTF-8 / printable ASCII */
            }
            break;
        }
    }
    buf_.push_back('"');
}

JsonWriter &JsonWriter::begin_object() { pre_value(); buf_.push_back('{'); first_.push_back(true); return *this; }
JsonWriter &JsonWriter::end_object()   { buf_.push_back('}'); if (!first_.empty()) first_.pop_back(); return *this; }
JsonWriter &JsonWriter::begin_array()  { pre_value(); buf_.push_back('['); first_.push_back(true); return *this; }
JsonWriter &JsonWriter::end_array()    { buf_.push_back(']'); if (!first_.empty()) first_.pop_back(); return *this; }

JsonWriter &JsonWriter::key(const char *k)
{
    pre_value();
    write_escaped(k == nullptr ? "" : k);
    after_key_ = true;
    return *this;
}

JsonWriter &JsonWriter::str_val(const char *s)
{
    pre_value();
    if (s == nullptr) buf_ += "null";
    else              write_escaped(s);
    return *this;
}

JsonWriter &JsonWriter::str_val(const std::string &s) { return str_val(s.c_str()); }
JsonWriter &JsonWriter::int_val(int64_t v)   { pre_value(); buf_ += std::to_string(v); return *this; }
JsonWriter &JsonWriter::uint_val(uint64_t v) { pre_value(); buf_ += std::to_string(v); return *this; }
JsonWriter &JsonWriter::bool_val(bool v)     { pre_value(); buf_ += v ? "true" : "false"; return *this; }
JsonWriter &JsonWriter::null_val()           { pre_value(); buf_ += "null"; return *this; }

JsonWriter &JsonWriter::raw_val(const char *json)
{
    pre_value();
    buf_ += (json == nullptr ? "null" : json);
    return *this;
}

void JsonWriter::clear()
{
    buf_.clear();
    first_.clear();
    after_key_ = false;
}

// ---------------------------------------------------------------------------
// JsonObject helpers
// ---------------------------------------------------------------------------

namespace {

/* Value of the four hex digits at `p` in text[0, n), or -1 if out of range / not hex. */
int hex4(const char *text, size_t n, size_t p)
{
    if (p + 4 > n) return -1;
    int v = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char c = text[p + k];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else                           return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* Append a Unicode scalar as UTF-8 (surrogates encoded as-is when lenient). */
void append_utf8(std::string &out, uint32_t cp)
{
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// JsonObject
// ---------------------------------------------------------------------------

void JsonObject::skip_ws(size_t &i) const
{
    while (i < src().size()) {
        const char c = src()[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
        else break;
    }
}

bool JsonObject::parse_string(size_t &i, std::string *out, bool strict) const
{
    const Text t = src();
    const size_t n = t.size();
    if (i >= n || t[i] != '"') return false;
    ++i;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            ++i;
            if (i >= n) return false;
            switch (t[i]) {
            case '"':  if (out) out->push_back('"');  ++i; break;
            case '\\': if (out) out->push_back('\\'); ++i; break;
            case '/':  if (out) out->push_back('/');  ++i; break;
            case 'b':  if (out) out->push_back('\b'); ++i; break;
            case 'f':  if (out) out->push_back('\f'); ++i; break;
            case 'n':  if (out) out->push_back('\n'); ++i; break;
            case 'r':  if (out) out->push_back('\r'); ++i; break;
            case 't':  if (out) out->push_back('\t'); ++i; break;
            case 'u': {
                const int first = hex4(t.data(), t.size(), i + 1);
                if (first < 0) return false;
                size_t adv = 5; /* 'u' + 4 hex digits */
                uint32_t code = static_cast<uint32_t>(first);
                if (code >= 0xD800 && code <= 0xDBFF) {
                    /* High surrogate: only valid when paired with a low one. */
                    bool paired = false;
                    if (i + 7 < n && t[i + 5] == '\\' && t[i + 6] == 'u') {
                        const int low = hex4(t.data(), t.size(), i + 7);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) +
                                   (static_cast<uint32_t>(low) - 0xDC00);
                            adv = 11; /* \uXXXX\uYYYY */
                            paired = true;
                        }
                    }
                    if (!paired && strict) return false;
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    if (strict) return false; /* lone low surrogate */
                }
                if (out) append_utf8(*out, code);
                i += adv;
                break;
            }
            default:
                return false;
            }
        } else if (c < 0x20) {
            return false; /* raw control byte inside a string */
        } else {
            if (out) out->push_back(static_cast<char>(c));
            ++i;
        }
    }
    return false; /* unterminated string */
}

bool JsonObject::parse_number(size_t &i) const
{
    const Text t = src();
    const size_t n = t.size();
    if (i < n && t[i] == '-') ++i;
    if (i >= n) return false;
    if (t[i] == '0') ++i;
    else if (t[i] >= '1' && t[i] <= '9') while (i < n && t[i] >= '0' && t[i] <= '9') ++i;
    else return false;
    if (i < n && t[i] == '.') {
        ++i;
        if (i >= n || t[i] < '0' || t[i] > '9') return false;
        while (i < n && t[i] >= '0' && t[i] <= '9') ++i;
    }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < n && (t[i] == '+' || t[i] == '-')) ++i;
        if (i >= n || t[i] < '0' || t[i] > '9') return false;
        while (i < n && t[i] >= '0' && t[i] <= '9') ++i;
    }
    return true;
}

bool JsonObject::parse_literal(size_t &i, const char *lit) const
{
    const size_t len = std::strlen(lit);
    if (src().compare(i, len, lit) != 0) return false;
    i += len;
    return true;
}

bool JsonObject::parse_value(size_t &i, int depth)
{
    if (i >= src().size()) return false;
    switch (src()[i]) {
    case '{': return parse_object(i, depth, false);
    case '[': return parse_array(i, depth);
    case '"': return parse_string(i, nullptr, false);
    case 't': return parse_literal(i, "true");
    case 'f': return parse_literal(i, "false");
    case 'n': return parse_literal(i, "null");
    default:  return parse_number(i);
    }
}

bool JsonObject::parse_object(size_t &i, int depth, bool top)
{
    if (depth > 16) return false;
    if (i >= src().size() || src()[i] != '{') return false;
    ++i;
    skip_ws(i);
    if (i < src().size() && src()[i] == '}') { ++i; return true; }
    for (;;) {
        skip_ws(i);
        std::string key;
        if (!parse_string(i, &key, false)) return false;
        skip_ws(i);
        if (i >= src().size() || src()[i] != ':') return false;
        ++i;
        skip_ws(i);
        const size_t vstart = i;
        if (!parse_value(i, depth + 1)) return false;
        if (top) members_.push_back(Member{key, vstart, i - vstart});
        skip_ws(i);
        if (i >= src().size()) return false;
        if (src()[i] == ',') { ++i; continue; }
        if (src()[i] == '}') { ++i; return true; }
        return false;
    }
}

bool JsonObject::parse_array(size_t &i, int depth)
{
    if (depth > 16) return false;
    if (i >= src().size() || src()[i] != '[') return false;
    ++i;
    skip_ws(i);
    if (i < src().size() && src()[i] == ']') { ++i; return true; }
    for (;;) {
        skip_ws(i);
        if (!parse_value(i, depth + 1)) return false;
        skip_ws(i);
        if (i >= src().size()) return false;
        if (src()[i] == ',') { ++i; continue; }
        if (src()[i] == ']') { ++i; return true; }
        return false;
    }
}

bool JsonObject::parse(const char *text, size_t len)
{
    if (text == nullptr) return false;

    /* Validate/index into a temporary so failure leaves the old state be. */
    JsonObject tmp;
    tmp.text_.assign(text, len);
    size_t i = 0;
    tmp.skip_ws(i);
    if (!tmp.parse_object(i, 1, true)) return false;
    tmp.skip_ws(i);
    if (i != tmp.text_.size()) return false;

    text_.swap(tmp.text_);
    members_.swap(tmp.members_);
    borrowed_ = false;
    view_ = nullptr;
    view_len_ = 0;
    return true;
}

bool JsonObject::parse_view(const char *text, size_t len)
{
    if (text == nullptr) return false;

    JsonObject tmp;
    tmp.borrowed_ = true;
    tmp.view_ = text;
    tmp.view_len_ = len;
    size_t i = 0;
    tmp.skip_ws(i);
    if (!tmp.parse_object(i, 1, true)) return false;
    tmp.skip_ws(i);
    if (i != len) return false;

    text_.clear();
    members_.swap(tmp.members_);
    borrowed_ = true;
    view_ = text;
    view_len_ = len;
    return true;
}

const std::string &JsonObject::text() const
{
    if (borrowed_) text_.assign(view_, view_len_); /* fresh copy each call: the bytes may change */
    return text_;
}

const JsonObject::Member *JsonObject::find(const char *key) const
{
    if (key == nullptr) return nullptr;
    const Member *found = nullptr;
    for (const Member &m : members_) {
        if (m.key == key) found = &m; /* duplicate keys: last wins */
    }
    return found;
}

bool JsonObject::has(const char *key) const { return find(key) != nullptr; }

bool JsonObject::get_raw(const char *key, std::string &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    out.assign(src().data() + m->off, m->len);
    return true;
}

bool JsonObject::get_string(const char *key, std::string &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    size_t pos = m->off;
    std::string value;
    if (!parse_string(pos, &value, true)) return false;
    if (pos != m->off + m->len) return false;
    out = value;
    return true;
}

bool JsonObject::get_int(const char *key, int64_t &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    const std::string raw(src().substr(m->off, m->len));
    if (raw.find('.') != std::string::npos ||
        raw.find('e') != std::string::npos ||
        raw.find('E') != std::string::npos) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0' || errno == ERANGE) return false;
    out = static_cast<int64_t>(v);
    return true;
}

bool JsonObject::get_double(const char *key, double &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    const std::string raw(src().substr(m->off, m->len));
    char *end = nullptr;
    const double v = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool JsonObject::get_bool(const char *key, bool &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    const std::string raw(src().substr(m->off, m->len));
    if (raw == "true")  { out = true;  return true; }
    if (raw == "false") { out = false; return true; }
    return false;
}

bool JsonObject::is_null(const char *key) const
{
    const Member *m = find(key);
    return m != nullptr && src().compare(m->off, m->len, "null") == 0;
}

bool JsonObject::get_object(const char *key, JsonObject &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    const Text t = src();
    if (m->len == 0 || t[m->off] != '{') return false;
    return out.parse(t.data() + m->off, m->len);
}

bool JsonObject::get_object_view(const char *key, JsonObject &out) const
{
    const Member *m = find(key);
    if (m == nullptr) return false;
    const Text t = src();
    if (m->len == 0 || t[m->off] != '{') return false;
    return out.parse_view(t.data() + m->off, m->len);
}

// ---------------------------------------------------------------------------
// json_array_items
// ---------------------------------------------------------------------------

bool json_array_spans(const char *text, size_t len, std::vector<JsonSpan> &out)
{
    out.clear();
    if (text == nullptr) return false;

    /* Validate in place over a borrowed reader. Elements are parsed at the
     * depth they had when json_array_items() wrapped the array as an object
     * member ({"a":[...]}: array at 2, elements at 3), so the depth limit is
     * unchanged. Each span is the exact value text (whitespace is skipped
     * around it, never included). */
    JsonObject r;
    r.borrowed_ = true;
    r.view_ = text;
    r.view_len_ = len;
    const int depth = 2;

    size_t i = 0;
    r.skip_ws(i);
    if (i >= len || text[i] != '[') return false;
    ++i;
    r.skip_ws(i);
    if (i < len && text[i] == ']') {
        ++i;
    } else {
        for (;;) {
            r.skip_ws(i);
            const size_t b = i;
            if (!r.parse_value(i, depth + 1)) { out.clear(); return false; }
            out.push_back(JsonSpan{b, i - b});
            r.skip_ws(i);
            if (i >= len) { out.clear(); return false; }
            if (text[i] == ',') { ++i; continue; }
            if (text[i] == ']') { ++i; break; }
            out.clear();
            return false;
        }
    }
    r.skip_ws(i);
    if (i != len) { out.clear(); return false; }
    return true;
}

bool json_array_items(const std::string &text, std::vector<std::string> &out)
{
    out.clear();
    std::vector<JsonSpan> spans;
    if (!json_array_spans(text.data(), text.size(), spans)) return false;
    out.reserve(spans.size());
    for (const JsonSpan &sp : spans) out.push_back(text.substr(sp.off, sp.len));
    return true;
}

}  // namespace pagerlink

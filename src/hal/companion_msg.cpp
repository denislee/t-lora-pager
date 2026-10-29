/**
 * @file      companion_msg.cpp
 * @brief     PagerLink messaging rules and the inbox ring — see companion_msg.h.
 */
#include "companion_msg.h"

namespace hal {
namespace msg {

namespace {

/** Which control characters a text field tolerates. */
enum class Ctl : uint8_t { None, Newline, NewlineTab };

/** True when `s` is well-formed UTF-8 (no overlongs, surrogates or > U+10FFFF). */
bool utf8_ok(const std::string &s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

bool has_bad_control(const std::string &s, Ctl allowed) {
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c != 0x7F) continue;
        if (c == '\n' && allowed != Ctl::None) continue;
        if (c == '\t' && allowed == Ctl::NewlineTab) continue;
        return true;
    }
    return false;
}

/**
 * Read string member `key` under the §6.5 rules. Returns false with `why` set
 * on a rule break. An absent optional member leaves `out` empty and succeeds.
 */
bool read_text(const pagerlink::JsonObject &p, const char *key, bool required, size_t max,
               Ctl ctl, std::string &out, std::string &why) {
    out.clear();
    const std::string k(key);
    if (!p.has(key)) {
        if (!required) return true;
        why = k + ": required";
        return false;
    }
    if (!p.get_string(key, out)) {
        why = k + ": expected string";
        return false;
    }
    if (required && out.empty()) {
        why = k + ": empty";
        return false;
    }
    if (out.size() > max) {
        why = k + ": too long";
        return false;
    }
    if (!utf8_ok(out)) {
        why = k + ": not valid UTF-8";
        return false;
    }
    if (has_bad_control(out, ctl)) {
        why = k + ": control character";
        return false;
    }
    return true;
}

bool id_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == ':' || c == '-';
}

bool parse_prio(const std::string &s, Prio &out) {
    if (s == "low") out = Prio::Low;
    else if (s == "normal") out = Prio::Normal;
    else if (s == "high") out = Prio::High;
    else return false;
    return true;
}

}  // namespace

const char *prio_name(Prio p) {
    switch (p) {
    case Prio::Low: return "low";
    case Prio::High: return "high";
    case Prio::Normal:
    default: return "normal";
    }
}

bool parse_notify_post(const pagerlink::JsonObject &p, NotifyPost &out, std::string &why) {
    out = NotifyPost();
    if (!read_text(p, "title", true, kTitleMax, Ctl::None, out.title, why)) return false;
    if (!read_text(p, "body", false, kBodyMax, Ctl::Newline, out.body, why)) return false;

    if (p.has("id")) {
        if (!p.get_string("id", out.id)) {
            why = "id: expected string";
            return false;
        }
        if (out.id.empty() || out.id.size() > kIdMax) {
            why = out.id.empty() ? "id: empty" : "id: too long";
            return false;
        }
        for (char c : out.id) {
            if (!id_char_ok(c)) {
                why = "id: bad character";
                return false;
            }
        }
    }

    if (!read_text(p, "src", false, kSrcMax, Ctl::None, out.src, why)) return false;

    if (p.has("ts")) {
        int64_t ts = 0;
        if (!p.get_int("ts", ts)) {
            why = "ts: expected integer";
            return false;
        }
        if (ts < 0 || ts > 4294967295LL) {
            why = "ts: out of range";
            return false;
        }
        out.ts = static_cast<uint32_t>(ts);
    }

    if (p.has("prio")) {
        std::string s;
        if (!p.get_string("prio", s) || !parse_prio(s, out.prio)) {
            why = "prio: not one of low, normal, high";
            return false;
        }
    }
    return true;
}

bool parse_clip_push(const pagerlink::JsonObject &p, std::string &text, std::string &why) {
    return read_text(p, "text", true, kClipMax, Ctl::NewlineTab, text, why);
}

bool parse_text_send(const pagerlink::JsonObject &p, std::string &text, Target &target,
                     std::string &why) {
    if (!read_text(p, "text", true, kTextSendMax, Ctl::NewlineTab, text, why)) return false;
    std::string t;
    if (!p.has("target")) {
        why = "target: required";
        return false;
    }
    if (!p.get_string("target", t) || (t != "editor" && t != "kb")) {
        why = "target: not one of editor, kb";
        return false;
    }
    target = (t == "editor") ? Target::Editor : Target::Kb;
    return true;
}

Delivery delivery_for(Prio p, bool fake_sleep, bool can_wake) {
    Delivery d;
    switch (p) {
    case Prio::Low:
        break;
    case Prio::Normal:
        d.banner = d.haptic = !fake_sleep;
        break;
    case Prio::High:
        d.banner = d.haptic = true;
        d.wake = fake_sleep && can_wake;
        break;
    }
    return d;
}

bool inbox_add(std::vector<InboxEntry> &ring, const InboxEntry &e, size_t cap) {
    if (!e.id.empty()) {
        for (const InboxEntry &x : ring) {
            if (x.id == e.id) return false;
        }
    }
    ring.push_back(e);
    if (cap == 0) cap = 1;
    if (ring.size() > cap) ring.erase(ring.begin(), ring.begin() + (ring.size() - cap));
    return true;
}

size_t inbox_unread(const std::vector<InboxEntry> &ring) {
    size_t n = 0;
    for (const InboxEntry &x : ring) {
        if (!x.read) n++;
    }
    return n;
}

std::string inbox_encode(const std::vector<InboxEntry> &ring) {
    std::string out;
    for (const InboxEntry &e : ring) {
        pagerlink::JsonWriter w;
        w.begin_object();
        if (!e.id.empty()) w.key("id").str_val(e.id);
        w.key("kind").str_val(e.kind.empty() ? std::string("msg") : e.kind);
        w.key("title").str_val(e.title);
        if (!e.body.empty()) w.key("body").str_val(e.body);
        if (!e.src.empty()) w.key("src").str_val(e.src);
        if (e.ts) w.key("ts").uint_val(e.ts);
        if (e.rx) w.key("rx").uint_val(e.rx);
        w.key("prio").str_val(prio_name(e.prio));
        if (e.read) w.key("read").bool_val(true);
        w.end_object();
        out += w.str();
        out += '\n';
    }
    return out;
}

void inbox_decode(const std::string &text, std::vector<InboxEntry> &out, size_t cap) {
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        pagerlink::JsonObject o;
        InboxEntry e;
        if (nl > pos && o.parse(text.data() + pos, nl - pos) && o.get_string("title", e.title) &&
            !e.title.empty()) {
            o.get_string("id", e.id);
            if (!o.get_string("kind", e.kind) || (e.kind != "msg" && e.kind != "clip")) {
                e.kind = "msg";
            }
            o.get_string("body", e.body);
            o.get_string("src", e.src);
            int64_t v = 0;
            if (o.get_int("ts", v) && v > 0 && v <= 4294967295LL) e.ts = static_cast<uint32_t>(v);
            v = 0;
            if (o.get_int("rx", v) && v > 0 && v <= 4294967295LL) e.rx = static_cast<uint32_t>(v);
            std::string pr;
            if (!o.get_string("prio", pr) || !parse_prio(pr, e.prio)) e.prio = Prio::Normal;
            bool rd = false;
            if (o.get_bool("read", rd)) e.read = rd;
            out.push_back(e);
        }
        pos = nl + 1;
    }
    if (cap == 0) cap = 1;
    if (out.size() > cap) out.erase(out.begin(), out.begin() + (out.size() - cap));
}

size_t utf8_prefix_len(const std::string &s, size_t max) {
    if (s.size() <= max) return s.size();
    size_t n = max;
    // Back up over continuation bytes so the cut lands on a lead byte.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return n;
}

}  // namespace msg
}  // namespace hal

/**
 * @file      settings_registry.cpp
 * @brief     Validation, batch set, revision and JSON encoders of the settings
 *            registry (see settings_registry.h). Pure C++, host-tested.
 */
#include "settings_registry.h"

#include <cstdio>
#include <cstdlib>

namespace settings {

using pagerlink::JsonObject;
using pagerlink::JsonWriter;

const char *type_name(Type t) {
    switch (t) {
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Enum: return "enum";
    case Type::String: return "string";
    case Type::Secret: return "secret";
    case Type::List: return "list";
    case Type::Object: return "object";
    }
    return "bool";
}

const char *apply_name(Apply a) {
    switch (a) {
    case Apply::Live: return "live";
    case Apply::Tick: return "tick";
    case Apply::Theme: return "theme";
    case Apply::NextSleep: return "sleep";
    case Apply::Reboot: return "reboot";
    }
    return "live";
}

/* --- Value --- */

Value Value::of_bool(bool v) {
    Value x;
    x.kind = Bool;
    x.b = v;
    return x;
}

Value Value::of_int(int64_t v) {
    Value x;
    x.kind = Int;
    x.i = v;
    return x;
}

Value Value::of_str(const std::string &v) {
    Value x;
    x.kind = Str;
    x.s = v;
    return x;
}

Value Value::of_json(const std::string &raw) {
    Value x;
    x.kind = Json;
    x.s = raw;
    return x;
}

bool Value::operator==(const Value &o) const {
    if (kind != o.kind) return false;
    switch (kind) {
    case None: return true;
    case Bool: return b == o.b;
    case Int: return i == o.i;
    case Str:
    case Json: return s == o.s;
    }
    return false;
}

void Value::write(JsonWriter &w) const {
    switch (kind) {
    case None: w.null_val(); break;
    case Bool: w.bool_val(b); break;
    case Int: w.int_val(i); break;
    case Str: w.str_val(s); break;
    case Json: w.raw_val(s.c_str()); break;
    }
}

bool value_from_json(const std::string &raw, Value &out) {
    JsonObject o;
    if (!o.parse("{\"v\":" + raw + "}")) return false;
    if (o.is_null("v")) {
        out = Value();
        return true;
    }
    bool b = false;
    if (o.get_bool("v", b)) {
        out = Value::of_bool(b);
        return true;
    }
    int64_t i = 0;
    if (o.get_int("v", i)) {
        out = Value::of_int(i);
        return true;
    }
    std::string s;
    if (o.get_string("v", s)) {
        out = Value::of_str(s);
        return true;
    }
    std::string text;
    if (!o.get_raw("v", text) || text.empty()) return false;
    if (text[0] == '[' || text[0] == '{') {
        out = Value::of_json(text);
        return true;
    }
    return false; /* a number with a fraction / exponent */
}

/* --- Registry --- */

Registry::Registry(uint32_t rev0) : rev_(rev0) {}

void Registry::clear(uint32_t rev0) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    groups_.clear();
    defs_.clear();
    actions_.clear();
    observers_.clear();
    rev_ = rev0;
    rev_schema_.clear();
}

void Registry::add_group(const std::string &id, const std::string &title) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Group g;
    g.id = id;
    g.title = title;
    groups_.push_back(g);
    rev_schema_.clear();
}

void Registry::add(const Def &d) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    defs_.push_back(d);
    rev_schema_.clear();
}

void Registry::add_action(const ActionDef &a) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    actions_.push_back(a);
    rev_schema_.clear();
}

bool Registry::bind(const std::string &id, Getter get, Setter set, Check check) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (Def &d : defs_) {
        if (d.id != id) continue;
        d.get = get;
        d.set = set;
        if (check) d.check = check;
        rev_schema_.clear(); /* "ro" follows whether a setter is bound */
        return true;
    }
    return false;
}

bool Registry::bind_action(
    const std::string &id,
    std::function<bool(const JsonObject &, JsonWriter &, Error &)> run) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (ActionDef &a : actions_) {
        if (a.id != id) continue;
        a.run = run;
        return true;
    }
    return false;
}

const Def *Registry::find(const std::string &id) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (const Def &d : defs_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

const ActionDef *Registry::find_action(const std::string &id) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (const ActionDef &a : actions_) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

uint32_t Registry::rev() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return rev_;
}

std::string Registry::rev_schema() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!rev_schema_.empty()) return rev_schema_;
    /* FNV-1a over the encoding every page is cut from. */
    uint32_t h = 2166136261u;
    auto mix = [&h](const std::string &s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
    };
    for (const Group &g : groups_) mix(g.id + "\x1f" + g.title + "\x1e");
    for (const Def &d : defs_) {
        if (!d.supported) continue;
        JsonWriter w;
        encode_item(d, w);
        mix(w.str());
    }
    for (const ActionDef &a : actions_) {
        if (!a.supported) continue;
        JsonWriter w;
        encode_action(a, w);
        mix(w.str());
    }
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(h));
    rev_schema_ = buf;
    return rev_schema_;
}

bool Registry::validate(const Def &d, const Value &v, Error &err) const {
    const std::string &id = d.id;
    switch (d.type) {
    case Type::Bool:
        if (v.kind != Value::Bool) {
            err.set("invalid_argument", id + ": expected bool");
            return false;
        }
        break;
    case Type::Int:
        if (v.kind != Value::Int) {
            err.set("invalid_argument", id + ": expected integer");
            return false;
        }
        if (v.i < d.min || v.i > d.max) {
            err.set("invalid_argument", id + ": out of range");
            return false;
        }
        if (d.step > 1 && (v.i - d.min) % d.step != 0) {
            err.set("invalid_argument", id + ": not a multiple of the step");
            return false;
        }
        break;
    case Type::Enum: {
        bool found = false;
        for (const Option &o : d.options) {
            if (o.v == v) {
                found = true;
                break;
            }
        }
        if (!found) {
            err.set("invalid_argument", id + ": not one of the choices");
            return false;
        }
        break;
    }
    case Type::String:
    case Type::Secret: {
        if (v.kind != Value::Str) {
            err.set("invalid_argument", id + ": expected string");
            return false;
        }
        const size_t max = d.max_len ? d.max_len : 255;
        if (v.s.size() > max) {
            err.set("invalid_argument", id + ": too long");
            return false;
        }
        for (unsigned char c : v.s) {
            if (c < 0x20 || c == 0x7f) {
                err.set("invalid_argument", id + ": control character");
                return false;
            }
        }
        break;
    }
    case Type::List:
        if (v.kind != Value::Json || v.s.empty() || v.s[0] != '[') {
            err.set("invalid_argument", id + ": expected array");
            return false;
        }
        break;
    case Type::Object:
        if (v.kind != Value::Json || v.s.empty() || v.s[0] != '{') {
            err.set("invalid_argument", id + ": expected object");
            return false;
        }
        break;
    }
    if (d.check && !d.check(v, err)) {
        if (err.code.empty()) err.code = "invalid_argument";
        if (err.msg.empty()) err.msg = id + ": rejected";
        else if (err.msg.compare(0, id.size(), id) != 0) err.msg = id + ": " + err.msg;
        return false;
    }
    return true;
}

bool Registry::check_one(const std::string &id, const Value &v, const Def *&d, Error &err) const {
    d = find(id);
    if (!d) {
        err.set("invalid_argument", id + ": unknown setting");
        return false;
    }
    if (!d->supported) {
        err.set("unsupported", id + ": not on this board");
        return false;
    }
    if (d->read_only || !d->set) {
        err.set("invalid_argument", id + ": read-only");
        return false;
    }
    return validate(*d, v, err);
}

bool Registry::get(const std::string &id, Value &out, Error &err) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const Def *d = find(id);
    if (!d) {
        err.set("invalid_argument", id + ": unknown setting");
        return false;
    }
    if (!d->supported) {
        err.set("unsupported", id + ": not on this board");
        return false;
    }
    out = d->get ? d->get() : Value();
    return true;
}

bool Registry::set(const std::string &id, const Value &v, Error &err) {
    std::vector<std::pair<std::string, Value>> one;
    one.push_back(std::make_pair(id, v));
    return set_many(one, err);
}

bool Registry::set_many(const std::vector<std::pair<std::string, Value>> &vals, Error &err,
                        std::vector<std::string> *applied, bool *reboot) {
    std::vector<std::string> changed;
    bool need_reboot = false;
    bool ok = true;
    uint32_t rev_now = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        /* Pass 1: everything is validated before anything is written. */
        std::vector<const Def *> defs;
        defs.reserve(vals.size());
        for (const auto &kv : vals) {
            const Def *d = nullptr;
            if (!check_one(kv.first, kv.second, d, err)) return false;
            defs.push_back(d);
        }
        /* Pass 2: write what changed. */
        for (size_t i = 0; i < vals.size(); ++i) {
            const Def &d = *defs[i];
            const Value &v = vals[i].second;
            if (!d.secret && d.get && d.get() == v) continue;
            Error e;
            if (!d.set(v, e)) {
                err.code = e.code.empty() ? "internal" : e.code;
                err.msg = d.id + ": " + (e.msg.empty() ? std::string("failed") : e.msg);
                ok = false;
                break;
            }
            changed.push_back(d.id);
            if (d.apply == Apply::Reboot) need_reboot = true;
        }
        if (!changed.empty()) ++rev_;
        rev_now = rev_;
    }
    if (applied) *applied = changed;
    if (reboot) *reboot = need_reboot;
    if (!changed.empty()) notify(rev_now, changed);
    return ok;
}

void Registry::note_changed(const std::vector<std::string> &ids) {
    if (ids.empty()) return;
    uint32_t rev_now;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        rev_now = ++rev_;
    }
    notify(rev_now, ids);
}

int Registry::subscribe(Observer fn) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const int token = next_token_++;
    observers_.push_back(std::make_pair(token, fn));
    return token;
}

void Registry::unsubscribe(int token) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (size_t i = 0; i < observers_.size(); ++i) {
        if (observers_[i].first == token) {
            observers_.erase(observers_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void Registry::notify(uint32_t rev, const std::vector<std::string> &ids) {
    std::vector<Observer> fns;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        for (const auto &o : observers_) fns.push_back(o.second);
    }
    for (const Observer &fn : fns) {
        if (fn) fn(rev, ids);
    }
}

/* --- JSON --- */

void Registry::encode_item(const Def &d, JsonWriter &w) const {
    w.begin_object();
    w.key("id").str_val(d.id);
    w.key("group").str_val(d.group);
    w.key("title").str_val(d.title);
    w.key("type").str_val(type_name(d.type));
    if (d.type == Type::Int) {
        w.key("min").int_val(d.min);
        w.key("max").int_val(d.max);
        w.key("step").int_val(d.step > 0 ? d.step : 1);
    }
    if (d.type == Type::Enum) {
        w.key("enum").begin_array();
        for (const Option &o : d.options) {
            w.begin_object().key("v");
            o.v.write(w);
            if (!o.label.empty()) w.key("label").str_val(o.label);
            w.end_object();
        }
        w.end_array();
    }
    if (d.type == Type::String || d.type == Type::Secret) {
        w.key("max_len").int_val(d.max_len ? d.max_len : 255);
    }
    w.key("apply").str_val(apply_name(d.apply));
    if (d.secret) w.key("secret").bool_val(true);
    if (d.danger) w.key("danger").bool_val(true);
    if (d.read_only || !d.set) w.key("ro").bool_val(true);
    if (!d.hint.empty()) w.key("hint").str_val(d.hint);
    w.end_object();
}

void Registry::encode_action(const ActionDef &a, JsonWriter &w) const {
    w.begin_object();
    w.key("id").str_val(a.id);
    w.key("group").str_val(a.group);
    w.key("title").str_val(a.title);
    if (a.danger) w.key("danger").bool_val(true);
    if (!a.hint.empty()) w.key("hint").str_val(a.hint);
    w.end_object();
}

void Registry::encode_value(const Def &d, JsonWriter &w) const {
    const Value v = d.get ? d.get() : Value();
    if (d.secret) {
        w.begin_object().key("set").bool_val(v.kind == Value::Bool && v.b).end_object();
    } else {
        v.write(w);
    }
}

bool Registry::schema_json(const JsonObject &params, JsonWriter &out, Error &err,
                           size_t max_bytes) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    /* The cursor is the index of the next supported item: "<n>". */
    size_t start = 0;
    if (params.has("cursor")) {
        std::string c;
        if (!params.get_string("cursor", c) || c.empty() || c.size() > 6 ||
            c.find_first_not_of("0123456789") != std::string::npos) {
            err.set("invalid_argument", "bad cursor");
            return false;
        }
        start = static_cast<size_t>(std::strtoul(c.c_str(), nullptr, 10));
    }
    std::vector<const Def *> items;
    for (const Def &d : defs_) {
        if (d.supported) items.push_back(&d);
    }
    if (start > items.size()) {
        err.set("invalid_argument", "bad cursor");
        return false;
    }

    out.begin_object();
    out.key("rev_schema").str_val(rev_schema());
    if (start == 0) {
        out.key("groups").begin_array();
        for (const Group &g : groups_) {
            out.begin_object().key("id").str_val(g.id).key("title").str_val(g.title).end_object();
        }
        out.end_array();
        out.key("actions").begin_array();
        for (const ActionDef &a : actions_) {
            if (!a.supported) continue;
            JsonWriter w;
            encode_action(a, w);
            out.raw_val(w.str().c_str());
        }
        out.end_array();
    }
    /* Room left for the items, minus the closers and a cursor member. */
    const size_t reserve = 32;
    out.key("items").begin_array();
    size_t i = start;
    bool first = true;
    for (; i < items.size(); ++i) {
        JsonWriter w;
        encode_item(*items[i], w);
        if (!first && out.str().size() + w.str().size() + 1 + reserve > max_bytes) break;
        out.raw_val(w.str().c_str());
        first = false;
    }
    out.end_array();
    if (i < items.size()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(i));
        out.key("cursor").str_val(buf);
    }
    out.end_object();
    return true;
}

bool Registry::get_json(const JsonObject &params, JsonWriter &out, Error &err) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<const Def *> pick;
    if (params.has("ids")) {
        std::string raw;
        std::vector<std::string> items;
        if (!params.get_raw("ids", raw) || !pagerlink::json_array_items(raw, items)) {
            err.set("invalid_argument", "ids must be an array");
            return false;
        }
        for (const std::string &it : items) {
            Value v;
            if (!value_from_json(it, v) || v.kind != Value::Str) {
                err.set("invalid_argument", "ids must be strings");
                return false;
            }
            const Def *d = find(v.s);
            if (!d) {
                err.set("invalid_argument", v.s + ": unknown setting");
                return false;
            }
            if (!d->supported) {
                err.set("unsupported", v.s + ": not on this board");
                return false;
            }
            pick.push_back(d);
        }
    } else {
        for (const Def &d : defs_) {
            if (d.supported) pick.push_back(&d);
        }
    }
    out.begin_object();
    out.key("rev").uint_val(rev_);
    out.key("rev_schema").str_val(rev_schema());
    out.key("values").begin_object();
    for (const Def *d : pick) {
        out.key(d->id.c_str());
        encode_value(*d, out);
    }
    out.end_object();
    out.end_object();
    return true;
}

bool Registry::set_json(const JsonObject &params, JsonWriter &out, Error &err) {
    /* Held across the if_rev check and the writes, so no other writer lands
     * in between (observers then run with it held, on this thread). */
    std::lock_guard<std::recursive_mutex> lock(mu_);
    JsonObject values;
    if (!params.get_object("values", values)) {
        err.set("invalid_argument", "values must be an object");
        return false;
    }
    if (params.has("if_rev")) {
        int64_t want = 0;
        if (!params.get_int("if_rev", want)) {
            err.set("invalid_argument", "if_rev must be an integer");
            return false;
        }
        if (static_cast<uint32_t>(want) != rev() || want < 0) {
            err.set("invalid_argument", "changed");
            return false;
        }
    }
    std::vector<std::pair<std::string, Value>> vals;
    for (const std::string &k : values.keys()) {
        std::string raw;
        Value v;
        if (!values.get_raw(k.c_str(), raw) || !value_from_json(raw, v)) {
            err.set("invalid_argument", k + ": bad value");
            return false;
        }
        vals.push_back(std::make_pair(k, v));
    }
    if (vals.empty()) {
        err.set("invalid_argument", "values is empty");
        return false;
    }
    std::vector<std::string> applied;
    bool reboot = false;
    if (!set_many(vals, err, &applied, &reboot)) return false;
    out.begin_object();
    out.key("rev").uint_val(rev());
    out.key("applied").begin_array();
    for (const std::string &id : applied) out.str_val(id);
    out.end_array();
    out.key("reboot").bool_val(reboot);
    out.end_object();
    return true;
}

}  // namespace settings

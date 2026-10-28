/**
 * @file      settings_registry.h
 * @brief     One table of every user setting, shared by the pager's Settings
 *            app and the PagerLink settings.* RPCs (IOS_SETTINGS_PLAN.md §4.1).
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test): no Arduino, LVGL or
 * HAL headers. Every store and apply step sits behind the Def's function
 * objects, so validation, the all-or-nothing batch set, the revision counter
 * and the JSON encoders are unit-tested on the host (test_settings_registry).
 * The device table that binds each id to its HAL call is settings_table.cpp.
 *
 * Rules the registry enforces for both UIs:
 *  - a value is validated (type, range/step, enum, length, charset, the Def's
 *    own check) before anything is written;
 *  - a batch is all or nothing: one bad value fails the whole call and
 *    nothing is written;
 *  - an unchanged value is not written again (NVS wear, R7) and does not bump
 *    the revision;
 *  - every write that changed something bumps `rev` once and tells the
 *    observers which ids changed.
 *
 * Thread-safety: every public method takes the registry's recursive mutex;
 * setters run with it held (they may call back into the registry). Observers
 * run after the mutex is released, on the writer's thread.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "hal/link/link_json.h"

namespace settings {

/** Wire type of a setting ("bool", "int", "enum", "string", "secret", "list", "object"). */
enum class Type : uint8_t { Bool, Int, Enum, String, Secret, List, Object };

/** When a change takes effect ("live", "tick", "theme", "sleep", "reboot"). */
enum class Apply : uint8_t { Live, Tick, Theme, NextSleep, Reboot };

const char *type_name(Type t);
const char *apply_name(Apply a);

/** A setting value: a JSON scalar, or raw JSON text for lists and objects. */
struct Value {
    enum Kind : uint8_t { None, Bool, Int, Str, Json };
    Kind kind = None;
    bool b = false;
    int64_t i = 0;
    std::string s; /* Str: the text; Json: compact raw JSON (array / object) */

    static Value of_bool(bool v);
    static Value of_int(int64_t v);
    static Value of_str(const std::string &v);
    static Value of_json(const std::string &raw);

    bool operator==(const Value &o) const;
    bool operator!=(const Value &o) const { return !(*this == o); }
    /** Append this value to `w` (Json is inserted verbatim). */
    void write(pagerlink::JsonWriter &w) const;
};

/** Decode one raw JSON value. Numbers with a fraction or exponent fail. */
bool value_from_json(const std::string &raw, Value &out);

/** Wire error: code is a PROTOCOL.md §3.4 code, msg names the id. */
struct Error {
    std::string code;
    std::string msg;
    void set(const char *c, const std::string &m) { code = c; msg = m; }
};

/** One enum choice. `label` empty = same as the value. */
struct Option {
    Value v;
    std::string label;
};

using Getter = std::function<Value()>;
/** Store and apply an already validated value. Return false with `err` set on failure. */
using Setter = std::function<bool(const Value &v, Error &err)>;
/** Extra validation with no side effects (charset, URL form, notes unlocked, ...). */
using Check = std::function<bool(const Value &v, Error &err)>;

struct Def {
    std::string id;
    std::string group;
    std::string title;
    Type type = Type::Bool;
    int64_t min = 0, max = 0, step = 1; /* Int */
    std::vector<Option> options;        /* Enum */
    uint16_t max_len = 0;               /* String / Secret: max bytes (0 = 255) */
    Apply apply = Apply::Live;
    bool secret = false;    /* write-only: get reads {"set":bool} */
    bool danger = false;    /* the app asks before sending it */
    bool read_only = false; /* get only */
    bool supported = true;  /* false: not on this board (hidden; get/set -> unsupported) */
    std::string hint;       /* one line the app shows under the row */
    Getter get;
    Setter set;
    Check check;
};

/** A one-shot command (settings.action). */
struct ActionDef {
    std::string id;
    std::string group;
    std::string title;
    bool danger = false;
    bool supported = true;
    std::string hint;
    /** Run it. `params` is the "p" object (may be empty). Return false with `err` set on failure. */
    std::function<bool(const pagerlink::JsonObject &params, pagerlink::JsonWriter &result, Error &err)> run;
};

struct Group {
    std::string id;
    std::string title;
};

class Registry {
public:
    using Observer = std::function<void(uint32_t rev, const std::vector<std::string> &ids)>;

    /** `rev0` seeds the revision (boot-random on the device, §4.3). */
    explicit Registry(uint32_t rev0 = 1);

    /** Remove every group, def, action and observer; reseed the revision. */
    void clear(uint32_t rev0 = 1);

    void add_group(const std::string &id, const std::string &title);
    void add(const Def &d);
    void add_action(const ActionDef &a);

    /** Attach the store/apply functions to a def added by the catalog. False: no such id. */
    bool bind(const std::string &id, Getter get, Setter set = Setter(), Check check = Check());
    /** Attach the run function to an action. False: no such id. */
    bool bind_action(const std::string &id,
                     std::function<bool(const pagerlink::JsonObject &, pagerlink::JsonWriter &, Error &)> run);

    /** The def for `id`, or nullptr. Pointers stay valid until clear()/add(). */
    const Def *find(const std::string &id) const;
    const ActionDef *find_action(const std::string &id) const;
    const std::vector<Def> &defs() const { return defs_; }

    uint32_t rev() const;
    /** Hex FNV-1a of the whole schema (every page), cached. */
    std::string rev_schema() const;

    /** Type/range/enum/length/charset + the def's check. No side effects. */
    bool validate(const Def &d, const Value &v, Error &err) const;

    /** Read one value (a secret reads Bool: whether it is set). */
    bool get(const std::string &id, Value &out, Error &err) const;

    /** Validate and write one value (the pager UI path). */
    bool set(const std::string &id, const Value &v, Error &err);

    /**
     * All or nothing: validate every value first, then write each changed one.
     * `applied` gets the ids that changed, `reboot` whether any of them needs
     * a reboot. A setter failing after validation stops the batch; ids written
     * before it stay written (and are reported to observers).
     */
    bool set_many(const std::vector<std::pair<std::string, Value>> &vals, Error &err,
                  std::vector<std::string> *applied = nullptr, bool *reboot = nullptr);

    /**
     * A store changed outside the registry (an app wrote it directly). Bumps
     * the revision and notifies the observers.
     */
    void note_changed(const std::vector<std::string> &ids);

    int subscribe(Observer fn);
    void unsubscribe(int token);

    /* --- PagerLink JSON (PROTOCOL.md §6.4) --- */

    /** settings.schema: params {"cursor"?}. Each page's result stays under `max_bytes`. */
    bool schema_json(const pagerlink::JsonObject &params, pagerlink::JsonWriter &out, Error &err,
                     size_t max_bytes = 3500) const;
    /** settings.get: params {"ids"?:[...]}. */
    bool get_json(const pagerlink::JsonObject &params, pagerlink::JsonWriter &out, Error &err) const;
    /** settings.set: params {"values":{...},"if_rev"?}. */
    bool set_json(const pagerlink::JsonObject &params, pagerlink::JsonWriter &out, Error &err);

private:
    void encode_item(const Def &d, pagerlink::JsonWriter &w) const;
    void encode_action(const ActionDef &a, pagerlink::JsonWriter &w) const;
    void encode_value(const Def &d, pagerlink::JsonWriter &w) const;
    /** Validate + unsupported/read-only checks for one (id, value). */
    bool check_one(const std::string &id, const Value &v, const Def *&d, Error &err) const;
    void notify(uint32_t rev, const std::vector<std::string> &ids);

    mutable std::recursive_mutex mu_;
    std::vector<Group> groups_;
    std::vector<Def> defs_;
    std::vector<ActionDef> actions_;
    std::vector<std::pair<int, Observer>> observers_;
    int next_token_ = 1;
    uint32_t rev_;
    mutable std::string rev_schema_; /* "" = not computed yet */
};

}  // namespace settings

/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/settings_registry.{h,cpp} and
 *            hal/settings_catalog.{h,cpp}.
 *
 * Drives the real catalog (pager and watch board profiles) through the
 * registry on the host: type/range/step/enum/length validation, the
 * all-or-nothing batch set, the revision counter and the no-rewrite rule, the
 * reboot hint, if_rev, secret write-only reads, observers, the board gates and
 * the settings.schema paging (page size, cursor walk, group/action header on
 * the first page only). Every def is bound to a small in-memory store here;
 * on the device settings_table.cpp binds the same ids to the HAL.
 */
#include <unity.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hal/settings_registry.h"
#include "hal/settings_catalog.h"
#include "hal/link/link_json.h"
#include "settings_vectors_gen.h" /* kSettingsVectorsJson: protocol/vectors/settings.json */

using namespace settings;

void setUp(void) {}
void tearDown(void) {}

/* --------------------------------------------------------------- fixtures */

/** The fake store: one Value per def id, bound by bind_all(). */
static std::map<std::string, Value> g_store;
/** Successful setter calls: proves a no-op write never reaches the store. */
static int g_writes = 0;

static BoardProfile pager_profile()
{
    BoardProfile b;
    b.min_brightness = 0;
    b.max_brightness = 16;
    b.keyboard = true;
    b.otg = true;
    b.ssh = true;
    b.gps = false;
    b.nfc = true;
    for (int ma = 64; ma <= 2048; ma += 64) b.charge_ma.push_back(static_cast<uint16_t>(ma));
    b.tz_names = {"Europe/Paris", "America/New_York"};
    b.home_apps = {{"Editor", "Notes"}, {"Tasks", "Tasks"}};
    return b;
}

static BoardProfile watch_profile()
{
    BoardProfile b;
    b.min_brightness = 0;
    b.max_brightness = 255;
    b.keyboard = false;
    b.otg = false;
    b.ssh = false;
    b.gps = false;
    b.nfc = false;
    for (uint16_t ma : {125, 150, 175, 200, 300, 400, 500, 600, 700, 800, 900, 1000}) {
        b.charge_ma.push_back(ma);
    }
    b.tz_names = {"Europe/Paris", "America/New_York"};
    b.home_apps = {{"Editor", "Notes"}, {"Tasks", "Tasks"}};
    return b;
}

/** The value a def's store starts at: its minimum / first choice / empty. */
static Value default_value(const Def &d)
{
    switch (d.type) {
    case Type::Bool: return Value::of_bool(false);
    case Type::Int: return Value::of_int(d.min);
    case Type::Enum: return d.options.empty() ? Value() : d.options.front().v;
    case Type::String: return Value::of_str("");
    case Type::Secret: return Value::of_bool(false);
    case Type::List: return Value::of_json("[]");
    case Type::Object: return Value::of_json("{}");
    }
    return Value();
}

/** Bind every catalog def to g_store: getter everywhere, setter unless read-only. */
static void bind_all(Registry &r)
{
    const std::vector<Def> defs = r.defs();
    for (const Def &d : defs) {
        const std::string id = d.id;
        g_store[id] = default_value(d);
        if (d.read_only) {
            r.bind(id, [id]() { return g_store[id]; });
            continue;
        }
        const Type type = d.type;
        r.bind(
            id, [id]() { return g_store[id]; },
            [id, type](const Value &v, Error &) {
                ++g_writes;
                /* Write-only secrets read back as "is set", never the text. */
                if (type == Type::Secret) g_store[id] = Value::of_bool(!v.s.empty());
                else g_store[id] = v;
                return true;
            });
    }
}

/** Fresh empty store, the catalog for `b`, everything bound. */
static void make(Registry &r, const BoardProfile &b)
{
    g_store.clear();
    g_writes = 0;
    r.clear(100);
    build_catalog(r, b);
    bind_all(r);
}

static pagerlink::JsonObject parse(const std::string &text)
{
    pagerlink::JsonObject o;
    TEST_ASSERT_TRUE(o.parse(text));
    return o;
}

/** Every supported id, in catalog order: the order settings.schema must use. */
static std::vector<std::string> supported_ids(const Registry &r)
{
    std::vector<std::string> ids;
    for (const Def &d : r.defs()) {
        if (d.supported) ids.push_back(d.id);
    }
    return ids;
}

/** One settings.schema walk: the pages, their sizes and the ids they carry. */
struct SchemaWalk {
    std::vector<std::string> pages;
    std::vector<size_t> sizes;
    std::vector<std::string> ids;
    size_t largest_item = 0; /* biggest encoded item, so the page bound is known */
};

static SchemaWalk walk_schema(const Registry &r, size_t max_bytes)
{
    SchemaWalk walk;
    std::string cursor;
    bool first_page = true;
    for (int page_no = 0; page_no < 50; ++page_no) {
        pagerlink::JsonWriter params_w;
        params_w.begin_object();
        if (!first_page) params_w.key("cursor").str_val(cursor);
        params_w.end_object();
        pagerlink::JsonObject params = parse(params_w.str());

        pagerlink::JsonWriter out;
        Error err;
        TEST_ASSERT_TRUE(r.schema_json(params, out, err, max_bytes));
        TEST_ASSERT_TRUE(walk.pages.size() < 50);
        walk.pages.push_back(out.str());
        walk.sizes.push_back(out.str().size());

        pagerlink::JsonObject page = parse(out.str());
        std::string raw;
        TEST_ASSERT_TRUE(page.get_raw("items", raw));
        std::vector<std::string> items;
        TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, items));
        for (const std::string &item : items) {
            pagerlink::JsonObject o = parse(item);
            std::string id;
            TEST_ASSERT_TRUE(o.get_string("id", id));
            walk.ids.push_back(id);
            if (item.size() > walk.largest_item) walk.largest_item = item.size();
        }

        if (!page.has("cursor")) return walk;
        TEST_ASSERT_TRUE(page.get_string("cursor", cursor));
        first_page = false;
    }
    TEST_FAIL_MESSAGE("settings.schema did not finish within 50 pages");
    return walk;
}

static void expect_code(const Error &err, const char *code)
{
    TEST_ASSERT_EQUAL_STRING(code, err.code.c_str());
}

/* ------------------------------------------------------------ validation */

static void test_int_ranges(void)
{
    Registry r;
    make(r, pager_profile());

    int seen = 0;
    for (const Def &d : r.defs()) {
        if (d.type != Type::Int) continue;
        ++seen;
        Error err;
        TEST_ASSERT_TRUE(r.validate(d, Value::of_int(d.min), err));
        TEST_ASSERT_TRUE(r.validate(d, Value::of_int(d.max), err));

        err = Error();
        TEST_ASSERT_FALSE(r.validate(d, Value::of_int(d.min - 1), err));
        expect_code(err, "invalid_argument");

        err = Error();
        TEST_ASSERT_FALSE(r.validate(d, Value::of_int(d.max + 1), err));
        expect_code(err, "invalid_argument");

        if (d.step > 1) {
            err = Error();
            TEST_ASSERT_FALSE(r.validate(d, Value::of_int(d.min + 1), err));
            expect_code(err, "invalid_argument");
        }

        err = Error();
        TEST_ASSERT_FALSE(r.validate(d, Value::of_bool(true), err));
        expect_code(err, "invalid_argument");

        err = Error();
        TEST_ASSERT_FALSE(r.validate(d, Value::of_str("1"), err));
        expect_code(err, "invalid_argument");
    }
    TEST_ASSERT_TRUE(seen > 0);
}

static void test_bool_enum_string(void)
{
    Registry r;
    make(r, pager_profile());

    int bools = 0, enums = 0, strings = 0;
    for (const Def &d : r.defs()) {
        Error err;
        if (d.type == Type::Bool) {
            ++bools;
            TEST_ASSERT_FALSE(r.validate(d, Value::of_int(1), err));
            expect_code(err, "invalid_argument");
            err = Error();
            TEST_ASSERT_TRUE(r.validate(d, Value::of_bool(true), err));
        } else if (d.type == Type::Enum) {
            ++enums;
            TEST_ASSERT_FALSE(d.options.empty());
            TEST_ASSERT_TRUE(r.validate(d, d.options.front().v, err));
            err = Error();
            TEST_ASSERT_FALSE(r.validate(d, Value::of_str("zzz-not-an-option"), err));
            expect_code(err, "invalid_argument");
        } else if (d.type == Type::String || d.type == Type::Secret) {
            ++strings;
            const size_t max = d.max_len ? d.max_len : 255;
            err = Error();
            TEST_ASSERT_FALSE(r.validate(d, Value::of_str(std::string(max + 1, 'a')), err));
            expect_code(err, "invalid_argument");
            err = Error();
            TEST_ASSERT_FALSE(r.validate(d, Value::of_str(std::string("a\x01") + "b"), err));
            expect_code(err, "invalid_argument");
            err = Error();
            TEST_ASSERT_TRUE(r.validate(d, Value::of_str(""), err));
        }
    }
    TEST_ASSERT_TRUE(bools > 0);
    TEST_ASSERT_TRUE(enums > 0);
    TEST_ASSERT_TRUE(strings > 0);
}

static void test_brightness_per_board(void)
{
    Registry pager;
    make(pager, pager_profile());
    const Def *bright = pager.find("display.brightness");
    TEST_ASSERT_NOT_NULL(bright);
    TEST_ASSERT_EQUAL_INT64(0, bright->min);
    TEST_ASSERT_EQUAL_INT64(16, bright->max);

    Registry watch;
    make(watch, watch_profile());
    const Def *dim = watch.find("display.brightness");
    TEST_ASSERT_NOT_NULL(dim);
    TEST_ASSERT_EQUAL_INT64(255, dim->max);
}

/* ----------------------------------------------------------------- gating */

static void test_gating_watch(void)
{
    Registry watch;
    make(watch, watch_profile());

    static const char *const gated[] = {"power.otg", "display.kb_backlight", "ssh.host"};
    for (const char *id : gated) {
        pagerlink::JsonObject p = parse(std::string("{\"ids\":[\"") + id + "\"]}");
        pagerlink::JsonWriter out;
        Error err;
        TEST_ASSERT_FALSE(watch.get_json(p, out, err));
        expect_code(err, "unsupported");
    }

    pagerlink::JsonObject setp = parse("{\"values\":{\"power.otg\":true}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_FALSE(watch.set_json(setp, out, err));
    expect_code(err, "unsupported");
    TEST_ASSERT_EQUAL_INT(0, g_writes);

    std::string watch_schema;
    for (const std::string &page : walk_schema(watch, 3500).pages) watch_schema += page;
    for (const char *id : gated) {
        TEST_ASSERT_TRUE(watch_schema.find(id) == std::string::npos);
    }

    Registry pager;
    make(pager, pager_profile());
    std::string pager_schema;
    for (const std::string &page : walk_schema(pager, 3500).pages) pager_schema += page;
    for (const char *id : gated) {
        TEST_ASSERT_TRUE(pager_schema.find(id) != std::string::npos);
    }
}

/* ------------------------------------------------------- schema paging */

static void check_schema_paging(const BoardProfile &b)
{
    Registry r;
    make(r, b);
    const std::vector<std::string> want = supported_ids(r);

    /* Default budget: the cursor walk must yield every supported id, in order. */
    const SchemaWalk full = walk_schema(r, 3500);
    TEST_ASSERT_TRUE(full.pages.size() > 1);
    TEST_ASSERT_EQUAL((int)want.size(), (int)full.ids.size());
    for (size_t i = 0; i < want.size() && i < full.ids.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(want[i].c_str(), full.ids[i].c_str());
    }
    for (size_t size : full.sizes) TEST_ASSERT_TRUE(size < 4096);

    /* groups + actions ride on the first page only; every page shares rev_schema. */
    pagerlink::JsonObject head = parse(full.pages.front());
    TEST_ASSERT_TRUE(head.has("groups"));
    TEST_ASSERT_TRUE(head.has("actions"));
    for (size_t i = 1; i < full.pages.size(); ++i) {
        pagerlink::JsonObject page = parse(full.pages[i]);
        TEST_ASSERT_FALSE(page.has("groups"));
        TEST_ASSERT_FALSE(page.has("actions"));
    }
    const std::string rev_schema = r.rev_schema();
    for (const std::string &page : full.pages) {
        std::string got;
        TEST_ASSERT_TRUE(parse(page).get_string("rev_schema", got));
        TEST_ASSERT_EQUAL_STRING(rev_schema.c_str(), got.c_str());
    }

    /* A budget far below the default still covers every id. The first page
     * also carries the groups/actions header, which schema_json writes
     * before it starts budgeting items (and it always emits at least one
     * item), so only the later pages are bounded by max_bytes + one item. */
    const SchemaWalk tight = walk_schema(r, 600);
    TEST_ASSERT_TRUE(tight.pages.size() > full.pages.size());
    TEST_ASSERT_TRUE(tight.largest_item > 0);
    TEST_ASSERT_EQUAL((int)want.size(), (int)tight.ids.size());
    for (size_t i = 0; i < want.size() && i < tight.ids.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(want[i].c_str(), tight.ids[i].c_str());
    }
    TEST_ASSERT_TRUE(tight.sizes.front() < 4096);
    for (size_t i = 1; i < tight.sizes.size(); ++i) {
        TEST_ASSERT_TRUE(tight.sizes[i] < 600 + tight.largest_item);
    }
}

static void test_schema_paging(void)
{
    check_schema_paging(pager_profile());
    check_schema_paging(watch_profile());
}

/* ------------------------------------------------------------ set_many */

static void test_all_or_nothing(void)
{
    Registry r;
    make(r, pager_profile());
    TEST_ASSERT_EQUAL_INT64(0, g_store["display.brightness"].i);

    pagerlink::JsonObject p =
        parse("{\"values\":{\"display.brightness\":5,\"display.timeout_s\":999}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_FALSE(r.set_json(p, out, err));
    expect_code(err, "invalid_argument");
    TEST_ASSERT_TRUE(err.msg.rfind("display.timeout_s", 0) == 0);

    TEST_ASSERT_EQUAL_INT64(0, g_store["display.brightness"].i);
    TEST_ASSERT_EQUAL_INT(0, g_writes);
    TEST_ASSERT_EQUAL_UINT32(100, r.rev());
}

static void test_set_bumps_rev_once(void)
{
    Registry r;
    make(r, pager_profile());

    const pagerlink::JsonObject p =
        parse("{\"values\":{\"display.brightness\":5,\"display.timeout_s\":30}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_TRUE(r.set_json(p, out, err));

    pagerlink::JsonObject res = parse(out.str());
    int64_t rev = 0;
    TEST_ASSERT_TRUE(res.get_int("rev", rev));
    TEST_ASSERT_EQUAL_INT64(101, rev);
    bool reboot = true;
    TEST_ASSERT_TRUE(res.get_bool("reboot", reboot));
    TEST_ASSERT_FALSE(reboot);

    std::string applied_raw;
    std::vector<std::string> applied;
    TEST_ASSERT_TRUE(res.get_raw("applied", applied_raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(applied_raw, applied));
    TEST_ASSERT_EQUAL(2, (int)applied.size());
    TEST_ASSERT_EQUAL_STRING("\"display.brightness\"", applied[0].c_str());
    TEST_ASSERT_EQUAL_STRING("\"display.timeout_s\"", applied[1].c_str());
    TEST_ASSERT_EQUAL_INT(2, g_writes);

    /* The identical call again changes nothing: no write, no new revision. */
    pagerlink::JsonWriter again;
    Error again_err;
    TEST_ASSERT_TRUE(r.set_json(p, again, again_err));
    pagerlink::JsonObject res2 = parse(again.str());
    int64_t rev2 = 0;
    TEST_ASSERT_TRUE(res2.get_int("rev", rev2));
    TEST_ASSERT_EQUAL_INT64(101, rev2);
    TEST_ASSERT_TRUE(res2.get_raw("applied", applied_raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(applied_raw, applied));
    TEST_ASSERT_EQUAL(0, (int)applied.size());
    TEST_ASSERT_EQUAL_INT(2, g_writes);
}

static void test_reboot_flag(void)
{
    Registry r;
    make(r, pager_profile());

    pagerlink::JsonObject p = parse("{\"values\":{\"storage.msc_target\":\"int\"}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_TRUE(r.set_json(p, out, err));

    pagerlink::JsonObject res = parse(out.str());
    bool reboot = false;
    TEST_ASSERT_TRUE(res.get_bool("reboot", reboot));
    TEST_ASSERT_TRUE(reboot);
    TEST_ASSERT_EQUAL_STRING("int", g_store["storage.msc_target"].s.c_str());
}

static void test_if_rev(void)
{
    Registry r;
    make(r, pager_profile());

    pagerlink::JsonObject p = parse("{\"values\":{\"display.brightness\":5},\"if_rev\":100}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_TRUE(r.set_json(p, out, err));
    TEST_ASSERT_EQUAL_UINT32(101, r.rev());

    pagerlink::JsonObject stale = parse("{\"values\":{\"display.brightness\":6},\"if_rev\":100}");
    pagerlink::JsonWriter out2;
    Error err2;
    TEST_ASSERT_FALSE(r.set_json(stale, out2, err2));
    expect_code(err2, "invalid_argument");
    TEST_ASSERT_EQUAL_STRING("changed", err2.msg.c_str());
    TEST_ASSERT_EQUAL_INT64(5, g_store["display.brightness"].i);
}

static void test_secret_write_only(void)
{
    Registry r;
    make(r, pager_profile());

    const pagerlink::JsonObject one = parse("{\"ids\":[\"nsync.token\"]}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_TRUE(r.get_json(one, out, err));
    pagerlink::JsonObject values;
    TEST_ASSERT_TRUE(parse(out.str()).get_object("values", values));
    std::string raw;
    TEST_ASSERT_TRUE(values.get_raw("nsync.token", raw));
    TEST_ASSERT_EQUAL_STRING("{\"set\":false}", raw.c_str());

    pagerlink::JsonObject setp = parse("{\"values\":{\"nsync.token\":\"ghp_secretvalue\"}}");
    pagerlink::JsonWriter out2;
    Error err2;
    TEST_ASSERT_TRUE(r.set_json(setp, out2, err2));

    pagerlink::JsonWriter out3;
    Error err3;
    TEST_ASSERT_TRUE(r.get_json(one, out3, err3));
    TEST_ASSERT_TRUE(parse(out3.str()).get_object("values", values));
    TEST_ASSERT_TRUE(values.get_raw("nsync.token", raw));
    TEST_ASSERT_EQUAL_STRING("{\"set\":true}", raw.c_str());

    pagerlink::JsonObject all = parse("{}");
    pagerlink::JsonWriter out4;
    Error err4;
    TEST_ASSERT_TRUE(r.get_json(all, out4, err4));
    TEST_ASSERT_TRUE(out4.str().find("ghp_secretvalue") == std::string::npos);
    TEST_ASSERT_TRUE(out4.str().find("\"nsync.token\":{\"set\":true}") != std::string::npos);
}

static void test_unknown_and_readonly(void)
{
    Registry r;
    make(r, pager_profile());

    pagerlink::JsonObject unknown = parse("{\"values\":{\"nope.x\":true}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_FALSE(r.set_json(unknown, out, err));
    expect_code(err, "invalid_argument");
    TEST_ASSERT_TRUE(err.msg.rfind("nope.x", 0) == 0);

    pagerlink::JsonObject saved = parse("{\"values\":{\"wifi.saved\":[]}}");
    pagerlink::JsonWriter out2;
    Error err2;
    TEST_ASSERT_FALSE(r.set_json(saved, out2, err2));
    expect_code(err2, "invalid_argument");
    TEST_ASSERT_TRUE(err2.msg.find("read-only") != std::string::npos);

    pagerlink::JsonObject sec = parse("{\"values\":{\"notes_sec.state\":\"off\"}}");
    pagerlink::JsonWriter out3;
    Error err3;
    TEST_ASSERT_FALSE(r.set_json(sec, out3, err3));
    expect_code(err3, "invalid_argument");
    TEST_ASSERT_TRUE(err3.msg.find("read-only") != std::string::npos);

    TEST_ASSERT_EQUAL_INT(0, g_writes);
}

/* ------------------------------------------------------------ observers */

static void test_observer(void)
{
    Registry r;
    make(r, pager_profile());

    int calls = 0;
    uint32_t last_rev = 0;
    std::vector<std::string> last_ids;
    const int token = r.subscribe([&calls, &last_rev, &last_ids](
                                       uint32_t rev, const std::vector<std::string> &ids) {
        ++calls;
        last_rev = rev;
        last_ids = ids;
    });

    const pagerlink::JsonObject p =
        parse("{\"values\":{\"display.brightness\":5,\"display.timeout_s\":30}}");
    pagerlink::JsonWriter out;
    Error err;
    TEST_ASSERT_TRUE(r.set_json(p, out, err));
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_UINT32(101, last_rev);
    TEST_ASSERT_EQUAL(2, (int)last_ids.size());
    TEST_ASSERT_EQUAL_STRING("display.brightness", last_ids[0].c_str());
    TEST_ASSERT_EQUAL_STRING("display.timeout_s", last_ids[1].c_str());

    /* Unchanged values are not written, so no observer runs. */
    pagerlink::JsonWriter out2;
    Error err2;
    TEST_ASSERT_TRUE(r.set_json(p, out2, err2));
    TEST_ASSERT_EQUAL_INT(1, calls);

    r.unsubscribe(token);
    pagerlink::JsonObject third = parse("{\"values\":{\"display.brightness\":9}}");
    pagerlink::JsonWriter out3;
    Error err3;
    TEST_ASSERT_TRUE(r.set_json(third, out3, err3));
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_UINT32(102, r.rev());
}

/* --------------------------------------------------------------- values */

static void test_value_from_json(void)
{
    Value v;

    TEST_ASSERT_TRUE(value_from_json("true", v));
    TEST_ASSERT_TRUE(v.kind == Value::Bool);
    TEST_ASSERT_TRUE(v.b);

    TEST_ASSERT_TRUE(value_from_json("-3", v));
    TEST_ASSERT_TRUE(v.kind == Value::Int);
    TEST_ASSERT_EQUAL_INT64(-3, v.i);

    TEST_ASSERT_TRUE(value_from_json("\"x\"", v));
    TEST_ASSERT_TRUE(v.kind == Value::Str);
    TEST_ASSERT_EQUAL_STRING("x", v.s.c_str());

    TEST_ASSERT_TRUE(value_from_json("[1]", v));
    TEST_ASSERT_TRUE(v.kind == Value::Json);
    TEST_ASSERT_EQUAL_STRING("[1]", v.s.c_str());

    TEST_ASSERT_TRUE(value_from_json("{\"a\":1}", v));
    TEST_ASSERT_TRUE(v.kind == Value::Json);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", v.s.c_str());

    TEST_ASSERT_FALSE(value_from_json("1.5", v));

    TEST_ASSERT_TRUE(value_from_json("null", v));
    TEST_ASSERT_TRUE(v.kind == Value::None);
}

static void test_url_ok(void)
{
    TEST_ASSERT_TRUE(url_ok(""));
    TEST_ASSERT_TRUE(url_ok("http://h"));
    TEST_ASSERT_TRUE(url_ok("https://h:8080/x"));
    TEST_ASSERT_FALSE(url_ok("ftp://h"));
    TEST_ASSERT_FALSE(url_ok("http://"));
    TEST_ASSERT_FALSE(url_ok("http:// h"));
}

/* ---------------------------------------------------- protocol vectors */

/* Wire type name -> Type (the vectors only use the documented names). */
static Type wire_type(const std::string &name)
{
    if (name == "bool") return Type::Bool;
    if (name == "int") return Type::Int;
    if (name == "enum") return Type::Enum;
    if (name == "string") return Type::String;
    if (name == "secret") return Type::Secret;
    if (name == "list") return Type::List;
    if (name == "object") return Type::Object;
    TEST_FAIL_MESSAGE(("unknown setting type: " + name).c_str());
    return Type::Bool;
}

/* Wire apply name -> Apply. Missing/unknown keeps Def's default (Live). */
static Apply wire_apply(const std::string &name)
{
    if (name == "live") return Apply::Live;
    if (name == "tick") return Apply::Tick;
    if (name == "theme") return Apply::Theme;
    if (name == "sleep") return Apply::NextSleep;
    if (name == "reboot") return Apply::Reboot;
    TEST_FAIL_MESSAGE(("unknown apply mode: " + name).c_str());
    return Apply::Live;
}

/**
 * Rebuild the registry the vectors were generated from: rev0, groups, one
 * def per catalog item, the actions, then every def bound to `store` seeded
 * from catalog.initial (read-only items get a getter only).
 */
static void build_from_catalog(settings::Registry &r, const pagerlink::JsonObject &root,
                               std::map<std::string, settings::Value> &store)
{
    int64_t rev0 = 1;
    TEST_ASSERT_TRUE(root.get_int("rev0", rev0));
    pagerlink::JsonObject catalog;
    TEST_ASSERT_TRUE(root.get_object("catalog", catalog));

    r.clear(static_cast<uint32_t>(rev0));

    std::string raw;
    std::vector<std::string> elems;

    TEST_ASSERT_TRUE(catalog.get_raw("groups", raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, elems));
    for (const std::string &elem : elems) {
        pagerlink::JsonObject g;
        TEST_ASSERT_TRUE(g.parse(elem));
        std::string id, title;
        TEST_ASSERT_TRUE(g.get_string("id", id));
        TEST_ASSERT_TRUE(g.get_string("title", title));
        r.add_group(id, title);
    }

    pagerlink::JsonObject initial;
    TEST_ASSERT_TRUE(catalog.get_object("initial", initial));

    TEST_ASSERT_TRUE(catalog.get_raw("items", raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, elems));
    for (const std::string &elem : elems) {
        pagerlink::JsonObject item;
        TEST_ASSERT_TRUE(item.parse(elem));

        Def d;
        std::string type_name;
        TEST_ASSERT_TRUE(item.get_string("id", d.id));
        TEST_ASSERT_TRUE(item.get_string("group", d.group));
        TEST_ASSERT_TRUE(item.get_string("title", d.title));
        TEST_ASSERT_TRUE(item.get_string("type", type_name));
        d.type = wire_type(type_name);

        if (item.has("min")) {
            int64_t v = 0;
            TEST_ASSERT_TRUE(item.get_int("min", v));
            d.min = v;
        }
        if (item.has("max")) {
            int64_t v = 0;
            TEST_ASSERT_TRUE(item.get_int("max", v));
            d.max = v;
        }
        if (item.has("step")) {
            int64_t v = 1;
            TEST_ASSERT_TRUE(item.get_int("step", v));
            d.step = v;
        }

        if (item.has("enum")) {
            std::string enum_raw;
            std::vector<std::string> choices;
            TEST_ASSERT_TRUE(item.get_raw("enum", enum_raw));
            TEST_ASSERT_TRUE(pagerlink::json_array_items(enum_raw, choices));
            for (const std::string &choice : choices) {
                pagerlink::JsonObject o;
                TEST_ASSERT_TRUE(o.parse(choice));
                std::string v_raw;
                TEST_ASSERT_TRUE(o.get_raw("v", v_raw));
                Option opt;
                TEST_ASSERT_TRUE(value_from_json(v_raw, opt.v));
                o.get_string("label", opt.label);
                d.options.push_back(opt);
            }
        }

        if (item.has("max_len")) {
            int64_t v = 0;
            TEST_ASSERT_TRUE(item.get_int("max_len", v));
            d.max_len = static_cast<uint16_t>(v);
        }

        std::string apply;
        if (item.get_string("apply", apply)) d.apply = wire_apply(apply);

        bool flag = false;
        if (item.get_bool("secret", flag)) d.secret = flag;
        if (item.get_bool("danger", flag)) d.danger = flag;
        if (item.get_bool("ro", flag)) d.read_only = flag;

        item.get_string("hint", d.hint);

        const std::string id = d.id;
        const bool secret = d.secret;
        const bool read_only = d.read_only;
        r.add(d);

        std::string init_raw;
        Value init;
        if (initial.get_raw(id.c_str(), init_raw)) {
            TEST_ASSERT_TRUE(value_from_json(init_raw, init));
        }
        store[id] = init;

        if (read_only) {
            r.bind(id, [&store, id]() { return store[id]; });
        } else {
            r.bind(
                id, [&store, id]() { return store[id]; },
                [&store, id, secret](const Value &v, Error &) {
                    /* Write-only secrets read back as "is set", never the text. */
                    if (secret) store[id] = Value::of_bool(!v.s.empty());
                    else store[id] = v;
                    return true;
                });
        }
    }

    TEST_ASSERT_TRUE(catalog.get_raw("actions", raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, elems));
    for (const std::string &elem : elems) {
        pagerlink::JsonObject action;
        TEST_ASSERT_TRUE(action.parse(elem));
        ActionDef a;
        TEST_ASSERT_TRUE(action.get_string("id", a.id));
        TEST_ASSERT_TRUE(action.get_string("group", a.group));
        TEST_ASSERT_TRUE(action.get_string("title", a.title));
        bool flag = false;
        if (action.get_bool("danger", flag)) a.danger = flag;
        action.get_string("hint", a.hint);
        r.add_action(a);
    }
}

/**
 * Every case in protocol/vectors/settings.json: a fresh registry rebuilt from
 * the embedded catalog must produce the recorded result (or error) byte for
 * byte. Every recorded event payload must also be a valid JSON object.
 */
static void test_golden_vectors(void)
{
    pagerlink::JsonObject root;
    TEST_ASSERT_TRUE(root.parse(kSettingsVectorsJson));

    std::string want_rev_schema;
    TEST_ASSERT_TRUE(root.get_string("rev_schema", want_rev_schema));

    std::string raw;
    std::vector<std::string> cases;
    TEST_ASSERT_TRUE(root.get_raw("cases", raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, cases));
    TEST_ASSERT_TRUE(!cases.empty());

    for (const std::string &elem : cases) {
        pagerlink::JsonObject cs;
        TEST_ASSERT_TRUE(cs.parse(elem));
        std::string name;
        TEST_ASSERT_TRUE(cs.get_string("name", name));

        /* Fresh store first, so the registry (which captures it) dies first. */
        std::map<std::string, Value> store;
        Registry r;
        build_from_catalog(r, root, store);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want_rev_schema.c_str(), r.rev_schema().c_str(),
                                         name.c_str());

        pagerlink::JsonObject params;
        TEST_ASSERT_TRUE(cs.get_object("params", params));
        std::string method;
        TEST_ASSERT_TRUE(cs.get_string("method", method));

        pagerlink::JsonWriter out;
        Error err;
        bool ok = false;
        if (method == "settings.schema") {
            ok = r.schema_json(params, out, err);
        } else if (method == "settings.get") {
            ok = r.get_json(params, out, err);
        } else if (method == "settings.set") {
            ok = r.set_json(params, out, err);
        } else {
            TEST_FAIL_MESSAGE(("unknown vector method: " + method).c_str());
        }

        std::string result_json;
        if (cs.get_string("result_json", result_json)) {
            TEST_ASSERT_TRUE_MESSAGE(ok, name.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(result_json.c_str(), out.str().c_str(),
                                             name.c_str());
        } else {
            pagerlink::JsonObject error;
            TEST_ASSERT_TRUE(cs.get_object("error", error));
            std::string code, msg;
            TEST_ASSERT_TRUE(error.get_string("code", code));
            TEST_ASSERT_TRUE(error.get_string("msg", msg));
            TEST_ASSERT_FALSE_MESSAGE(ok, name.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(code.c_str(), err.code.c_str(), name.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(msg.c_str(), err.msg.c_str(), name.c_str());
        }
    }

    std::vector<std::string> events;
    TEST_ASSERT_TRUE(root.get_raw("events", raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, events));
    for (const std::string &elem : events) {
        pagerlink::JsonObject event;
        TEST_ASSERT_TRUE(event.parse(elem));
        std::string p_json;
        TEST_ASSERT_TRUE(event.get_string("p_json", p_json));
        pagerlink::JsonObject payload;
        TEST_ASSERT_TRUE_MESSAGE(payload.parse(p_json), p_json.c_str());
    }
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_int_ranges);
    RUN_TEST(test_bool_enum_string);
    RUN_TEST(test_brightness_per_board);
    RUN_TEST(test_gating_watch);
    RUN_TEST(test_schema_paging);
    RUN_TEST(test_all_or_nothing);
    RUN_TEST(test_set_bumps_rev_once);
    RUN_TEST(test_reboot_flag);
    RUN_TEST(test_if_rev);
    RUN_TEST(test_secret_write_only);
    RUN_TEST(test_unknown_and_readonly);
    RUN_TEST(test_observer);
    RUN_TEST(test_value_from_json);
    RUN_TEST(test_url_ok);
    RUN_TEST(test_golden_vectors);
    return UNITY_END();
}

/**
 * @file      ui_time_sync.cpp
 * @brief     Timezone (city) selection + timezone-aware NTP sync.
 *
 * The device's Date & Time screen lets the user pick an IANA city; we use the
 * resulting POSIX TZ rule to compute the correct UTC offset (including DST).
 *
 * We previously used worldtimeapi.org for both the city list and the offset
 * lookup, but that service was retired and the round-trip was blocking the
 * UI. Everything here is now local: the list below is bundled at compile
 * time, and offsets are derived from newlib's TZ machinery via
 * setenv("TZ", ...) + localtime_r() + tm.tm_gmtoff.
 *
 * The user's chosen IANA name is persisted in NVS so it survives reboots and
 * is reused by the "Sync from Internet" button in the Date & Time settings
 * subpage. The default (when nothing is stored) is "America/Sao_Paulo".
 */
#include <string>
#include <vector>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

#define TZ_PREFS_NS  "timezone"
#define TZ_DEFAULT   "America/Sao_Paulo"

// IANA name → POSIX TZ rule. Curated list of ~80 widely-used zones; the POSIX
// rules encode DST transitions where applicable so tm_gmtoff is correct
// year-round without further lookups. Add entries here as needed — the
// picker surfaces them in insertion order, so keep regional groupings
// together for usability.
struct TzEntry {
    const char *name;
    const char *posix;
};

static const TzEntry TZ_TABLE[] = {
    // Americas — Sao Paulo first so it's the visible default.
    {"America/Sao_Paulo",      "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/Santiago",       "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America/Bogota",         "<-05>5"},
    {"America/Lima",           "<-05>5"},
    {"America/Caracas",        "<-04>4"},
    {"America/La_Paz",         "<-04>4"},
    {"America/Manaus",         "<-04>4"},
    {"America/Anchorage",      "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles",    "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Denver",         "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",        "MST7"},
    {"America/Chicago",        "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City",    "CST6CDT,M4.1.0,M10.5.0"},
    {"America/New_York",       "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto",        "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Halifax",        "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns",       "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Noronha",        "<-02>2"},

    // Atlantic / Europe / Africa.
    {"Atlantic/Azores",        "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"UTC",                    "UTC0"},
    {"Europe/London",          "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon",          "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",          "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Africa/Casablanca",      "<+01>-1"},
    {"Europe/Madrid",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris",           "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome",            "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Africa/Lagos",           "WAT-1"},
    {"Africa/Johannesburg",    "SAST-2"},
    {"Europe/Athens",          "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul",        "<+03>-3"},
    {"Africa/Cairo",           "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Nairobi",         "EAT-3"},
    {"Europe/Moscow",          "MSK-3"},

    // Middle East / Asia.
    {"Asia/Jerusalem",         "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Dubai",             "<+04>-4"},
    {"Asia/Baku",              "<+04>-4"},
    {"Asia/Tehran",            "<+0330>-3:30"},
    {"Asia/Kabul",             "<+0430>-4:30"},
    {"Asia/Karachi",           "PKT-5"},
    {"Asia/Tashkent",          "<+05>-5"},
    {"Asia/Kolkata",           "IST-5:30"},
    {"Asia/Colombo",           "<+0530>-5:30"},
    {"Asia/Kathmandu",         "<+0545>-5:45"},
    {"Asia/Dhaka",             "<+06>-6"},
    {"Asia/Yangon",            "<+0630>-6:30"},
    {"Asia/Bangkok",           "<+07>-7"},
    {"Asia/Jakarta",           "WIB-7"},
    {"Asia/Ho_Chi_Minh",       "<+07>-7"},
    {"Asia/Singapore",         "<+08>-8"},
    {"Asia/Kuala_Lumpur",      "<+08>-8"},
    {"Asia/Hong_Kong",         "HKT-8"},
    {"Asia/Taipei",            "CST-8"},
    {"Asia/Shanghai",          "CST-8"},
    {"Asia/Manila",            "PST-8"},
    {"Asia/Seoul",             "KST-9"},
    {"Asia/Tokyo",             "JST-9"},

    // Oceania.
    {"Australia/Perth",        "AWST-8"},
    {"Australia/Adelaide",     "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Darwin",       "ACST-9:30"},
    {"Australia/Brisbane",     "AEST-10"},
    {"Australia/Sydney",       "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Hobart",       "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Guam",           "ChST-10"},
    {"Pacific/Noumea",         "<+11>-11"},
    {"Pacific/Auckland",       "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Fiji",           "<+12>-12"},
    {"Pacific/Tongatapu",      "<+13>-13"},
    {"Pacific/Honolulu",       "HST10"},
};

static const char *tz_find_posix(const char *name)
{
    if (!name || !*name) return nullptr;
    for (const TzEntry &e : TZ_TABLE) {
        if (strcmp(e.name, name) == 0) return e.posix;
    }
    return nullptr;
}

std::string timezone_get_user_tz()
{
#ifdef ARDUINO
    Preferences p;
    if (p.begin(TZ_PREFS_NS, true)) {
        String s = p.getString("tz", "");
        p.end();
        if (s.length()) return std::string(s.c_str());
    }
#endif
    return std::string(TZ_DEFAULT);
}

void timezone_set_user_tz(const char *tz)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(TZ_PREFS_NS, false)) return;
    if (tz && *tz) p.putString("tz", tz);
    else           p.remove("tz");
    p.end();

    // Apply the new zone to the C library env so localtime()/mktime() use
    // it immediately. Without this, the picker would only take effect on
    // the next reboot or NTP run — which is what bit us before factory.ino
    // started seeding TZ at boot.
    const char *posix = tz_find_posix((tz && *tz) ? tz : TZ_DEFAULT);
    if (posix) {
        setenv("TZ", posix, 1);
        tzset();
    }
#else
    (void)tz;
#endif
}

// Returns the POSIX TZ rule for the user's selected zone (or the default).
// Exposed so the NTP sync path can hand it to configTzTime() directly.
const char *timezone_get_user_posix()
{
    std::string tz = timezone_get_user_tz();
    const char *p = tz_find_posix(tz.c_str());
    if (p) return p;
    return tz_find_posix(TZ_DEFAULT);
}

void timezone_fetch_list(std::vector<std::string> &out)
{
    out.clear();
    out.reserve(sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]));
    for (const TzEntry &e : TZ_TABLE) out.emplace_back(e.name);
}

// Compute the total UTC offset (DST included) for `tz` using newlib's TZ
// machinery, at the current wall time. Restores the previous TZ env var
// before returning so we don't perturb anyone else's view of time.
bool timezone_fetch_offset(const char *tz, int &offset_sec, std::string &err)
{
    offset_sec = 0;

    const char *posix = tz_find_posix(tz);
    if (!posix) {
        err = "unknown timezone";
        return false;
    }

#ifdef ARDUINO
    const char *prev = getenv("TZ");
    std::string saved = prev ? std::string(prev) : std::string();
    setenv("TZ", posix, 1);
    tzset();

    time_t now = time(nullptr);
    if (now < 24 * 3600) now = 24 * 3600;  // pre-sync clock sentinel

    // ESP32 newlib doesn't expose tm_gmtoff, so derive the offset ourselves:
    // treat localtime(now) and gmtime(now) as input to mktime() (which
    // interprets its arg as local time under the current TZ). mktime(lt)
    // returns `now`; mktime(gm) returns `now - offset`, so the difference
    // is the UTC offset we want, DST included.
    struct tm lt, gm;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gm);
    gm.tm_isdst = 0;
    long gmtoff = (long)difftime(mktime(&lt), mktime(&gm));

    if (prev) setenv("TZ", saved.c_str(), 1);
    else      unsetenv("TZ");
    tzset();

    offset_sec = (int)gmtoff;
    return true;
#else
    (void)err;
    return false;
#endif
}

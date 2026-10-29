/**
 * @file      settings_catalog.cpp
 * @brief     The settings catalog (see settings_catalog.h). Pure C++.
 *
 * Groups are listed in the pager's Settings tile order; the app renders one
 * section per group in this order. Ids are wire ids: never rename one.
 */
#include "settings_catalog.h"
#include "ssh_hosts.h"

namespace settings {

const FontContext kFontContexts[] = {
    {"system", "System"},     {"home", "Home"},         {"notes", "Notes"},
    {"journal", "Journal"},   {"weather", "Weather"},   {"telegram", "Telegram"},
    {"ssh", "SSH"},           {"chat", "Chat"},         {"header", "Header"},
};
const size_t kFontContextCount = sizeof(kFontContexts) / sizeof(kFontContexts[0]);

const char *const kFontFaces[] = {"montserrat", "unscii8", "unscii16", "courier",
                                  "inter",      "atkinson", "jetbrains"};
const size_t kFontFaceCount = sizeof(kFontFaces) / sizeof(kFontFaces[0]);

static const char *const kFontFaceLabels[] = {
    "Montserrat (ASCII)", "Unscii 8", "Unscii 16", "Courier",
    "Inter (Latin-1)", "Atkinson (Latin-1)", "JetBrains Mono"};

bool url_ok(const std::string &u) {
    if (u.empty()) return true;
    size_t host;
    if (u.compare(0, 7, "http://") == 0) host = 7;
    else if (u.compare(0, 8, "https://") == 0) host = 8;
    else return false;
    if (u.size() <= host || u[host] == '/' || u[host] == ':') return false;
    for (char c : u) {
        if (c == ' ') return false;
    }
    return true;
}

namespace {

Option opt_s(const char *v, const char *label = "") {
    Option o;
    o.v = Value::of_str(v);
    o.label = label;
    return o;
}

Option opt_i(int64_t v, const std::string &label = "") {
    Option o;
    o.v = Value::of_int(v);
    o.label = label;
    return o;
}

Def make(const char *id, const char *group, const char *title, Type t,
         Apply a = Apply::Live) {
    Def d;
    d.id = id;
    d.group = group;
    d.title = title;
    d.type = t;
    d.apply = a;
    return d;
}

Def make_int(const char *id, const char *group, const char *title, int64_t mn,
             int64_t mx, int64_t step, Apply a = Apply::Live) {
    Def d = make(id, group, title, Type::Int, a);
    d.min = mn;
    d.max = mx;
    d.step = step;
    return d;
}

Def make_str(const char *id, const char *group, const char *title, uint16_t max_len,
             const char *hint = "") {
    Def d = make(id, group, title, Type::String);
    d.max_len = max_len;
    d.hint = hint;
    return d;
}

Def make_secret(const char *id, const char *group, const char *title, uint16_t max_len) {
    Def d = make(id, group, title, Type::Secret);
    d.secret = true;
    d.max_len = max_len;
    d.hint = "Write-only. Needs Notes encryption on and unlocked on the pager.";
    return d;
}

ActionDef action(const char *id, const char *group, const char *title, bool danger = false,
                 const char *hint = "") {
    ActionDef a;
    a.id = id;
    a.group = group;
    a.title = title;
    a.danger = danger;
    a.hint = hint;
    return a;
}

bool url_check(const Value &v, Error &err) {
    if (url_ok(v.s)) return true;
    err.set("invalid_argument", "must be an http:// or https:// URL");
    return false;
}

/** owner/name: one '/', both halves [A-Za-z0-9._-]. */
bool repo_check(const Value &v, Error &err) {
    const std::string &s = v.s;
    if (s.empty()) return true;
    const size_t slash = s.find('/');
    bool ok = slash != std::string::npos && slash > 0 && slash + 1 < s.size() &&
              s.find('/', slash + 1) == std::string::npos;
    for (char c : s) {
        if (!ok) break;
        ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             c == '.' || c == '_' || c == '-' || c == '/';
    }
    if (!ok) err.set("invalid_argument", "must be owner/name");
    return ok;
}

/** No spaces, '~', '^', ':', '?', '*', '[', '\\', "..", leading '-' (git ref rules, roughly). */
bool branch_check(const Value &v, Error &err) {
    const std::string &s = v.s;
    bool ok = s.empty() || (s[0] != '-' && s[0] != '/' && s.find("..") == std::string::npos &&
                            s[s.size() - 1] != '/');
    for (char c : s) {
        if (!ok) break;
        ok = !(c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' ||
               c == '[' || c == '\\');
    }
    if (!ok) err.set("invalid_argument", "not a valid branch name");
    return ok;
}

bool host_check(const Value &v, Error &err) {
    for (char c : v.s) {
        if (c == ' ' || c == '/' || c == '@') {
            err.set("invalid_argument", "must be a host name or address");
            return false;
        }
    }
    return true;
}

bool ssh_hosts_check(const Value &v, Error &err) {
    std::string why;
    if (hal::ssh::parse_hosts(v.s, nullptr, &why)) return true;
    err.set("invalid_argument", why);
    return false;
}

}  // namespace

void build_catalog(Registry &r, const BoardProfile &b) {
    /* --- groups, in the pager's Settings tile order --- */
    r.add_group("display", "Display & Backlight");
    r.add_group("power", "Charger & Performance");
    r.add_group("fonts", "Fonts");
    r.add_group("editor", "Notes Editor");
    r.add_group("time", "Date & Time");
    r.add_group("conn", "Connectivity");
    r.add_group("wifi", "Wi-Fi Networks");
    r.add_group("home", "Home Apps");
    r.add_group("hub", "Local Hub");
    r.add_group("weather", "Weather");
    r.add_group("tg", "Telegram");
    r.add_group("nsync", "Notes Sync");
    r.add_group("notes_sec", "Notes Security");
    r.add_group("storage", "Storage");
    if (b.ssh) r.add_group("ssh", "SSH");
    r.add_group("sys", "System");

    /* --- 3.1 Display & backlight --- */
    r.add(make_int("display.brightness", "display", "Screen brightness", b.min_brightness,
                   b.max_brightness, 1));
    {
        Def d = make_int("display.kb_backlight", "display", "Keyboard backlight (%)", 0, 100, 5);
        d.supported = b.keyboard;
        r.add(d);
    }
    {
        Def d = make_int("display.timeout_s", "display", "Screen timeout (s)", 0, 180, 1,
                         Apply::Tick);
        d.hint = "0 = never";
        r.add(d);
    }
    {
        Def d = make("display.sleep_mode", "display", "Sleep mode", Type::Enum, Apply::NextSleep);
        d.options.push_back(opt_s("light", "Light"));
        d.options.push_back(opt_s("deep", "Deep"));
        r.add(d);
    }
    {
        Def d = make("display.ui_style", "display", "UI style", Type::Enum, Apply::Theme);
        d.options.push_back(opt_s("classic", "Classic"));
        d.options.push_back(opt_s("pixel", "Pixel Iso"));
        d.options.push_back(opt_s("hotel", "Hotel"));
        d.options.push_back(opt_s("beos", "BeOS"));
        r.add(d);
    }
    {
        Def d = make("display.dark", "display", "Dark mode", Type::Bool, Apply::Theme);
        d.hint = "Hotel and BeOS only; Classic and Pixel Iso are always dark.";
        r.add(d);
    }
    r.add(make("statusbar.show_mem", "display", "Show memory in status bar", Type::Bool));
    r.add(make("statusbar.show_files", "display", "Show file count in status bar", Type::Bool));

    /* --- 3.2 Charger & performance --- */
    r.add(make("power.charging", "power", "Charging", Type::Bool));
    {
        Def d = make("power.charge_current_ma", "power", "Charge current", Type::Enum);
        for (uint16_t ma : b.charge_ma) d.options.push_back(opt_i(ma, std::to_string(ma) + " mA"));
        r.add(d);
    }
    r.add(make("power.charge_limit_80", "power", "Limit charge to 80%", Type::Bool));
    {
        Def d = make("power.otg", "power", "OTG output", Type::Bool);
        d.supported = b.otg;
        d.hint = "Live state; not saved across reboots.";
        r.add(d);
    }
    {
        Def d = make("perf.cpu_mhz", "power", "CPU clock", Type::Enum, Apply::Tick);
        d.options.push_back(opt_i(240, "240 MHz (Max)"));
        d.options.push_back(opt_i(160, "160 MHz"));
        d.options.push_back(opt_i(80, "80 MHz (Power)"));
        r.add(d);
    }

    /* --- 3.3 Fonts --- */
    for (size_t i = 0; i < kFontContextCount; ++i) {
        const std::string base = std::string("fonts.") + kFontContexts[i].ctx;
        Def face = make("", "fonts", "", Type::Enum, Apply::Theme);
        face.id = base + ".face";
        face.title = std::string(kFontContexts[i].title) + " font";
        for (size_t f = 0; f < kFontFaceCount; ++f) {
            face.options.push_back(opt_s(kFontFaces[f], kFontFaceLabels[f]));
        }
        face.hint = "Only Inter and Atkinson render accented Latin-1. Applies to screens opened after the change.";
        r.add(face);
        Def size = make_int("", "fonts", "", 10, 32, 2, Apply::Theme);
        size.id = base + ".size";
        size.title = std::string(kFontContexts[i].title) + " size";
        r.add(size);
    }

    /* --- Notes editor --- */
    {
        Def d = make("editor.double_space_period", "editor", "\".\" shortcut (double space)", Type::Bool);
        d.hint = "Two spaces after a word type a period and a space.";
        r.add(d);
    }

    /* --- 3.4 Date & time --- */
    {
        Def d = make("time.tz", "time", "Time zone", Type::Enum);
        d.options.push_back(opt_s("", "Default (America/Sao_Paulo)"));
        for (const std::string &tz : b.tz_names) d.options.push_back(opt_s(tz.c_str()));
        r.add(d);
    }
    r.add_action(action("time.sync_ntp", "time", "Sync time from the internet (NTP)", false,
                        "Needs Wi-Fi."));
    {
        ActionDef a = action("time.sync_gps", "time", "Sync time from GPS");
        a.supported = b.gps;
        r.add_action(a);
    }

    /* --- 3.5 Connectivity --- */
    r.add(make("conn.wifi", "conn", "Wi-Fi", Type::Bool));
    {
        Def d = make("conn.bt", "conn", "Bluetooth", Type::Bool);
        d.danger = true;
        d.hint = "Turning Bluetooth off disconnects this app. Turn it back on from the pager.";
        r.add(d);
    }
    r.add(make("conn.radio", "conn", "LoRa radio", Type::Bool));
    {
        Def d = make("conn.nfc", "conn", "NFC", Type::Bool);
        d.supported = b.nfc;
        r.add(d);
    }
    r.add(make("conn.speaker", "conn", "Speaker", Type::Bool));
    r.add(make("conn.haptic", "conn", "Haptics", Type::Bool));
    {
        Def d = make("wifi.saved", "wifi", "Saved networks", Type::List);
        d.read_only = true;
        d.hint = "Most recent first. Passwords never leave the pager.";
        r.add(d);
    }
    r.add_action(action("wifi.scan", "wifi", "Scan for networks"));
    r.add_action(action("wifi.add", "wifi", "Add a network"));
    r.add_action(action("wifi.forget", "wifi", "Forget a network", true));
    r.add_action(action("wifi.test", "wifi", "Test internet"));

    /* --- 3.6 Home apps --- */
    for (const auto &app : b.home_apps) {
        Def d = make("", "home", "", Type::Bool);
        d.id = "home.visible." + app.first;
        d.title = "Show " + app.second;
        r.add(d);
    }

    /* --- 3.7 Local hub --- */
    r.add(make("hub.enabled", "hub", "Use the local hub", Type::Bool));
    {
        Def d = make_str("hub.url", "hub", "Hub URL", 128, "http://host:port; empty clears.");
        d.check = url_check;
        r.add(d);
    }

    /* --- 3.8 Weather --- */
    {
        Def d = make("weather.location", "weather", "Location", Type::Object);
        d.hint = "{\"auto\":true} or {\"name\",\"lat\",\"lon\"}";
        r.add(d);
    }

    /* --- 3.9 Telegram --- */
    {
        Def d = make_str("tg.url", "tg", "Bridge URL", 128,
                         "Only for the pager's direct Wi-Fi path.");
        d.check = url_check;
        r.add(d);
    }
    r.add(make_secret("tg.token", "tg", "Bridge token", 200));
    r.add(make("tg.notify_vibrate", "tg", "Vibrate on new messages", Type::Bool));
    r.add(make("tg.notify_banner", "tg", "Banner on new messages", Type::Bool));
    {
        Def d = make("tg.favorites", "tg", "Favorite chats", Type::List);
        d.hint = "[{\"id\":<chat id>,\"title\":\"...\"}]";
        r.add(d);
    }

    /* --- 3.10 Notes sync --- */
    {
        Def d = make_str("nsync.repo", "nsync", "GitHub repository", 100, "owner/name");
        d.check = repo_check;
        r.add(d);
    }
    {
        Def d = make_str("nsync.branch", "nsync", "Branch", 100, "Empty = main");
        d.check = branch_check;
        r.add(d);
    }
    r.add(make_secret("nsync.token", "nsync", "GitHub token (PAT)", 255));

    /* --- 3.11 Notes security --- */
    {
        Def d = make("notes_sec.state", "notes_sec", "Notes encryption", Type::Enum);
        d.options.push_back(opt_s("off", "Off"));
        d.options.push_back(opt_s("locked", "Locked"));
        d.options.push_back(opt_s("unlocked", "Unlocked"));
        d.read_only = true;
        d.hint = "Set, change or disable the passphrase on the pager.";
        r.add(d);
    }
    r.add_action(action("notes_sec.lock", "notes_sec", "Lock notes now"));

    /* --- 3.12 Storage --- */
    {
        Def d = make("storage.msc_target", "storage", "USB drive shows", Type::Enum, Apply::Reboot);
        d.options.push_back(opt_s("sd", "SD card"));
        d.options.push_back(opt_s("int", "Internal flash"));
        r.add(d);
    }
    {
        Def d = make("storage.prune_internal", "storage", "Keep only the 50 newest notes on internal",
                     Type::Bool);
        d.hint = "Older notes move to the SD card.";
        r.add(d);
    }
    r.add_action(action("storage.copy_int_to_sd", "storage", "Copy internal notes to SD"));
    r.add_action(action("storage.prune_now", "storage", "Prune internal now"));

    /* --- 3.13 SSH --- */
    {
        Def d = make_str("ssh.host", "ssh", "Host", 128);
        d.check = host_check;
        d.supported = b.ssh;
        r.add(d);
        d = make_str("ssh.user", "ssh", "User", 64);
        d.check = Check();
        d.supported = b.ssh;
        r.add(d);
        d = make_int("ssh.port", "ssh", "Port", 1, 65535, 1);
        d.supported = b.ssh;
        r.add(d);
        d = make_str("ssh.hosts", "ssh", "Saved hosts", 1024,
                     "name=user@host:port; separated by ';'. All share the key and password.");
        d.check = ssh_hosts_check;
        d.supported = b.ssh;
        r.add(d);
        d = make_secret("ssh.password", "ssh", "Password", 128);
        d.supported = b.ssh;
        r.add(d);
        /* An unencrypted OpenSSH/PEM private key (ed25519, ECDSA or RSA up to
         * 2048 bits fits), tried before the password. Set from the phone. */
        d = make_secret("ssh.key", "ssh", "Private key", 3000);
        d.hint = "Unencrypted OpenSSH or PEM key; tried before the password.";
        d.supported = b.ssh;
        r.add(d);
    }

    /* --- 3.14 System --- */
    r.add_action(action("sys.power_off", "sys", "Power off", true,
                        "The pager asks for confirmation on its screen."));
}

}  // namespace settings

#include "nvs.h"

#ifdef ARDUINO
#include <Preferences.h>
#else
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hal {

#ifndef ARDUINO
// --- Emulator store: one file per <namespace>/<key> under $PAGERLINK_NVS_DIR --
namespace {

std::mutex &fs_mutex()
{
    static std::mutex m;
    return m;
}

// mkdir -p: create every missing component of `path` (absolute or relative).
void mkdir_p(const std::string &path)
{
    std::string cur;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t end = path.find('/', i);
        if (end == std::string::npos) end = path.size();
        if (end > i) {
            if (!cur.empty() && cur[cur.size() - 1] != '/') cur += '/';
            cur.append(path, i, end - i);
            ::mkdir(cur.c_str(), 0755);   // ignore EEXIST / errors
        }
        i = end + 1;
    }
}

// Backing directory, resolved and created once per process.
const std::string &nvs_dir()
{
    static const std::string dir = [] {
        std::string d;
        const char *explicit_dir = getenv("PAGERLINK_NVS_DIR");
        if (explicit_dir && explicit_dir[0]) {
            d = explicit_dir;
        } else {
            const char *root = getenv("PAGERLINK_FS_ROOT");
            d = (root && root[0]) ? root : "/tmp/pagerlink-fs";
            d += "/nvs";
        }
        mkdir_p(d);
        return d;
    }();
    return dir;
}

// Path of one entry, creating the namespace directory on the way.
std::string entry_path(const char *ns, const char *key)
{
    std::string p = nvs_dir();
    p += '/';
    p += (ns && *ns) ? ns : "_";
    ::mkdir(p.c_str(), 0755);             // ignore EEXIST / errors
    p += '/';
    p += (key && *key) ? key : "_";
    return p;
}

// Read the whole file. Missing file -> false, `out` untouched.
bool read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    out.swap(data);
    return true;
}

// Write through <path>.tmp + rename() so a crash mid-write cannot truncate the
// stored value.
void write_file(const std::string &path, const void *data, size_t len)
{
    const std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    if (len && data) fwrite(data, 1, len, f);
    fflush(f);
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) ::unlink(tmp.c_str());
}

void remove_file(const std::string &path)
{
    ::unlink(path.c_str());
}

} // namespace
#endif

std::string nvs_get_str(const char *ns, const char *key, const char *dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return dflt ? dflt : "";
    String v = p.getString(key, dflt ? dflt : "");
    p.end();
    return std::string(v.c_str());
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    std::string v;
    if (read_file(entry_path(ns, key), v)) return v;
    return dflt ? dflt : "";
#endif
}

void nvs_set_str(const char *ns, const char *key, const char *value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    if (value && *value) p.putString(key, value);
    else                 p.remove(key);
    p.end();
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    const std::string path = entry_path(ns, key);
    if (value && *value) write_file(path, value, std::strlen(value));
    else                 remove_file(path);
#endif
}

void nvs_put_str(const char *ns, const char *key, const std::string &value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    p.putString(key, value.c_str());
    p.end();
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    write_file(entry_path(ns, key), value.data(), value.size());
#endif
}

bool nvs_get_bool(const char *ns, const char *key, bool dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return dflt;
    bool v = p.getBool(key, dflt);
    p.end();
    return v;
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    std::string v;
    if (!read_file(entry_path(ns, key), v)) return dflt;
    return v == "1";
#endif
}

void nvs_set_bool(const char *ns, const char *key, bool value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    p.putBool(key, value);
    p.end();
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    const char *text = value ? "1" : "0";
    write_file(entry_path(ns, key), text, 1);
#endif
}

uint16_t nvs_get_u16(const char *ns, const char *key, uint16_t dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return dflt;
    uint16_t v = p.getUShort(key, dflt);
    p.end();
    return v;
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    std::string v;
    if (!read_file(entry_path(ns, key), v)) return dflt;
    if (v.empty()) return dflt;
    return static_cast<uint16_t>(strtoul(v.c_str(), nullptr, 10));
#endif
}

void nvs_set_u16(const char *ns, const char *key, uint16_t value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    p.putUShort(key, value);
    p.end();
#else
    char text[8];
    snprintf(text, sizeof(text), "%u", (unsigned)value);
    std::lock_guard<std::mutex> lk(fs_mutex());
    write_file(entry_path(ns, key), text, std::strlen(text));
#endif
}

bool nvs_get_blob(const char *ns, const char *key, std::string &out)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return false;
    size_t len = p.getBytesLength(key);
    if (len == 0) { p.end(); return false; }
    out.resize(len);
    size_t got = p.getBytes(key, &out[0], len);
    p.end();
    if (got != len) { out.clear(); return false; }
    return true;
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    return read_file(entry_path(ns, key), out);
#endif
}

void nvs_set_blob(const char *ns, const char *key, const void *data, size_t len)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    if (len && data) p.putBytes(key, data, len);
    else             p.remove(key);
    p.end();
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    const std::string path = entry_path(ns, key);
    if (len) write_file(path, data, len);
    else     remove_file(path);
#endif
}

bool nvs_has(const char *ns, const char *key)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return false;
    bool v = p.isKey(key);
    p.end();
    return v;
#else
    std::lock_guard<std::mutex> lk(fs_mutex());
    struct stat st;
    if (::stat(entry_path(ns, key).c_str(), &st) != 0) return false;
    return !S_ISDIR(st.st_mode);
#endif
}

} // namespace hal

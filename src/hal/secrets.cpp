/**
 * @file      secrets.cpp
 * @brief     Encrypted NVS wrapper. See secrets.h.
 */
#include "secrets.h"
#include "notes_crypto.h"
#include "str_encode.h"

#include <cstring>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace hal {

bool secret_exists(const char *ns, const char *key)
{
#ifdef ARDUINO
    if (!ns || !key) return false;
    Preferences p;
    if (!p.begin(ns, true)) return false;
    bool present = p.getBytesLength(key) > 0;
    p.end();
    return present;
#else
    (void)ns; (void)key;
    return false;
#endif
}

std::string secret_load(const char *ns, const char *key)
{
#ifdef ARDUINO
    if (!ns || !key) return "";
    Preferences p;
    if (!p.begin(ns, true)) return "";
    size_t len = p.getBytesLength(key);
    if (len == 0) { p.end(); return ""; }
    if (!notes_crypto_is_unlocked()) { p.end(); return ""; }
    std::vector<uint8_t> ct(len);
    p.getBytes(key, ct.data(), len);
    p.end();
    std::string out;
    if (!notes_crypto_decrypt_buffer(ct.data(), ct.size(), out)) return "";
    return out;
#else
    (void)ns; (void)key;
    return "";
#endif
}

bool secret_store(const char *ns, const char *key, const char *plaintext,
                  std::string *err, bool trim_ws)
{
#ifdef ARDUINO
    if (!ns || !key) { if (err) *err = "Bad slot."; return false; }
    std::string value = plaintext ? plaintext : "";
    if (trim_ws) {
        std::string raw;
        raw.swap(value);
        value = trim(raw);
        secret_scrub(raw);
    }
    /* Validate and encrypt first, so NVS is only opened read-write for a
     * write that is going to happen. An empty value clears the slot
     * unconditionally. */
    const bool clear = value.empty();
    std::vector<uint8_t> ct;
    const char *refusal = nullptr;
    if (!clear) {
        if (!notes_crypto_is_enabled()) {
            refusal = "Enable Notes encryption first "
                      "(Settings \xC2\xBB Notes Security).";
        } else if (!notes_crypto_is_unlocked()) {
            refusal = "Notes session locked \xE2\x80\x94 unlock first.";
        } else if (!notes_crypto_encrypt_buffer((const uint8_t *)value.data(),
                                                value.size(), ct)) {
            refusal = "Encryption failed.";
        }
    }
    secret_scrub(value);
    if (refusal) { if (err) *err = refusal; return false; }

    Preferences p;
    if (!p.begin(ns, false)) {
        if (err) *err = "NVS open failed.";
        return false;
    }
    if (clear) p.remove(key);
    else       p.putBytes(key, ct.data(), ct.size());
    p.end();
    return true;
#else
    (void)ns; (void)key; (void)plaintext; (void)trim_ws;
    if (err) *err = "Not supported on emulator.";
    return false;
#endif
}

std::string secret_mask_display(const char *ns, const char *key)
{
    std::string tok = secret_load(ns, key);
    if (tok.empty()) return secret_exists(ns, key) ? "(locked)" : "(not set)";
    std::string out = "********";
    if (tok.size() > 8) out += tok.substr(tok.size() - 4);
    secret_scrub(tok);
    return out;
}

void secret_purge_legacy(const char *ns, const char *legacy_key)
{
#ifdef ARDUINO
    if (!ns || !legacy_key) return;
    Preferences p;
    if (!p.begin(ns, false)) return;
    if (p.isKey(legacy_key)) p.remove(legacy_key);
    p.end();
#else
    (void)ns; (void)legacy_key;
#endif
}

void secret_scrub(std::string &s)
{
    if (s.empty()) return;
    volatile char *p = &s[0];
    for (size_t i = 0; i < s.size(); i++) p[i] = 0;
    s.clear();
    s.shrink_to_fit();
}

} // namespace hal

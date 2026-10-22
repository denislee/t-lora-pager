/**
 * @file      secrets.cpp
 * @brief     Encrypted NVS wrapper. See secrets.h.
 */
#include "secrets.h"
#include "notes_crypto.h"
#include "str_encode.h"

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace hal {

namespace {

/* Masks remembered per "ns/key" so Settings labels skip the ~2 s PBKDF2 on
 * every paint. Holds only "********"+last four chars, never the secret. */
std::mutex g_mask_mu;
std::map<std::string, std::string> g_masks;

std::string slot_id(const char *ns, const char *key)
{
    return std::string(ns) + "/" + key;
}

std::string mask_of(const std::string &tok)
{
    std::string out = "********";
    if (tok.size() > 8) out += tok.substr(tok.size() - 4);
    return out;
}

void mask_remember(const char *ns, const char *key, const std::string &mask)
{
    std::lock_guard<std::mutex> lock(g_mask_mu);
    g_masks[slot_id(ns, key)] = mask;
}

void mask_forget(const char *ns, const char *key)
{
    std::lock_guard<std::mutex> lock(g_mask_mu);
    g_masks.erase(slot_id(ns, key));
}

/* Secret slots whose keys stay pinned while unlocked (P6.22b). The strings
 * mirror their owners' constants (hal can't include app headers):
 *   ui_telegram.cpp   TG_PREFS_NS "tgbridge"  / "token_enc"
 *   ui_notes_sync.cpp NSYNC_PREFS_NS "notesync" / "token_enc"
 *   ui_ssh.cpp        SSH_NS "ssh"           / SSH_PW_KEY "pw_enc"
 * This is also the full list of encrypted secret slots: change-passphrase
 * re-encrypts and disable erases exactly these (secret_slot_*, P7.7), so a
 * new secret_store slot MUST be added here or it stops decrypting after the
 * next passphrase change. */
struct PinnedSlot { const char *ns; const char *key; };
constexpr PinnedSlot kPinnedSlots[] = {
    {"tgbridge", "token_enc"},
    {"notesync", "token_enc"},
    {"ssh",      "pw_enc"},
};
static_assert(NC_PIN_SECRET_FIRST + sizeof(kPinnedSlots) / sizeof(kPinnedSlots[0]) <= NC_PIN_N,
              "notes_crypto.h NC_PIN_N too small for the pinned secret slots");

[[maybe_unused]] int pinned_tag(const char *ns, const char *key)
{
    for (size_t i = 0; i < sizeof(kPinnedSlots) / sizeof(kPinnedSlots[0]); i++) {
        if (strcmp(kPinnedSlots[i].ns, ns) == 0 && strcmp(kPinnedSlots[i].key, key) == 0)
            return NC_PIN_SECRET_FIRST + (int)i;
    }
    return -1;
}

} // namespace

void secret_prewarm_pinned()
{
#ifdef ARDUINO
    for (size_t i = 0; i < sizeof(kPinnedSlots) / sizeof(kPinnedSlots[0]); i++) {
        if (!notes_crypto_is_unlocked()) return;
        std::vector<uint8_t> ct;
        Preferences p;
        if (!p.begin(kPinnedSlots[i].ns, true)) continue;
        size_t len = p.getBytesLength(kPinnedSlots[i].key);
        if (len > 0) {
            ct.resize(len);
            if (p.getBytes(kPinnedSlots[i].key, ct.data(), len) != len) ct.clear();
        }
        p.end();
        if (ct.empty()) continue;
        notes_crypto_pin_key(NC_PIN_SECRET_FIRST + (int)i, ct.data(), ct.size(),
                             /*derive=*/true);
    }
#endif
}

constexpr size_t kSlotCount = sizeof(kPinnedSlots) / sizeof(kPinnedSlots[0]);

size_t secret_slot_count()
{
    return kSlotCount;
}

std::string secret_slot_name(size_t i)
{
    if (i >= kSlotCount) return "";
    return slot_id(kPinnedSlots[i].ns, kPinnedSlots[i].key);
}

bool secret_slot_read_raw(size_t i, std::vector<uint8_t> &ct)
{
    ct.clear();
#ifdef ARDUINO
    if (i >= kSlotCount) return false;
    Preferences p;
    if (!p.begin(kPinnedSlots[i].ns, true)) return false;
    size_t len = p.getBytesLength(kPinnedSlots[i].key);
    if (len > 0) {
        ct.resize(len);
        if (p.getBytes(kPinnedSlots[i].key, ct.data(), len) != len) ct.clear();
    }
    p.end();
    return !ct.empty();
#else
    (void)i;
    return false;
#endif
}

bool secret_slot_write_raw(size_t i, const uint8_t *ct, size_t len)
{
#ifdef ARDUINO
    if (i >= kSlotCount || !ct || len == 0) return false;
    Preferences p;
    if (!p.begin(kPinnedSlots[i].ns, false)) return false;
    bool ok = p.putBytes(kPinnedSlots[i].key, ct, len) == len;
    p.end();
    return ok;
#else
    (void)i; (void)ct; (void)len;
    return false;
#endif
}

void secret_slot_erase(size_t i)
{
    if (i >= kSlotCount) return;
#ifdef ARDUINO
    Preferences p;
    if (p.begin(kPinnedSlots[i].ns, false)) {
        if (p.isKey(kPinnedSlots[i].key)) p.remove(kPinnedSlots[i].key);
        p.end();
    }
#endif
    notes_crypto_unpin_key(NC_PIN_SECRET_FIRST + (int)i);
    mask_forget(kPinnedSlots[i].ns, kPinnedSlots[i].key);
}

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
    std::string mask = (!clear && !refusal) ? mask_of(value) : std::string();
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
    /* Keep a pinned slot pointing at the salt now on flash: the encrypt above
     * just cached its key, so this costs no derive. */
    int tag = pinned_tag(ns, key);
    if (tag >= 0) {
        if (clear) notes_crypto_unpin_key(tag);
        else       notes_crypto_pin_key(tag, ct.data(), ct.size(), /*derive=*/false);
    }
    if (clear) mask_forget(ns, key);
    else       mask_remember(ns, key, mask);
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
    std::string out = mask_of(tok);
    mask_remember(ns, key, out);
    secret_scrub(tok);
    return out;
}

bool secret_mask_peek(const char *ns, const char *key, std::string &out)
{
    if (!secret_exists(ns, key)) {
        mask_forget(ns, key);
        out = "(not set)";
        return true;
    }
    if (!notes_crypto_is_unlocked()) {
        mask_forget(ns, key);
        out = "(locked)";
        return true;
    }
    std::lock_guard<std::mutex> lock(g_mask_mu);
    auto it = g_masks.find(slot_id(ns, key));
    if (it != g_masks.end()) {
        out = it->second;
        return true;
    }
    return false;
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

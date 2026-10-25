/**
 * @file      nfc_provision.cpp
 * @brief     NFC-driven credential provisioning. See nfc_provision.h.
 */
#include "nfc_provision.h"
#include "ui_define.h"
#include "hal/secrets.h"
#include "hal/notes_crypto.h"

#include <cstring>
#include <string>

namespace {

constexpr const char *PREFIX = "lilygo+";

struct Slot {
    const char *tag;        /* the "<slot>" between "lilygo+" and ":" */
    const char *ns;         /* NVS namespace */
    const char *key;        /* NVS key (always *_enc for encrypted secrets) */
    const char *display;    /* human label for the confirm dialog */
};

constexpr Slot SLOTS[] = {
    { "tg", "tgbridge", "token_enc", "Telegram bearer"   },
    { "gh", "notesync", "token_enc", "GitHub PAT"        },
};

/* Pending confirm state, scrubbed on every exit from the dialog. Only one
 * provisioning dialog is ever in flight. */
struct PendingWrite {
    const Slot *slot = nullptr;
    std::string value;
    bool active = false;
} g_pending;

static std::string mask_preview(const std::string &v)
{
    if (v.size() <= 4) return "****";
    if (v.size() <= 12) return "****" + v.substr(v.size() - 4);
    return v.substr(0, 2) + "..." + v.substr(v.size() - 4);
}

/* Strict ASCII-only, no whitespace, no control bytes. Bearer tokens and PATs
 * are all printable ASCII; anything else means a mis-encoded tag. */
static bool value_looks_sane(const std::string &v)
{
    if (v.empty() || v.size() > 512) return false;
    for (char c : v) {
        unsigned char u = (unsigned char)c;
        if (u < 0x21 || u > 0x7E) return false;
    }
    return true;
}

static const Slot *find_slot(const char *tag, size_t tag_len)
{
    for (const auto &s : SLOTS) {
        size_t n = strlen(s.tag);
        if (n == tag_len && memcmp(s.tag, tag, n) == 0) return &s;
    }
    return nullptr;
}

// ui_confirm has already closed the dialog.
static void confirm_done(bool save, void *)
{
    if (save && g_pending.active && g_pending.slot) {
        std::string err;
        bool ok = hal::secret_store(g_pending.slot->ns, g_pending.slot->key,
                                    g_pending.value.c_str(), &err);
        /* Don't leave the plaintext in the pending buffer a moment longer
         * than we must — scrub before the follow-up popup runs. */
        hal::secret_scrub(g_pending.value);
        g_pending.active = false;
        const Slot *slot = g_pending.slot;
        g_pending.slot = nullptr;

        if (ok) {
            std::string done = std::string(slot->display) + " saved.";
            ui_msg_pop_up("NFC Provisioning", done.c_str());
        } else {
            std::string fail = std::string("Could not save ") + slot->display +
                               ":\n" + (err.empty() ? "unknown error" : err);
            ui_msg_pop_up("NFC Provisioning", fail.c_str());
        }
        return;
    }

    /* Cancel — wipe pending state. */
    hal::secret_scrub(g_pending.value);
    g_pending.active = false;
    g_pending.slot = nullptr;
}

static void show_confirm(const Slot *slot, const std::string &value)
{
    g_pending.slot = slot;
    g_pending.value = value;
    g_pending.active = true;

    std::string msg = "Save ";
    msg += slot->display;
    msg += " from NFC tag?\n\nValue: ";
    msg += mask_preview(value);

    if (!notes_crypto_is_enabled()) {
        msg += "\n\nNotes encryption is off — enable it in\n"
               "Settings " "\xC2\xBB" " Notes Security first.";
    } else if (!notes_crypto_is_unlocked()) {
        msg += "\n\nNotes session is locked.\nUnlock before saving.";
    }

    ui_confirm("NFC Provisioning", msg.c_str(), "Cancel", "Save", confirm_done, nullptr);
}

} /* namespace */

bool nfc_provision_maybe_handle(const char *text, size_t len)
{
    if (!text || len == 0) return false;

    /* Some writers null-terminate the payload; trim trailing \0 / CR / LF
     * so a user-friendly tag still parses. */
    while (len > 0) {
        unsigned char c = (unsigned char)text[len - 1];
        if (c == '\0' || c == '\r' || c == '\n' || c == ' ') { len--; continue; }
        break;
    }

    const size_t prefix_len = strlen(PREFIX);
    if (len <= prefix_len) return false;
    if (memcmp(text, PREFIX, prefix_len) != 0) return false;

    /* Locate the ':' separator between the slot tag and the value. */
    const char *rest = text + prefix_len;
    size_t rest_len = len - prefix_len;
    const char *colon = (const char *)memchr(rest, ':', rest_len);
    if (!colon) return false;
    size_t tag_len = (size_t)(colon - rest);
    if (tag_len == 0) return false;

    const Slot *slot = find_slot(rest, tag_len);
    if (!slot) {
        /* Recognised prefix but unknown slot — still ours to report on, so
         * return true to suppress the generic Text popup and tell the user
         * the tag is a no-op. */
        ui_msg_pop_up("NFC Provisioning",
                      "Unknown slot on tag.\nExpected lilygo+tg: or lilygo+gh:");
        return true;
    }

    std::string value(colon + 1, len - prefix_len - tag_len - 1);
    if (!value_looks_sane(value)) {
        ui_msg_pop_up("NFC Provisioning",
                      "Tag value is empty or contains\nnon-printable bytes.");
        return true;
    }

    show_confirm(slot, value);
    /* Scrub our local copy; show_confirm moved it into g_pending already. */
    hal::secret_scrub(value);
    return true;
}

/**
 * @file      ui_nfc_test.cpp
 * @brief     NFC discovery self-test overlay.
 *
 * Pops up from Settings » Connectivity » NFC Test. Forces the NFC chip on,
 * hooks the low-level detection callback, and reports live counters + the
 * last detected NDEF record type so the user can tell whether:
 *
 *   - the chip is powered and discovery is running
 *   - tags are being detected at all
 *   - records are being parsed by the library
 *   - the provisioning handler is seeing the payload
 *
 * Restores the prior NFC enable state on close.
 */
#include "ui_define.h"
#include "hal/peripherals.h"

#include <cstdio>
#include <cstring>

// Only reachable from the Connectivity "NFC Test" row, which exists only on
// builds with the ST25R3916 reader; compiled out everywhere else.
#if defined(USING_ST25R3916)
namespace {

struct NfcTestCtx {
    ui_overlay_t ov        = {};
    lv_obj_t *state_label  = nullptr;
    lv_obj_t *count_label  = nullptr;
    lv_obj_t *last_label   = nullptr;
    bool prev_nfc_enabled  = false;

    /* Counters (updated inside diag_hook; read by poll_cb). int is atomic on
     * ESP32-S3 reads/writes so we don't need a lock for these scalars. */
    volatile uint32_t raw_detects = 0;
    volatile uint32_t parsed_text = 0;
    volatile uint32_t parsed_uri  = 0;
    volatile uint32_t parsed_wifi = 0;
    volatile uint32_t parsed_other = 0;

    /* Last event description, written by diag_hook and rendered by poll_cb
     * to keep LVGL calls off the callback (which can fire under the NFC
     * state machine). Bounded char[] rather than std::string so we don't
     * allocate inside the hook. */
    int      last_kind = -1;
    char     last_preview[48] = {0};

    lv_timer_t *poll_timer = nullptr;
    uint32_t    opened_tick_ms = 0;
    uint32_t    heartbeat = 0;
};

static NfcTestCtx *g_ctx = nullptr;

static const char *kind_name(int kind)
{
    switch (kind) {
        case 0: return "raw";
        case 1: return "Text";
        case 2: return "URI";
        case 3: return "WiFi";
        case 4: return "Other";
        default: return "?";
    }
}

/* Redact credential payloads so a Settings walker doesn't get a free look
 * at the next bearer someone taps in. Text records starting with our
 * provisioning prefix are masked entirely; everything else gets a short
 * preview clipped to the buffer. */
static void fill_preview(char *out, size_t out_sz, int kind,
                         const char *text, unsigned len)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!text || len == 0) return;

    static const char kPrefix[] = "lilygo+";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    if (kind == 1 && len >= prefix_len &&
        memcmp(text, kPrefix, prefix_len) == 0) {
        snprintf(out, out_sz, "<provisioning payload redacted>");
        return;
    }

    size_t copy = len < out_sz - 1 ? len : out_sz - 1;
    /* Replace non-printables with '.' so a binary tag doesn't corrupt the
     * label. */
    for (size_t i = 0; i < copy; i++) {
        unsigned char c = (unsigned char)text[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[copy] = '\0';
}

static void diag_hook(int kind, const char *text, unsigned len)
{
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    switch (kind) {
        case 0: ctx->raw_detects++;  break;
        case 1: ctx->parsed_text++;  break;
        case 2: ctx->parsed_uri++;   break;
        case 3: ctx->parsed_wifi++;  break;
        case 4: ctx->parsed_other++; break;
    }
    ctx->last_kind = kind;
    fill_preview(ctx->last_preview, sizeof(ctx->last_preview), kind, text, len);
}

static void render(NfcTestCtx *ctx)
{
    if (!ctx) return;

    if (ctx->state_label) {
        /* Three independent signals:
         *  - enable flag   = user setting + power gate
         *  - discovery     = did beginNFC() actually start RFAL polling
         *  - heartbeat     = this overlay is live (rotates every render) */
        static const char spin[] = {'|', '/', '-', '\\'};
        char buf[128];
        uint32_t secs = (lv_tick_get() - ctx->opened_tick_ms) / 1000;
        snprintf(buf, sizeof(buf),
                 "Chip: %s  |  Discovery: %s  %c\n"
                 "Uptime: %u:%02u",
                 hw_get_nfc_enable() ? "ON" : "OFF",
                 hw_nfc_discovery_active() ? "ACTIVE" : "not init",
                 spin[ctx->heartbeat++ & 3],
                 (unsigned)(secs / 60), (unsigned)(secs % 60));
        lv_label_set_text(ctx->state_label, buf);
    }

    if (ctx->count_label) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "Raw: %u  Text: %u  URI: %u\n"
                 "WiFi: %u  Other: %u",
                 (unsigned)ctx->raw_detects,
                 (unsigned)ctx->parsed_text,
                 (unsigned)ctx->parsed_uri,
                 (unsigned)ctx->parsed_wifi,
                 (unsigned)ctx->parsed_other);
        lv_label_set_text(ctx->count_label, buf);
    }

    if (ctx->last_label) {
        if (ctx->last_kind < 0) {
            lv_label_set_text(ctx->last_label, "Last: (none)");
        } else {
            char buf[96];
            if (ctx->last_preview[0]) {
                snprintf(buf, sizeof(buf), "Last: %s - %s",
                         kind_name(ctx->last_kind), ctx->last_preview);
            } else {
                snprintf(buf, sizeof(buf), "Last: %s",
                         kind_name(ctx->last_kind));
            }
            lv_label_set_text(ctx->last_label, buf);
        }
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    /* Redraw unconditionally so the heartbeat/uptime visibly updates even
     * when no tag has been detected. Cheap — labels and a timer tick only. */
    render(ctx);
}

static void tear_down()
{
    if (!g_ctx) return;
    NfcTestCtx *ctx = g_ctx;
    g_ctx = nullptr;

    hw_set_nfc_diag_hook(nullptr);

    /* Restore the NFC enable state we captured at open. If it was off when
     * the user came in, flip it back off so we don't drain battery. */
    if (!ctx->prev_nfc_enabled && hw_get_nfc_enable()) {
        hw_set_nfc_enable(false);
    }

    if (ctx->poll_timer) lv_timer_del(ctx->poll_timer);
    ui_overlay_close(&ctx->ov);
    delete ctx;
}

static void close_event_cb(lv_event_t *e) { (void)e; tear_down(); }

static void reset_event_cb(lv_event_t *e)
{
    (void)e;
    NfcTestCtx *ctx = g_ctx;
    if (!ctx) return;
    ctx->raw_detects = 0;
    ctx->parsed_text = 0;
    ctx->parsed_uri  = 0;
    ctx->parsed_wifi = 0;
    ctx->parsed_other = 0;
    ctx->last_kind = -1;
    ctx->last_preview[0] = '\0';
    hw_feedback();
}

} /* namespace */

void ui_nfc_test_open()
{
    if (g_ctx) return;

    NfcTestCtx *ctx = new NfcTestCtx();
    g_ctx = ctx;

    ctx->prev_nfc_enabled = hw_get_nfc_enable();
    lv_obj_t *overlay = ui_overlay_open(&ctx->ov, close_event_cb, false);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Force discovery on regardless of the user's prior setting. tear_down
     * restores the original state. */
    if (!ctx->prev_nfc_enabled) hw_set_nfc_enable(true);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "NFC Self-Test");
    lv_obj_set_style_text_color(title, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    // Full-width wrapping label in `color`.
    auto add_label = [overlay](const char *text, lv_color_t color) {
        lv_obj_t *l = lv_label_create(overlay);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, color, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        return l;
    };
    ctx->state_label = add_label("", UI_COLOR_FG);
    ctx->count_label = add_label("", UI_COLOR_FG);
    ctx->last_label  = add_label("Last: (none)", UI_COLOR_FG);
    add_label("Tap a tag to the back of the device.\n"
              "\"Raw\" should bump on any tag; \"Text\"\n"
              "shows a lilygo+ tag was recognised.", UI_COLOR_MUTED);

    lv_obj_t *reset_btn = ui_modal_btn_create(overlay, LV_SYMBOL_REFRESH " Reset counters",
                                              reset_event_cb, nullptr);
    lv_obj_set_width(reset_btn, lv_pct(100));
    lv_group_add_obj(ctx->ov.group, reset_btn);
    lv_group_focus_obj(reset_btn);

    /* Install the hook after the labels exist so the first event paints
     * against a real overlay. */
    hw_set_nfc_diag_hook(diag_hook);

    ctx->opened_tick_ms = lv_tick_get();
    render(ctx);
    /* 500 ms cadence: fast enough for the heartbeat to feel live, slow
     * enough not to churn the label tree. */
    ctx->poll_timer = lv_timer_create(poll_cb, 500, nullptr);
}
#endif /* USING_ST25R3916 */

/**
 * @file      settings_connectivity.cpp
 * @brief     Settings » Connectivity subpage. Extracted from ui_settings.cpp;
 *            see settings_internal.h for the cross-TU contract.
 *
 * Toggles: WiFi, Bluetooth, Radio, NFC, Speaker, Haptic. Plus three
 * follow-up buttons whose visibility tracks the relevant toggle:
 *   - "WiFi Networks" / "Test Internet"  — shown iff WiFi on
 *   - "NFC Test"                         — shown iff NFC on
 * Toggling WiFi/NFC rebuilds the focus group in place via
 * activate_subpage_group so the newly-hidden-or-shown rows leave/join the
 * encoder nav chain at their registered position.
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../core/one_shot_job.h"
#include "settings_internal.h"

namespace connectivity_cfg {

namespace {

// Connectivity subpage — "WiFi Networks" row is hidden when WiFi is off,
// "NFC Test" row the same when NFC is off. `row` is the full menu_cont
// (icon + label + right-arrow btn); `btn` is the inner clickable that needs
// to join/leave the nav group. Hiding the whole row drops the label too.
lv_obj_t *g_wifi_networks_row    = nullptr;
lv_obj_t *g_wifi_networks_btn    = nullptr;
lv_obj_t *g_nfc_test_row         = nullptr;
lv_obj_t *g_nfc_test_btn         = nullptr;
lv_obj_t *g_connectivity_subpage = nullptr;
lv_obj_t *g_internet_test_row    = nullptr;
lv_obj_t *g_internet_test_btn    = nullptr;
lv_obj_t *g_internet_test_status = nullptr;

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// Rebuild the subpage nav group so re-shown rows are inserted at their
// registered position (right after the toggle) rather than tacked onto the
// end, and so they're actually focusable again. activate_subpage_group
// defaults focus to the back button — move it back to the toggle the user
// just interacted with.
void refocus_after_visibility_change(lv_obj_t *btn)
{
    if (g_connectivity_subpage) {
        activate_subpage_group(g_connectivity_subpage);
        lv_group_focus_obj(btn);
    }
}

// Show/hide the rows that follow a toggle (also the initial state at build).
void show_wifi_rows(bool en)
{
    set_hidden(g_wifi_networks_row, !en);
    set_hidden(g_wifi_networks_btn, !en);
    set_hidden(g_internet_test_row, !en);
    set_hidden(g_internet_test_btn, !en);
    set_hidden(g_internet_test_status, !en);
}

void show_nfc_rows(bool en)
{
    set_hidden(g_nfc_test_row, !en);
    set_hidden(g_nfc_test_btn, !en);
}

void wifi_after(lv_obj_t *btn, bool en)
{
    if (!g_wifi_networks_row) return;
    show_wifi_rows(en);
    refocus_after_visibility_change(btn);
}

void nfc_after(lv_obj_t *btn, bool en)
{
    if (!g_nfc_test_row) return;
    show_nfc_rows(en);
    refocus_after_visibility_change(btn);
}

void apply_radio(bool en)
{
    int16_t st = hw_set_radio_enable(en);
    if (st != 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Radio config failed (err %d)", (int)st);
        ui_msg_pop_up("Radio", msg);
    }
}

// The six hardware toggles share one body: commit the new state through the
// settings registry (which mirrors it into local_param), apply it, then run
// the optional follow-up (row visibility). create_toggle_btn_row flips the
// On/Off label itself.
struct HwToggle {
    const char *label;
    bool (*get)();
    const char *id;                           // settings registry id
    void (*apply)(bool en);
    void (*after)(lv_obj_t *btn, bool en);   // may be nullptr
};

void hw_toggle_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    const HwToggle *t = (const HwToggle *)lv_event_get_user_data(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    settings_commit_bool(t->id, en);
    t->apply(en);
    if (t->after) t->after(obj, en);
}

const HwToggle kWifi    = { "WiFi",      hw_get_wifi_enable,    "conn.wifi",    hw_set_wifi_enable,    wifi_after };
const HwToggle kBt      = { "Bluetooth", hw_get_bt_enable,      "conn.bt",      hw_set_bt_enable,      nullptr };
const HwToggle kRadio   = { "Radio",     hw_get_radio_enable,   "conn.radio",   apply_radio,           nullptr };
const HwToggle kNfc     = { "NFC",       hw_get_nfc_enable,     "conn.nfc",     hw_set_nfc_enable,     nfc_after };
const HwToggle kSpeaker = { "Speaker",   hw_get_speaker_enable, "conn.speaker", hw_set_speaker_enable, nullptr };
const HwToggle kHaptic  = { "Haptic",    hw_get_haptic_enable,  "conn.haptic",  hw_set_haptic_enable,  nullptr };

lv_obj_t *add_hw_toggle(lv_obj_t *sub_page, const HwToggle &t)
{
    return create_toggle_btn_row(sub_page, t.label, t.get(), hw_toggle_cb, (void *)&t);
}

void wifi_networks_click_cb(lv_event_t *)
{
    ui_wifi_networks_open();
}

// Render the internet-test outcome into the status label. Shared by the
// job's delivery and the synchronous task-spawn-failure fallback.
void show_internet_result(bool ok, uint32_t rtt_ms, const std::string &err)
{
    if (!g_internet_test_status) return;
    if (ok) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " Online (%u ms)", (unsigned)rtt_ms);
        lv_label_set_text(g_internet_test_status, buf);
        lv_obj_set_style_text_color(g_internet_test_status,
                                    lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        std::string msg = LV_SYMBOL_CLOSE " " + (err.empty() ? std::string("Failed") : err);
        lv_label_set_text(g_internet_test_status, msg.c_str());
        lv_obj_set_style_text_color(g_internet_test_status,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
}

// P2.3: hw_ping_internet blocks for up to 3 s; running it inline froze the
// LVGL thread (dead back button/encoder) for the whole probe — exactly the
// situation in which a user taps this button. Run it on a core::OneShotJob
// worker and deliver the result on the LVGL thread: the worker touches only
// its own heap result (never the widget tree), and reset_state() detaches it
// before the subpage widgets free.
struct InternetTestResult {
    bool ok = false;
    uint32_t rtt_ms = 0;
    std::string err;
};

void internet_test_done(InternetTestResult &res)
{
    show_internet_result(res.ok, res.rtt_ms, res.err);
}

core::OneShotJob<InternetTestResult> g_inet_test_job(internet_test_done);

void internet_test_click_cb(lv_event_t *)
{
    if (!g_internet_test_status) return;
    using Job = core::OneShotJob<InternetTestResult>;
    Job::Start started = g_inet_test_job.start([]() {
        InternetTestResult *res = new InternetTestResult();
        res->ok = hw_ping_internet(&res->rtt_ms, &res->err);
        return res;
    }, 4096, "inet_tst", 1);
    // Attached: a check is already in flight (e.g. page was exited/re-entered
    // mid-probe); the job re-attached so that result still lands.
    lv_label_set_text(g_internet_test_status, started == Job::Start::Attached
                                                  ? "Still testing..."
                                                  : "Testing 1.1.1.1...");
    lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_ACCENT, 0);
    if (started == Job::Start::Failed) {
        // Task slots exhausted — fall back to a synchronous probe so the
        // button still works (pays the old up-to-3 s UI freeze).
        uint32_t rtt_ms = 0;
        std::string err;
        bool ok = hw_ping_internet(&rtt_ms, &err);
        show_internet_result(ok, rtt_ms, err);
    }
}

} // anonymous namespace

void reset_state()
{
    g_wifi_networks_row    = nullptr;
    g_wifi_networks_btn    = nullptr;
    g_nfc_test_row         = nullptr;
    g_nfc_test_btn         = nullptr;
    g_connectivity_subpage = nullptr;
    g_internet_test_row    = nullptr;
    g_internet_test_btn    = nullptr;
    g_internet_test_status = nullptr;
    g_inet_test_job.teardown();
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *btn;

    add_hw_toggle(sub_page, kWifi);

    btn = settings_button_row(sub_page, LV_SYMBOL_WIFI, "WiFi Networks", wifi_networks_click_cb);
    g_wifi_networks_btn = btn;
    g_wifi_networks_row = lv_obj_get_parent(btn);  // menu_cont holding icon+label+btn
    g_connectivity_subpage = sub_page;

    // "Test Internet" — TCP connect to Cloudflare DNS (1.1.1.1:53) to confirm
    // the WiFi link has actual internet reachability, not just an AP association.
    // Hidden together with the WiFi Networks row when WiFi is off.
    btn = settings_button_row(sub_page, LV_SYMBOL_REFRESH, "Test Internet", internet_test_click_cb);
    g_internet_test_btn = btn;
    g_internet_test_row = lv_obj_get_parent(btn);
    g_internet_test_status = lv_label_create(sub_page);
    lv_label_set_text(g_internet_test_status, "");
    lv_obj_set_style_text_color(g_internet_test_status, UI_COLOR_MUTED, 0);
    lv_obj_set_style_pad_left(g_internet_test_status, 12, 0);
    show_wifi_rows(hw_get_wifi_enable());

    add_hw_toggle(sub_page, kBt);
    add_hw_toggle(sub_page, kRadio);
    add_hw_toggle(sub_page, kNfc);

#if defined(USING_ST25R3916)
    btn = settings_button_row(sub_page, LV_SYMBOL_REFRESH, "NFC Test",
                              [](lv_event_t *) { ui_nfc_test_open(); });
    g_nfc_test_btn = btn;
    g_nfc_test_row = lv_obj_get_parent(btn);
    show_nfc_rows(hw_get_nfc_enable());
#endif

    add_hw_toggle(sub_page, kSpeaker);
    add_hw_toggle(sub_page, kHaptic);
}

} // namespace connectivity_cfg

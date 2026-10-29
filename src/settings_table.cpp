/**
 * @file      settings_table.cpp
 * @brief     Binds every id of the settings catalog to its store and apply
 *            step, and every settings action to its worker (hal/settings.h).
 *
 * Store rules (IOS_SETTINGS_PLAN.md §4.1):
 *  - blob fields update `user_setting` field by field and save the blob at
 *    once (fixes T2: the toggles used to be saved only on Settings exit);
 *  - other namespaces go through their existing owner (hal::hub_*, the
 *    timezone, apps::tg_cfg_*, secrets, ...), so their caches stay coherent;
 *  - anything that touches LVGL or a radio stack is applied on the LVGL thread
 *    (apply_later -> core::ui_post, T4), after the value is stored, so a read
 *    right after a write already sees the new value.
 *
 * Setters run with the registry mutex held: keep them short and never block
 * on the LVGL thread from one.
 */
#include "hal/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apps/app_registry.h"
#include "apps/menu_app.h"
#include "core/system.h"
#include "core/ui_post.h"
#include "hal/audio.h"
#include "hal/display.h"
#include "hal/gps_time_sync.h"
#include "hal/hub.h"
#include "hal/internal.h"
#include "hal/link/link_service.h"
#include "hal/notes_crypto.h"
#include "hal/nvs.h"
#include "hal/peripherals.h"
#include "hal/power.h"
#include "hal/radio.h"
#include "hal/secrets.h"
#include "hal/settings_catalog.h"
#include "hal/storage.h"
#include "hal/system.h"
#include "hal/timezone.h"
#include "hal/weather_cache.h"
#include "hal/wireless.h"
#include "ui/modals.h"
#include "ui/theme.h"

#ifdef ARDUINO
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <pthread.h>
#include <random>
#include <unistd.h>
#endif

/* Defined in apps/ui_weather.cpp (declared privately in settings_internal.h). */
std::string weather_get_user_city();
void weather_set_user_location(const char *city, double lat, double lon);

namespace settings {

using pagerlink::JsonObject;
using pagerlink::JsonWriter;

namespace {

/* ------------------------------------------------------------ apply */

thread_local std::vector<std::function<void()>> *t_apply_sink = nullptr;

void apply_later(std::function<void()> fn) {
    if (t_apply_sink) t_apply_sink->push_back(fn);
    else core::ui_post(fn);
}

void apply_theme() {
    apply_later([] {
        if (theme_init()) core::System::getInstance().applyChromeStyle();
    });
}

/* ------------------------------------------------------------ workers */

/** Run `fn` on its own worker (FreeRTOS task / detached pthread). */
void spawn(const char *name, std::function<void()> fn) {
    std::function<void()> *heap = new std::function<void()>(fn);
#ifdef ARDUINO
    if (xTaskCreatePinnedToCore(
            [](void *arg) {
                std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(arg));
                (*f)();
                vTaskDelete(nullptr);
            },
            name, 6144, heap, 1, nullptr, 0) != pdPASS) {
        delete heap;
    }
#else
    (void)name;
    pthread_t th;
    if (pthread_create(
            &th, nullptr,
            [](void *arg) -> void * {
                std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(arg));
                (*f)();
                return nullptr;
            },
            heap) == 0) {
        pthread_detach(th);
    } else {
        delete heap;
    }
#endif
}

void sleep_ms(uint32_t ms) {
#ifdef ARDUINO
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    usleep(ms * 1000);
#endif
}

uint32_t now_ms() {
    return pagerlink::steady_ms();
}

std::mutex g_job_mu;
uint32_t g_next_job = 1;

std::string new_job_id() {
    std::lock_guard<std::mutex> lk(g_job_mu);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "j%u", static_cast<unsigned>(g_next_job++));
    return buf;
}

/** evt.settings.job {"job","done":true,"ok","msg","result"?}. */
void job_done(const std::string &job, bool ok, const std::string &msg,
              const std::string &result_json = std::string()) {
    JsonWriter w;
    w.begin_object().key("job").str_val(job).key("done").bool_val(true).key("ok").bool_val(ok);
    w.key("msg").str_val(msg);
    if (!result_json.empty()) w.key("result").raw_val(result_json.c_str());
    w.end_object();
    pagerlink::service_emit("evt.settings.job", w.str());
}

/** Start `work` (returns ok, sets msg/result) on a worker; answer {"job"} now. */
bool start_job(const char *name, JsonWriter &out,
               std::function<bool(std::string &msg, std::string &result)> work) {
    const std::string job = new_job_id();
    spawn(name, [job, work] {
        std::string msg, result;
        const bool ok = work(msg, result);
        job_done(job, ok, msg, result);
    });
    out.begin_object().key("job").str_val(job).end_object();
    return true;
}

/* ------------------------------------------------------------ blob fields */

Value blob_u8(uint8_t user_setting_params_t::*f) { return Value::of_int(user_setting.*f); }

/** Store one u8 blob field and save the blob. */
void put_u8(uint8_t user_setting_params_t::*f, int64_t v) {
    user_setting.*f = static_cast<uint8_t>(v);
    save_user_setting_nvs();
}

void bind_blob_bool(Registry &r, const char *id, uint8_t user_setting_params_t::*f,
                    std::function<void(bool)> apply = std::function<void(bool)>()) {
    r.bind(id, [f] { return Value::of_bool(user_setting.*f != 0); },
           [f, apply](const Value &v, Error &) {
               put_u8(f, v.b ? 1 : 0);
               if (apply) {
                   const bool on = v.b;
                   apply_later([apply, on] { apply(on); });
               }
               return true;
           });
}

void bind_blob_int(Registry &r, const char *id, uint8_t user_setting_params_t::*f) {
    r.bind(id, [f] { return blob_u8(f); },
           [f](const Value &v, Error &) {
               put_u8(f, v.i);
               return true;
           });
}

struct FontFields {
    uint8_t user_setting_params_t::*face;
    uint8_t user_setting_params_t::*size;
};

/* Same order as kFontContexts. */
const FontFields kFontFields[] = {
    {&user_setting_params_t::system_font_index, &user_setting_params_t::system_font_size},
    {&user_setting_params_t::home_font_index, &user_setting_params_t::home_font_size},
    {&user_setting_params_t::editor_font_index, &user_setting_params_t::editor_font_size},
    {&user_setting_params_t::journal_font_index, &user_setting_params_t::journal_font_size},
    {&user_setting_params_t::weather_font_index, &user_setting_params_t::weather_font_size},
    {&user_setting_params_t::telegram_font_index, &user_setting_params_t::telegram_font_size},
    {&user_setting_params_t::ssh_font_index, &user_setting_params_t::ssh_font_size},
    {&user_setting_params_t::chat_font_index, &user_setting_params_t::chat_font_size},
    {&user_setting_params_t::header_font_index, &user_setting_params_t::header_font_size},
};

/* ------------------------------------------------------------ secrets */

/** Writing a non-empty secret needs notes encryption on and unlocked (T5). */
bool secret_check(const Value &v, Error &err) {
    if (v.s.empty()) return true;
    if (!notes_crypto_is_enabled()) {
        err.set("unauthorized", "encryption off");
        return false;
    }
    if (!notes_crypto_is_unlocked()) {
        err.set("unauthorized", "notes locked");
        return false;
    }
    return true;
}

void bind_secret(Registry &r, const char *id, const char *ns, const char *key,
                 std::function<bool(const char *, std::string *)> store) {
    const std::string sns = ns, skey = key;
    r.bind(id, [sns, skey] { return Value::of_bool(hal::secret_exists(sns.c_str(), skey.c_str())); },
           [store](const Value &v, Error &err) {
               std::string why;
               if (store(v.s.c_str(), &why)) return true;
               err.set("unauthorized", why.empty() ? std::string("refused") : why);
               return false;
           },
           secret_check);
}

/* ------------------------------------------------------------ helpers */

bool str_bind_nvs(Registry &r, const char *id, const char *ns, const char *key) {
    const std::string sns = ns, skey = key;
    return r.bind(id, [sns, skey] { return Value::of_str(hal::nvs_get_str(sns.c_str(), skey.c_str(), "")); },
                  [sns, skey](const Value &v, Error &) {
                      hal::nvs_set_str(sns.c_str(), skey.c_str(), v.s.c_str());
                      return true;
                  });
}

/** Parse a JSON array of objects into raw element texts. */
bool array_objects(const std::string &raw, std::vector<std::string> &items) {
    return pagerlink::json_array_items(raw, items);
}

std::string ssid_list_json() {
    std::vector<std::string> ssids;
    hw_wifi_get_saved_list(ssids);
    JsonWriter w;
    w.begin_array();
    for (const std::string &s : ssids) w.str_val(s);
    w.end_array();
    return w.str();
}

std::string fmt_coord(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.5f", v);
    return buf;
}

/* Telegram favorites, read and written in the Telegram app's NVS format
 * (ui_telegram.cpp load_favorites/save_favorites): ids in `favs`
 * (comma-separated), titles in `fav_titles` as `<id>\x1F<title>\x1E`. */
void load_favs_nvs(std::vector<std::pair<long long, std::string>> &out) {
    out.clear();
    const std::string ids = hal::nvs_get_str("tgbridge", "favs", "");
    const std::string titles = hal::nvs_get_str("tgbridge", "fav_titles", "");
    size_t i = 0;
    while (i < ids.size()) {
        size_t j = ids.find(',', i);
        if (j == std::string::npos) j = ids.size();
        const long long id = std::strtoll(ids.substr(i, j - i).c_str(), nullptr, 10);
        if (id != 0) out.push_back(std::make_pair(id, std::string()));
        i = j + 1;
    }
    i = 0;
    while (i < titles.size()) {
        size_t end = titles.find('\x1E', i);
        if (end == std::string::npos) end = titles.size();
        const std::string rec = titles.substr(i, end - i);
        const size_t sep = rec.find('\x1F');
        if (sep != std::string::npos) {
            const long long id = std::strtoll(rec.substr(0, sep).c_str(), nullptr, 10);
            for (auto &f : out) {
                if (f.first == id) f.second = rec.substr(sep + 1);
            }
        }
        i = end + 1;
    }
}

void save_favs_nvs(const std::vector<std::pair<long long, std::string>> &favs) {
    std::string ids, titles;
    for (const auto &f : favs) {
        char b[24];
        std::snprintf(b, sizeof(b), "%lld", f.first);
        if (!ids.empty()) ids.push_back(',');
        ids += b;
        if (f.second.empty()) continue;
        titles += b;
        titles.push_back('\x1F');
        titles += f.second;
        titles.push_back('\x1E');
    }
    hal::nvs_set_str("tgbridge", "favs", ids.c_str());
    hal::nvs_set_str("tgbridge", "fav_titles", titles.c_str());
}

/* ------------------------------------------------------------ profile */

BoardProfile device_profile() {
    BoardProfile b;
    b.min_brightness = hw_get_disp_min_brightness();
    b.max_brightness = hw_get_disp_max_brightness();
    b.keyboard = hw_has_keyboard();
    b.otg = hw_has_otg_function();
#if defined(ARDUINO_T_LORA_PAGER)
    b.ssh = true;
#endif
#ifdef ARDUINO
    b.gps = true;
#endif
#if defined(ARDUINO_T_LORA_PAGER) || defined(ARDUINO_T_WATCH_S3_ULTRA)
    b.nfc = true;
#endif
    std::vector<uint16_t> ma;
    hw_charge_current_options(ma);
    b.charge_ma = ma;
    timezone_fetch_list(b.tz_names);
    for (int i = 0; i < apps::home_apps_count(); ++i) {
        b.home_apps.push_back(std::make_pair(std::string(apps::home_apps_name(i)),
                                             std::string(apps::home_apps_label(i))));
    }
    return b;
}

uint32_t boot_random() {
#ifdef ARDUINO
    return esp_random() | 1u;
#else
    std::random_device rd;
    return static_cast<uint32_t>(rd()) | 1u;
#endif
}

/* ------------------------------------------------------------ bindings */

void bind_display(Registry &r) {
    r.bind("display.brightness", [] { return blob_u8(&user_setting_params_t::brightness_level); },
           [](const Value &v, Error &) {
               put_u8(&user_setting_params_t::brightness_level, v.i);
               const uint8_t lvl = static_cast<uint8_t>(v.i);
               apply_later([lvl] { hw_set_disp_backlight(lvl); });
               return true;
           });
    /* Wire 0..100 % in steps of 5; stored 0..255 like the Settings slider. */
    r.bind("display.kb_backlight",
           [] {
               const int lvl = user_setting.keyboard_bl_level;
               return Value::of_int(((lvl * 20 + 127) / 255) * 5);
           },
           [](const Value &v, Error &) {
               const uint8_t lvl = static_cast<uint8_t>(v.i * 255 / 100);
               put_u8(&user_setting_params_t::keyboard_bl_level, lvl);
               apply_later([lvl] { hw_set_kb_backlight(lvl); });
               return true;
           });
    bind_blob_int(r, "display.timeout_s", &user_setting_params_t::disp_timeout_second);
    r.bind("display.sleep_mode",
           [] { return Value::of_str(user_setting.sleep_mode ? "deep" : "light"); },
           [](const Value &v, Error &) {
               put_u8(&user_setting_params_t::sleep_mode, v.s == "deep" ? 1 : 0);
               return true;
           });
    /* Style and dark mode: NVS is the truth (ui_style_get caches it in RAM and
     * is refreshed by ui_style_set on the LVGL thread). */
    r.bind("display.ui_style",
           [] {
               std::string s = hal::nvs_get_str("ui", "style", "");
               if (s.empty()) s = ui_style_get() == UI_STYLE_PIXEL_ISO ? "pixel" : "classic";
               if (s != "pixel" && s != "hotel" && s != "beos") s = "classic";
               return Value::of_str(s);
           },
           [](const Value &v, Error &) {
               hal::nvs_set_str("ui", "style", v.s.c_str());
               const ui_style_t st = v.s == "pixel"   ? UI_STYLE_PIXEL_ISO
                                     : v.s == "hotel" ? UI_STYLE_HOTEL
                                     : v.s == "beos"  ? UI_STYLE_BEOS
                                                      : UI_STYLE_CLASSIC;
               apply_later([st] {
                   ui_style_set(st);
                   core::System::getInstance().applyChromeStyle();
               });
               return true;
           });
    r.bind("display.dark", [] { return Value::of_bool(hal::nvs_get_bool("ui", "dark", false)); },
           [](const Value &v, Error &) {
               hal::nvs_set_bool("ui", "dark", v.b);
               const bool dark = v.b;
               apply_later([dark] {
                   ui_dark_mode_set(dark);
                   core::System::getInstance().applyChromeStyle();
               });
               return true;
           });
    bind_blob_bool(r, "statusbar.show_mem", &user_setting_params_t::show_mem_usage);
    bind_blob_bool(r, "statusbar.show_files", &user_setting_params_t::show_file_count);
}

void bind_power(Registry &r) {
    bind_blob_bool(r, "power.charging", &user_setting_params_t::charger_enable, [](bool on) {
        hw_set_charger(on);
        hw_apply_charge_limit();
    });
    r.bind("power.charge_current_ma", [] { return Value::of_int(user_setting.charger_current); },
           [](const Value &v, Error &err) {
               const uint8_t level = hw_charge_level_for_ma(static_cast<uint16_t>(v.i));
               if (level == 0) {
                   err.set("invalid_argument", "not one of the choices");
                   return false;
               }
               user_setting.charger_current = static_cast<uint16_t>(v.i);
               save_user_setting_nvs();
               apply_later([level] { hw_set_charger_current_level(level); });
               return true;
           });
    bind_blob_bool(r, "power.charge_limit_80", &user_setting_params_t::charge_limit_en, [](bool on) {
        if (on) hw_apply_charge_limit();
        else hw_set_charger(user_setting.charger_enable);
    });
    r.bind("power.otg", [] { return Value::of_bool(hw_get_otg_enable()); },
           [](const Value &v, Error &err) {
               if (hw_set_otg(v.b)) return true;
               err.set("internal", "the charger refused OTG");
               return false;
           });
    r.bind("perf.cpu_mhz", [] { return Value::of_int(user_setting.cpu_freq_mhz); },
           [](const Value &v, Error &) {
               user_setting.cpu_freq_mhz = static_cast<uint16_t>(v.i);
               save_user_setting_nvs();
               return true;
           });
}

void bind_fonts(Registry &r) {
    for (size_t i = 0; i < kFontContextCount; ++i) {
        const std::string base = std::string("fonts.") + kFontContexts[i].ctx;
        const FontFields f = kFontFields[i];
        r.bind(base + ".face",
               [f] {
                   size_t idx = user_setting.*(f.face);
                   if (idx >= kFontFaceCount) idx = 0;
                   return Value::of_str(kFontFaces[idx]);
               },
               [f](const Value &v, Error &) {
                   for (size_t k = 0; k < kFontFaceCount; ++k) {
                       if (v.s == kFontFaces[k]) put_u8(f.face, static_cast<int64_t>(k));
                   }
                   apply_theme();
                   return true;
               });
        r.bind(base + ".size", [f] { return blob_u8(f.size); },
               [f](const Value &v, Error &) {
                   put_u8(f.size, v.i);
                   apply_theme();
                   return true;
               });
    }
}

void bind_editor(Registry &r) {
    r.bind("editor.double_space_period",
           [] { return Value::of_bool(apps::editor_cfg_get_double_space_period()); },
           [](const Value &v, Error &) {
               apps::editor_cfg_set_double_space_period(v.b);
               return true;
           });
}

void bind_time(Registry &r) {
    r.bind("time.tz",
           [] {
               std::string tz = timezone_get_user_tz();
               return Value::of_str(timezone_is_known(tz.c_str()) ? tz : std::string());
           },
           [](const Value &v, Error &) {
               timezone_set_user_tz(v.s.c_str());
               return true;
           });
    r.bind_action("time.sync_ntp", [](const JsonObject &, JsonWriter &out, Error &err) {
        if (!hw_get_wifi_connected()) {
            err.set("wifi_offline", "Wi-Fi is not connected");
            return false;
        }
        return start_job("set_ntp", out, [](std::string &msg, std::string &) {
            if (!hw_start_time_sync_ntp()) {
                msg = "could not start NTP";
                return false;
            }
            const uint32_t t0 = now_ms();
            while (static_cast<uint32_t>(now_ms() - t0) < 20000) {
                if (hw_get_time_sync_status() == 1) {
                    msg = "synced";
                    return true;
                }
                sleep_ms(250);
            }
            msg = "NTP timed out";
            return false;
        });
    });
    r.bind_action("time.sync_gps", [](const JsonObject &, JsonWriter &out, Error &) {
        return start_job("set_gps", out, [](std::string &msg, std::string &) {
            std::shared_ptr<int> started(new int(0));
            core::ui_post([started] { *started = hw_start_time_sync_gps() ? 1 : -1; });
            const uint32_t t0 = now_ms();
            while (static_cast<uint32_t>(now_ms() - t0) < 95000) {
                sleep_ms(200);
                if (*started < 0) {
                    msg = "GPS unavailable";
                    return false;
                }
                /* Serial1 is pumped on the LVGL thread, as the Settings page does. */
                core::ui_post([] { hw_pump_time_sync_gps(); });
                const int s = hw_get_time_sync_gps_status();
                if (*started > 0 && s == 1) {
                    msg = "synced";
                    return true;
                }
                if (*started > 0 && s < 0) {
                    msg = s == -1 ? "no GPS fix" : "GPS unsupported";
                    return false;
                }
            }
            core::ui_post([] { hw_stop_time_sync_gps(); });
            msg = "GPS timed out";
            return false;
        });
    });
}

void bind_conn(Registry &r) {
    bind_blob_bool(r, "conn.wifi", &user_setting_params_t::wifi_enable,
                   [](bool on) { hw_set_wifi_enable(on); });
    bind_blob_bool(r, "conn.bt", &user_setting_params_t::bt_enable,
                   [](bool on) { hw_set_bt_enable(on); });
    bind_blob_bool(r, "conn.radio", &user_setting_params_t::radio_enable,
                   [](bool on) { hw_set_radio_enable(on); });
    bind_blob_bool(r, "conn.nfc", &user_setting_params_t::nfc_enable,
                   [](bool on) { hw_set_nfc_enable(on); });
    bind_blob_bool(r, "conn.speaker", &user_setting_params_t::speaker_enable,
                   [](bool on) { hw_set_speaker_enable(on); });
    bind_blob_bool(r, "conn.haptic", &user_setting_params_t::haptic_enable,
                   [](bool on) { hw_set_haptic_enable(on); });

    r.bind("wifi.saved", [] { return Value::of_json(ssid_list_json()); });

    r.bind_action("wifi.scan", [](const JsonObject &, JsonWriter &out, Error &err) {
        if (!hw_get_wifi_enable()) {
            err.set("wifi_offline", "Wi-Fi is off");
            return false;
        }
        return start_job("set_scan", out, [](std::string &msg, std::string &result) {
            core::ui_post([] { hw_set_wifi_scan(); });
            sleep_ms(300);
            const uint32_t t0 = now_ms();
            while (hw_get_wifi_scanning() && static_cast<uint32_t>(now_ms() - t0) < 15000) {
                sleep_ms(250);
            }
            std::vector<wifi_scan_params_t> list;
            hw_get_wifi_scan_result(list);
            JsonWriter w;
            w.begin_array();
            for (const wifi_scan_params_t &n : list) {
                if (n.ssid.empty()) continue;
                w.begin_object()
                    .key("ssid").str_val(n.ssid)
                    .key("rssi").int_val(n.rssi)
                    .key("secure").bool_val(n.authmode != 0)
                    .end_object();
            }
            w.end_array();
            result = w.str();
            msg = "ok";
            return true;
        });
    });

    r.bind_action("wifi.add", [](const JsonObject &p, JsonWriter &out, Error &err) {
        std::string ssid, pw;
        if (!p.get_string("ssid", ssid) || ssid.empty() || ssid.size() > 32) {
            err.set("invalid_argument", "ssid: 1..32 bytes");
            return false;
        }
        if (p.has("password") && (!p.get_string("password", pw) || pw.size() > 64)) {
            err.set("invalid_argument", "password: up to 64 bytes");
            return false;
        }
        if (!hw_get_wifi_enable()) {
            err.set("wifi_offline", "Wi-Fi is off");
            return false;
        }
        return start_job("set_wadd", out, [ssid, pw](std::string &msg, std::string &) {
            std::shared_ptr<wifi_conn_params_t> c(new wifi_conn_params_t);
            c->ssid = ssid;
            c->password = pw;
            core::ui_post([c] { hw_set_wifi_connect(*c); });
            const uint32_t t0 = now_ms();
            while (static_cast<uint32_t>(now_ms() - t0) < 20000) {
                sleep_ms(500);
                std::string cur;
                if (hw_get_wifi_connected()) {
                    hw_get_wifi_ssid(cur);
                    if (cur == ssid) {
                        hw_wifi_add_saved(ssid, pw);
                        note_changed("wifi.saved");
                        msg = "connected";
                        return true;
                    }
                }
            }
            msg = "could not connect";
            return false;
        });
    });

    r.bind_action("wifi.forget", [](const JsonObject &p, JsonWriter &out, Error &err) {
        bool all = false;
        std::string ssid;
        if (p.get_bool("all", all) && all) {
            hw_wifi_forget();
        } else if (p.get_string("ssid", ssid) && !ssid.empty()) {
            if (!hw_wifi_forget_ssid(ssid)) {
                err.set("invalid_argument", "ssid: not saved");
                return false;
            }
        } else {
            err.set("invalid_argument", "ssid or all required");
            return false;
        }
        note_changed("wifi.saved");
        out.begin_object().end_object();
        return true;
    });

    r.bind_action("wifi.test", [](const JsonObject &, JsonWriter &out, Error &) {
        return start_job("set_ping", out, [](std::string &msg, std::string &result) {
            uint32_t ms = 0;
            const bool ok = hw_ping_internet(&ms);
            JsonWriter w;
            w.begin_object().key("ok").bool_val(ok).key("ms").uint_val(ms).end_object();
            result = w.str();
            msg = ok ? "online" : "offline";
            return true;
        });
    });
}

void bind_home(Registry &r) {
    for (int i = 0; i < apps::home_apps_count(); ++i) {
        const std::string id = std::string("home.visible.") + apps::home_apps_name(i);
        r.bind(id, [i] { return Value::of_bool(apps::home_apps_is_visible(i)); },
               [i](const Value &v, Error &) {
                   apps::home_apps_set_visible(i, v.b);
                   return true;
               });
    }
}

void bind_hub_weather(Registry &r) {
    r.bind("hub.enabled", [] { return Value::of_bool(hal::hub_get_enabled_pref()); },
           [](const Value &v, Error &) {
               hal::hub_set_enabled(v.b);
               return true;
           });
    r.bind("hub.url", [] { return Value::of_str(hal::hub_get_url_raw()); },
           [](const Value &v, Error &) {
               hal::hub_set_url(v.s.c_str());
               return true;
           });

    r.bind("weather.location",
           [] {
               const std::string city = weather_get_user_city();
               JsonWriter w;
               w.begin_object();
               if (city.empty()) {
                   w.key("auto").bool_val(true);
               } else {
                   w.key("name").str_val(city);
                   GeoData g;
                   if (weather_cache_load_geo(weather_cache_key(city), g)) {
                       w.key("lat").raw_val(fmt_coord(g.lat).c_str());
                       w.key("lon").raw_val(fmt_coord(g.lon).c_str());
                   }
               }
               w.end_object();
               return Value::of_json(w.str());
           },
           [](const Value &v, Error &) {
               JsonObject o;
               o.parse(v.s);
               bool is_auto = false;
               if (o.get_bool("auto", is_auto) && is_auto) {
                   weather_set_user_location("", 0, 0);
                   return true;
               }
               std::string name;
               double lat = 0, lon = 0;
               o.get_string("name", name);
               o.get_double("lat", lat);
               o.get_double("lon", lon);
               weather_set_user_location(name.c_str(), lat, lon);
               return true;
           },
           [](const Value &v, Error &err) {
               JsonObject o;
               if (!o.parse(v.s)) {
                   err.set("invalid_argument", "bad object");
                   return false;
               }
               bool is_auto = false;
               if (o.get_bool("auto", is_auto) && is_auto) return true;
               std::string name;
               double lat = 0, lon = 0;
               if (!o.get_string("name", name) || name.empty() || name.size() > 64) {
                   err.set("invalid_argument", "name: 1..64 bytes");
                   return false;
               }
               for (unsigned char c : name) {
                   if (c < 0x20) {
                       err.set("invalid_argument", "name: control character");
                       return false;
                   }
               }
               if (!o.get_double("lat", lat) || !o.get_double("lon", lon) || lat < -90 ||
                   lat > 90 || lon < -180 || lon > 180) {
                   err.set("invalid_argument", "lat/lon out of range");
                   return false;
               }
               return true;
           });
}

void bind_telegram(Registry &r) {
    r.bind("tg.url", [] { return Value::of_str(hal::nvs_get_str("tgbridge", "url", "")); },
           [](const Value &v, Error &) {
               hal::nvs_set_str("tgbridge", "url", v.s.c_str());
               /* tg_cfg_set_url also mirrors into an open Telegram app. */
               const std::string url = v.s;
               apply_later([url] { apps::tg_cfg_set_url(url.c_str()); });
               return true;
           });
    bind_secret(r, "tg.token", "tgbridge", "token_enc",
                [](const char *v, std::string *e) { return apps::tg_cfg_set_token(v, e); });
    r.bind("tg.notify_vibrate", [] { return Value::of_bool(apps::tg_cfg_get_notif_vibrate()); },
           [](const Value &v, Error &) {
               apps::tg_cfg_set_notif_vibrate(v.b);
               return true;
           });
    r.bind("tg.notify_banner", [] { return Value::of_bool(apps::tg_cfg_get_notif_banner()); },
           [](const Value &v, Error &) {
               apps::tg_cfg_set_notif_banner(v.b);
               return true;
           });
    r.bind("tg.favorites",
           [] {
               std::vector<std::pair<long long, std::string>> favs;
               load_favs_nvs(favs);
               JsonWriter w;
               w.begin_array();
               for (const auto &f : favs) {
                   w.begin_object().key("id").int_val(f.first).key("title").str_val(f.second).end_object();
               }
               w.end_array();
               return Value::of_json(w.str());
           },
           [](const Value &v, Error &) {
               std::vector<std::string> items;
               array_objects(v.s, items);
               std::vector<std::pair<long long, std::string>> favs;
               for (const std::string &it : items) {
                   JsonObject o;
                   int64_t id = 0;
                   std::string title;
                   o.parse(it);
                   o.get_int("id", id);
                   o.get_string("title", title);
                   favs.push_back(std::make_pair(static_cast<long long>(id), title));
               }
               save_favs_nvs(favs);
               /* The Telegram app's in-memory set is LVGL-thread only. */
               apply_later([favs] { apps::tg_cfg_set_favorites(favs); });
               return true;
           },
           [](const Value &v, Error &err) {
               std::vector<std::string> items;
               if (!array_objects(v.s, items) || items.size() > 32) {
                   err.set("invalid_argument", "up to 32 favorites");
                   return false;
               }
               for (const std::string &it : items) {
                   JsonObject o;
                   int64_t id = 0;
                   std::string title;
                   if (!o.parse(it) || !o.get_int("id", id) || id == 0 ||
                       (o.has("title") && (!o.get_string("title", title) || title.size() > 64))) {
                       err.set("invalid_argument", "each favorite needs a non-zero id and a title up to 64 bytes");
                       return false;
                   }
                   for (unsigned char c : title) {
                       if (c < 0x20) {
                           err.set("invalid_argument", "title: control character");
                           return false;
                       }
                   }
               }
               return true;
           });
}

void bind_notes(Registry &r) {
    str_bind_nvs(r, "nsync.repo", "notesync", "repo");
    str_bind_nvs(r, "nsync.branch", "notesync", "branch");
    bind_secret(r, "nsync.token", "notesync", "token_enc",
                [](const char *v, std::string *e) { return apps::nsync_cfg_set_token(v, e); });

    r.bind("notes_sec.state", [] {
        if (!notes_crypto_is_enabled()) return Value::of_str("off");
        return Value::of_str(notes_crypto_is_unlocked() ? "unlocked" : "locked");
    });
    r.bind_action("notes_sec.lock", [](const JsonObject &, JsonWriter &out, Error &) {
        notes_crypto_lock();
        note_changed("notes_sec.state");
        out.begin_object().end_object();
        return true;
    });
}

void bind_storage(Registry &r) {
    r.bind("storage.msc_target",
           [] { return Value::of_str(user_setting.msc_prefer_sd ? "sd" : "int"); },
           [](const Value &v, Error &) {
               hw_set_msc_prefer_sd(v.s == "sd"); /* saves the blob + the lib's key */
               return true;
           });
    bind_blob_bool(r, "storage.prune_internal", &user_setting_params_t::prune_internal);

    r.bind_action("storage.copy_int_to_sd", [](const JsonObject &, JsonWriter &out, Error &) {
        return start_job("set_copy", out, [](std::string &msg, std::string &result) {
            int copied = 0, failed = 0;
            std::string why;
            const bool ok = hw_copy_internal_to_sd(&copied, &failed, &why);
            JsonWriter w;
            w.begin_object().key("copied").int_val(copied).key("failed").int_val(failed).end_object();
            result = w.str();
            msg = ok ? "done" : (why.empty() ? std::string("copy failed") : why);
            return ok;
        });
    });
    r.bind_action("storage.prune_now", [](const JsonObject &, JsonWriter &out, Error &) {
        return start_job("set_prune", out, [](std::string &msg, std::string &) {
            hw_prune_internal_storage();
            msg = "done";
            return true;
        });
    });
}

void bind_ssh(Registry &r) {
    str_bind_nvs(r, "ssh.host", "ssh", "host");
    str_bind_nvs(r, "ssh.user", "ssh", "user");
    r.bind("ssh.port", [] { return Value::of_int(hal::nvs_get_u16("ssh", "port", 22)); },
           [](const Value &v, Error &) {
               hal::nvs_set_u16("ssh", "port", static_cast<uint16_t>(v.i));
               return true;
           });
    bind_secret(r, "ssh.password", "ssh", "pw_enc", [](const char *v, std::string *e) {
        /* Passwords keep their spaces (no trim), like the SSH app's own prompt. */
        return hal::secret_store("ssh", "pw_enc", v, e, false);
    });
}

void power_off_confirmed(bool yes, void *) {
    if (!yes) return;
    hw_feedback();
    hw_shutdown();
}

void bind_sys(Registry &r) {
    r.bind_action("sys.power_off", [](const JsonObject &, JsonWriter &out, Error &) {
        core::ui_post([] {
            ui_confirm("Power off", "The phone asked to power off the pager.", "Cancel", "Power off",
                       power_off_confirmed, nullptr);
        });
        out.begin_object().key("confirm").str_val("on_pager").end_object();
        return true;
    });
}

Registry &build() {
    static Registry r;
    r.clear(boot_random());
    build_catalog(r, device_profile());
    bind_display(r);
    bind_power(r);
    bind_fonts(r);
    bind_editor(r);
    bind_time(r);
    bind_conn(r);
    bind_home(r);
    bind_hub_weather(r);
    bind_telegram(r);
    bind_notes(r);
    bind_storage(r);
    bind_ssh(r);
    bind_sys(r);
    return r;
}

}  // namespace

Registry &registry() {
    static Registry &r = build();
    return r;
}

bool set(const char *id, const Value &v, std::string *msg, bool apply) {
    Error err;
    std::vector<std::function<void()>> dropped;
    std::vector<std::function<void()>> *prev = t_apply_sink;
    if (!apply) t_apply_sink = &dropped;
    const bool ok = registry().set(id, v, err);
    t_apply_sink = prev;
    if (ok) return true;
    if (msg) *msg = err.msg;
    return false;
}

Value get(const char *id) {
    Value v;
    Error err;
    registry().get(id, v, err);
    return v;
}

void set_apply_sink(std::vector<std::function<void()>> *sink) {
    t_apply_sink = sink;
}

void note_changed(const char *id) {
    std::vector<std::string> ids;
    ids.push_back(id);
    registry().note_changed(ids);
}

}  // namespace settings

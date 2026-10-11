/**
 * @file      peripherals.cpp
 * @brief     NFC discovery and the NFC enable toggle.
 */
#include "peripherals.h"
#include "system.h"
#include "internal.h"
#include "wireless.h"
#include <string>

#ifdef ARDUINO
#include <LilyGoLib.h>
#include "nfc_reader.h"
#include "nfc_task.h"
#include "../nfc_provision.h"
#else
#include <climits>
#include <cstdlib>
#endif

// --- NFC ---------------------------------------------------------------

#if defined(USING_ST25R3916) && defined(ARDUINO)

/* WiFi NDEF tag handler. The upstream factory example has a rich confirm
 * dialog in ui_nfc.cpp; this codebase doesn't carry that file over, so we
 * stub it to a direct connect-and-notify flow. Enough to keep the link
 * closed and still surface a visible action when the user taps a WiFi tag. */
static void ui_nfc_pop_up(wifi_conn_params_t &params)
{
    ui_msg_pop_up("NFC WiFi",
                  (std::string("Connecting to ") + params.ssid + "...").c_str());
    hw_set_wifi_connect(params);
}

static nfc_diag_cb_t g_diag_cb = nullptr;

static void diag_emit(int kind, const char *text, unsigned len)
{
    if (g_diag_cb) g_diag_cb(kind, text, len);
}

static void nrf_notify_callback()
{
    Serial.println("NDEF Detected.");
    hw_feedback();
    diag_emit(0, nullptr, 0);
}

static void ndef_event_callback(ndefTypeId id, void *data)
{
    ndefTypeRtdDeviceInfo   devInfoData;
    ndefConstBuffer         bufAarString;
    ndefRtdUri              url;
    ndefRtdText             text;
    String msg = "";
    wifi_conn_params_t params;
    switch (id) {
    case NDEF_TYPE_EMPTY:
        break;
    case NDEF_TYPE_RTD_DEVICE_INFO:
        memcpy(&devInfoData, data, sizeof(ndefTypeRtdDeviceInfo));
        break;
    case NDEF_TYPE_RTD_TEXT:
        memcpy(&text, data, sizeof(ndefRtdText));
        // Credential provisioning intercepts `lilygo+<slot>:<value>` before
        // anything else sees the record — the tag payload is a secret, so it
        // must never reach serial, the NFC test screen (diag_emit) or a popup.
        if (nfc_provision_maybe_handle(
                reinterpret_cast<const char *>(text.bufSentence.buffer),
                text.bufSentence.length)) {
            break;
        }
        Serial.printf("LanguageCode: %s\nSentence: %s\n", reinterpret_cast < const char * > (text.bufLanguageCode.buffer), reinterpret_cast < const char * > (text.bufSentence.buffer));
        diag_emit(1, reinterpret_cast<const char *>(text.bufSentence.buffer),
                  text.bufSentence.length);
        msg.concat("LanguageCode: ");
        msg.concat(reinterpret_cast < const char * > (text.bufLanguageCode.buffer));
        msg.concat("\nSentence: ");
        msg.concat(reinterpret_cast < const char * > (text.bufSentence.buffer));
        ui_msg_pop_up("NFC Text", msg.c_str());
        break;
    case NDEF_TYPE_RTD_URI:
        memcpy(&url, data, sizeof(ndefRtdUri));
        Serial.printf("PROTOCOL:%s URL:%s\n", reinterpret_cast < const char * > (url.bufProtocol.buffer), reinterpret_cast < const char * > (url.bufUriString.buffer));
        diag_emit(2, reinterpret_cast<const char *>(url.bufUriString.buffer),
                  url.bufUriString.length);
        msg.concat("PROTOCOL:");
        msg.concat(reinterpret_cast < const char * > (url.bufProtocol.buffer));
        msg.concat("URL:");
        msg.concat(reinterpret_cast < const char * > (url.bufUriString.buffer));
        ui_msg_pop_up("NFC Url", msg.c_str());
        break;
    case NDEF_TYPE_RTD_AAR:
        memcpy(&bufAarString, data, sizeof(ndefConstBuffer));
        Serial.printf("NDEF_TYPE_RTD_AAR :%s\n", (char *)bufAarString.buffer);
        break;
    case NDEF_TYPE_MEDIA:
        break;
    case NDEF_TYPE_MEDIA_VCARD:
        break;
    case NDEF_TYPE_MEDIA_WIFI: {
        ndefTypeWifi *wifi = (ndefTypeWifi *)data;
        params.ssid = std::string(reinterpret_cast < const char * > (wifi->bufNetworkSSID.buffer), wifi->bufNetworkSSID.length);
        params.password = std::string(reinterpret_cast < const char * > (wifi->bufNetworkKey.buffer), wifi->bufNetworkKey.length);
        Serial.printf("ssid:<%s>\n", params.ssid.c_str());
        diag_emit(3, params.ssid.c_str(), params.ssid.size());
        ui_nfc_pop_up(params);
    }
    break;
    default:
        diag_emit(4, nullptr, 0);
        break;
    }
}
#endif  /*USING_ST25R3916*/

#if defined(USING_ST25R3916) && defined(ARDUINO)
static bool g_discovery_active = false;
#endif

bool hw_start_nfc_discovery()
{
#if  defined(USING_ST25R3916) && defined(ARDUINO)
    instance.powerControl(POWER_NFC, true);
    bool ok = beginNFC(nrf_notify_callback, ndef_event_callback);
    g_discovery_active = ok;
    // Kick the poller out of its idle block so it starts scanning at once,
    // rather than after its next idle-timeout re-check.
    if (ok) hw_nfc_task_notify_wake();
    return ok;
#else
    return false;
#endif
}

void hw_stop_nfc_discovery()
{
#if  defined(USING_ST25R3916) && defined(ARDUINO)
    deinitNFC();
    instance.powerControl(POWER_NFC, false);
    g_discovery_active = false;
#endif
}

bool hw_nfc_discovery_active()
{
#if defined(USING_ST25R3916) && defined(ARDUINO)
    return g_discovery_active;
#else
    return false;
#endif
}

void hw_set_nfc_diag_hook(nfc_diag_cb_t cb)
{
#if defined(USING_ST25R3916) && defined(ARDUINO)
    g_diag_cb = cb;
#else
    (void)cb;
#endif
}

bool hw_get_nfc_enable() { return user_setting.nfc_enable; }

// Toggling NFC also brings the discovery state machine up/down. Powering the
// chip alone isn't enough — `loopNFCReader()` runs from the NFC task
// (hal/nfc_task.cpp) but polls nothing until `beginNFC()` registers the callbacks and
// starts RFAL. The reference factory firmware only started discovery from
// the dedicated NFC app's lifecycle; we don't have that app, so the toggle
// is the single source of truth.
void hw_set_nfc_enable(bool en) {
    user_setting.nfc_enable = en;
#ifdef ARDUINO
    if (en) {
        hw_start_nfc_discovery();
    } else {
        hw_stop_nfc_discovery();
    }
    delay(10);
#endif
}

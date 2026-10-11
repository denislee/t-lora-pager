/**
 * @file      nfc_reader.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2024  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2024-10-11
 *
 */

#include "nfc_reader.h"
#include "../core/scoped_lock.h"

#if defined(ARDUINO) && defined(USING_ST25R3916)

enum NFCReaderState {
    ST_POLLING,
    ST_WAIT_RELEASED,
};

static uint8_t          *rawBuffer = NULL;
#define RAW_BUFFER_SIZE 1024
static NdefClass        ndef(&NFCReader);
static NFCReaderState   state = ST_POLLING;
static notify_callback_t ndef_notify_cb = NULL;
static ndef_event_callback_t ndef_event_cb = NULL;
static bool _nfc_running = false;

static void ndefRecordHandle(const ndefRecord *record);
static ReturnCode ndefRtdDeviceInfoDump(const ndefType *devInfo, ndefTypeRtdDeviceInfo *devInfoData);
static ReturnCode ndefRtdTextDump(const ndefType *text, ndefRtdText *rtdText);
static ReturnCode ndefRtdUriDump(const ndefType *uri, ndefRtdUri *rtdUri);
static ReturnCode ndefRtdAarDump(const ndefType *aar, ndefConstBuffer *bufAarString);
static ReturnCode ndefMediaWifiDump(const ndefType *wifi, ndefTypeWifi *wifiConfig);

static void ndefClassHandler()
{
    rfalNfcDevice *nfcDev;
    NFCReader.rfalNfcGetActiveDevice(&nfcDev);

    {
        char idbuf[3 * 16 + 1];
        int n = 0;
        int max_bytes = nfcDev->nfcidLen;
        if (max_bytes > 16) max_bytes = 16;
        for (int i = 0; i < max_bytes; i++) {
            n += snprintf(idbuf + n, sizeof(idbuf) - n, "%02X ", nfcDev->nfcid[i]);
        }
        log_d("NDEF ID: %s", idbuf);
    }

    // See if we can get an NDEF record from it
    ReturnCode  err = ndef.ndefPollerContextInitialization(nfcDev);
    if (err != ST_ERR_NONE) {
        return;
    }

    log_d("NDEF context initialized.");

    ndefInfo info;
    err = ndef.ndefPollerNdefDetect(&info);
    if (err != ST_ERR_NONE) {
        log_e("ndefPollerNdefDetect error: %u %s", err, ndef.errorToString(err));
        return;
    }

    log_d("NDEF detected.");
    uint32_t actual_size = 0;

    if (!rawBuffer) return;

    memset(rawBuffer, 0, RAW_BUFFER_SIZE);
    err = ndef.ndefPollerReadRawMessage(rawBuffer,  RAW_BUFFER_SIZE, &actual_size);
    if (err != ST_ERR_NONE) {
        return;
    }

    ndefMessage ndefMsg;
    ndefConstBuffer ndefBuf;
    ndefBuf.buffer = rawBuffer;
    ndefBuf.length = actual_size;

    err = ndef.ndefMessageDecode(&ndefBuf, &ndefMsg);
    if (err != ST_ERR_NONE) {
        log_e("NDEF message decode failed: %d", err);
        return;
    }

    // Got an NDEF "message" (a set of NDEF records)
    for (ndefRecord *record = ndefMessageGetFirstRecord(&ndefMsg); record != NULL;
         record = ndefMessageGetNextRecord(record)) {
        ndefRecordHandle(record);
    }
}

static void demoNotif(rfalNfcState st )
{
    if ( st == RFAL_NFC_STATE_WAKEUP_MODE ) {
        log_d("Wake Up mode started");
    } else if ( st == RFAL_NFC_STATE_POLL_TECHDETECT ) {
        log_d("Wake Up mode terminated. Polling for devices");
    } else if ( st == RFAL_NFC_STATE_POLL_SELECT ) {
        log_d("State poll select");
    } else if ( st == RFAL_NFC_STATE_START_DISCOVERY ) {
        log_d("State start discovery");
    } else if (st == RFAL_NFC_STATE_ACTIVATED) {
        if (ndef_notify_cb) {
            ndef_notify_cb();
        }
        ndefClassHandler();
        NFCReader.rfalNfcDeactivate(true);
        NFCReader.rfalNfcaPollerSleep();

        state = ST_WAIT_RELEASED;
    }
}



// Rate-limits the "tag can be removed" log line while the tag stays in the field.
static uint32_t interval = 0;

void loopNFCReader()
{
    if (!_nfc_running)return;
    switch (state) {
    case ST_POLLING:
        NFCReader.rfalNfcWorker();
        break;
    case ST_WAIT_RELEASED: {
        rfalNfcaSensRes sensRes;
        rfalNfcaSelRes  selRes;
        rfalNfcDevice   *nfcDev;
        NFCReader.rfalNfcGetActiveDevice(&nfcDev);
        NFCReader.rfalNfcaPollerInitialize();
        ReturnCode err = NFCReader.rfalNfcaPollerCheckPresence(RFAL_14443A_SHORTFRAME_CMD_WUPA, &sensRes);
        if (err == ST_ERR_NONE) {
            if (((nfcDev->dev.nfca.type == RFAL_NFCA_T1T) && (!rfalNfcaIsSensResT1T(&sensRes))) ||
                    ((nfcDev->dev.nfca.type != RFAL_NFCA_T1T) && (NFCReader.rfalNfcaPollerSelect(nfcDev->dev.nfca.nfcId1, nfcDev->dev.nfca.nfcId1Len, &selRes) != ST_ERR_NONE))) {
                state = ST_POLLING;
                log_d("Start discovery");
                return ;
            }
            if (millis() > interval) {
                log_d("Operation completed, tag can be removed from the field");
                interval = millis() + 1000;
            }
            NFCReader.rfalNfcaPollerSleep();
        } else if (err == ST_ERR_TIMEOUT) {
            state = ST_POLLING;
            log_d("Start discovery");
            return ;
        }
    }
    break;
    default:
        break;
    }
}

// Decodes one record's well-known type and hands it to the event callback.
// vCard records are reported with no payload (nothing consumes them).
static void ndefRecordHandle(const ndefRecord *record)
{
    static ndefTypeRtdDeviceInfo   devInfoData;
    static ndefConstBuffer         bufAarString;
    static ndefTypeWifi            wifiConfig;
    static ndefRtdUri              url;
    static ndefRtdText             rtdText;

    ndefType   type;
    if (ndef.ndefRecordToType(record, &type) != ST_ERR_NONE) {
        return;
    }
    void *user_data = NULL;
    switch (type.id) {
    case NDEF_TYPE_RTD_DEVICE_INFO:
        ndefRtdDeviceInfoDump(&type, &devInfoData);
        user_data = &devInfoData;
        break;
    case NDEF_TYPE_RTD_TEXT:
        ndefRtdTextDump(&type, &rtdText);
        user_data = &rtdText;
        break;
    case NDEF_TYPE_RTD_URI:
        ndefRtdUriDump(&type, &url);
        user_data = &url;
        break;
    case NDEF_TYPE_RTD_AAR:
        ndefRtdAarDump(&type, &bufAarString);
        user_data = &bufAarString;
        break;
    case NDEF_TYPE_MEDIA_WIFI:
        ndefMediaWifiDump(&type, &wifiConfig);
        user_data = &wifiConfig;
        break;
    case NDEF_TYPE_EMPTY:
    case NDEF_TYPE_MEDIA_VCARD:
    case NDEF_TYPE_ID_COUNT:
    default:
        break;
    }
    if (ndef_event_cb) {
        ndef_event_cb(type.id, user_data);
    }
}

static ReturnCode ndefRtdDeviceInfoDump(const ndefType *devInfo, ndefTypeRtdDeviceInfo *devInfoData)
{

    if (devInfo == NULL) {
        return ST_ERR_PARAM;
    }
    if (devInfoData == NULL) {
        return ST_ERR_PARAM;
    }
    if (devInfo->id != NDEF_TYPE_RTD_DEVICE_INFO) {
        return ST_ERR_PARAM;
    }

    ndef.ndefGetRtdDeviceInfo(devInfo, devInfoData);
    return ST_ERR_NONE;
}

static ReturnCode ndefRtdTextDump(const ndefType *text, ndefRtdText *rtdText)
{

    if (text == NULL || rtdText == NULL ) {
        return ST_ERR_PARAM;
    }
    if (text->id != NDEF_TYPE_RTD_TEXT) {
        return ST_ERR_PARAM;
    }

    ndef.ndefGetRtdText(text, &rtdText->utfEncoding, &rtdText->bufLanguageCode, &rtdText->bufSentence);
    return ST_ERR_NONE;
}

static ReturnCode ndefRtdUriDump(const ndefType *uri, ndefRtdUri *rtdUri)
{
    if (uri == NULL || rtdUri == NULL) {
        return ST_ERR_PARAM;
    }
    if (uri->id != NDEF_TYPE_RTD_URI) {
        return ST_ERR_PARAM;
    }
    ndef.ndefGetRtdUri(uri, &rtdUri->bufProtocol, &rtdUri->bufUriString);
    return ST_ERR_NONE;
}

static ReturnCode ndefRtdAarDump(const ndefType *aar, ndefConstBuffer *bufAarString)
{
    if (aar == NULL || bufAarString == NULL) {
        return ST_ERR_PARAM;
    }
    if (aar->id != NDEF_TYPE_RTD_AAR) {
        return ST_ERR_PARAM;
    }
    ndef.ndefGetRtdAar(aar, bufAarString);
    return ST_ERR_NONE;
}

static ReturnCode ndefMediaWifiDump(const ndefType *wifi, ndefTypeWifi *wifiConfig)
{
    if (wifi == NULL || wifiConfig == NULL) {
        return ST_ERR_PARAM;
    }
    if (wifi->id != NDEF_TYPE_MEDIA_WIFI) {
        return ST_ERR_PARAM;
    }
    ndef.ndefGetWifi(wifi, wifiConfig);
    return ST_ERR_NONE;
}

bool beginNFC(notify_callback_t notify_cb, ndef_event_callback_t event_cb)
{
    if (!rawBuffer) {
        rawBuffer = (uint8_t *)ps_malloc(RAW_BUFFER_SIZE);
        if (!rawBuffer) {
            Serial.println("rawBuffer allocation failed!");
            return false;
        }
    }
    ndef_notify_cb = notify_cb;
    ndef_event_cb = event_cb;
    rfalNfcDiscoverParam discover_params;
    discover_params.devLimit = 1;
    discover_params.techs2Find = RFAL_NFC_POLL_TECH_A;
    discover_params.GBLen = RFAL_NFCDEP_GB_MAX_LEN;
    discover_params.notifyCb = demoNotif;
    discover_params.totalDuration = 1000U;
    discover_params.wakeupEnabled = false;
    Serial.print("Starting discovery ");
    // Reinitialize NFC reader
    NFCReader.rfalNfcInitialize();
    if (NFCReader.rfalNfcDiscover(&discover_params) != ST_ERR_NONE) {
        Serial.println("failed!");
        return false;
    }
    Serial.println("success.");
    state = ST_POLLING;
    _nfc_running = true;
    NFCReader.rfalNfcDeactivate(true);
    return true;
}

void deinitNFC()
{
    NFCReader.rfalNfcDeactivate(false);
    _nfc_running = false;
    if (rawBuffer) {
        free(rawBuffer);
        rawBuffer = NULL;
    }
}

#endif /*ARDUINO*/

/**
 * @file      audio.cpp
 * @brief     Audio player (WAV task), microphone recording.
 */
#include "audio.h"
#include "internal.h"
#include "system.h"
#include "../core/kb_lock.h"
#include "../core/spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <SD.h>
#include <Esp.h>
#endif

// --- Player task / event group ----------------------------------------

#ifdef ARDUINO
static TaskHandle_t       playerTaskHandler = NULL;
static QueueHandle_t      playerQueue       = NULL;
static EventGroupHandle_t playerEvent       = NULL;
// Signalled by the recorder task when it has fully closed the WAV file, so
// hw_rec_stop() can block on it instead of busy-polling recorder_running.
static SemaphoreHandle_t  recorderDoneSem   = NULL;

#define PLAYER_PLAY     _BV(0)
#define PLAYER_END      _BV(1)
#define PLAYER_RUNNING  _BV(2)
// Set by the player when it clears PLAYER_RUNNING on exit, so callers can
// block for the stop to complete rather than polling hw_player_running().
#define PLAYER_STOPPED  _BV(3)

// Stream-plays a 16 kHz / 16-bit / mono WAV file from SD without loading it
// all into PSRAM (a 5-minute note is ~10 MB). Header is a fixed-size 44-byte
// RIFF/WAVE/fmt/data layout — we trust it because we write it ourselves
// (wav_write_header below). Output goes to the audio codec on the
// pager and to the I2S PCM amplifier on the watches.
#if defined(USING_AUDIO_CODEC) || defined(USING_PCM_AMPLIFIER)
static void playWAV_sd(const char *filename)
{
    char path[128];
    snprintf(path, sizeof(path), "/%s", filename);
    File f;
    {
        core::ScopedSpiLock lock;
        f = SD.open(path);
        if (!f) {
            log_e("SD open %s failed", path);
            return;
        }
        if (f.size() < 44 || !f.seek(44)) {
            log_e("%s is not a playable WAV", path);
            f.close();
            return;
        }
    }

    auto close_with_lock = [&] {
        core::ScopedSpiLock lock;
        f.close();
    };

#if defined(USING_AUDIO_CODEC)
    if (!(HW_CODEC_ONLINE & hw_get_device_online())) {
        log_e("Audio codec not online");
        close_with_lock();
        return;
    }
    int ret;
    {
        core::ScopedI2cLock i2c;  // P7.45 (codec + PA callback -> XL9555)
        ret = instance.codec.open(16, 1, HW_REC_SAMPLE_RATE);
    }
    if (ret < 0) {
        log_e("codec.open failed 0x%X", ret);
        close_with_lock();
        return;
    }
#else // USING_PCM_AMPLIFIER
    {
        core::ScopedI2cLock i2c;  // P7.45
        instance.powerControl(POWER_SPEAK, true);
    }
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    instance.player.configureTX(HW_REC_SAMPLE_RATE, 16, (i2s_channel_t)1);
#else
    instance.player.configureTX(HW_REC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
#endif

    auto close_output = [] {
#if defined(USING_AUDIO_CODEC)
        {
            core::ScopedI2cLock i2c;  // P7.45
            instance.codec.close();
        }
#else
        // Back to the user's speaker setting rather than forcing it off.
        {
            core::ScopedI2cLock i2c;  // P7.45
            instance.powerControl(POWER_SPEAK, user_setting.speaker_enable);
        }
#endif
    };

    xEventGroupSetBits(playerEvent, PLAYER_RUNNING);

    const size_t CHUNK = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        close_output();
        close_with_lock();
        xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
        xEventGroupSetBits(playerEvent, PLAYER_STOPPED);
        return;
    }

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(playerEvent, PLAYER_PLAY | PLAYER_END,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & PLAYER_END) break;

        int n;
        {
            core::ScopedSpiLock lock;
            n = f.read(buf, CHUNK);
        }
        if (n <= 0) break;

#if defined(USING_AUDIO_CODEC)
        instance.codec.write(buf, (size_t)n);
#else
        instance.player.write(buf, (size_t)n);
#endif
    }

    free(buf);
    close_output();
    close_with_lock();
    xEventGroupClearBits(playerEvent, PLAYER_RUNNING | PLAYER_PLAY | PLAYER_END);
    xEventGroupSetBits(playerEvent, PLAYER_STOPPED);
}
#endif

// Only WAV is supported: it is the only format the firmware records, and the
// only thing hw_set_sd_music_play() is ever asked to play.
static void hw_sd_play(const char *filename)
{
    size_t len = strlen(filename);
    bool isWAV = (len > 4 && strcasecmp(filename + len - 4, ".wav") == 0);
    if (!isWAV) {
        log_e("Cannot play %s: only .wav is supported", filename);
        return;
    }
#if defined(USING_AUDIO_CODEC) || defined(USING_PCM_AMPLIFIER)
    playWAV_sd(filename);
#else
    log_e("Cannot play %s: this board has no audio output", filename);
#endif
}

static void playerTask(void *args)
{
    audio_params_t params;
    while (1) {
        if (xQueueReceive(playerQueue, &params, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (params.event == APP_EVENT_PLAY) {
            log_d("Event: filename:%s", params.filename);
            hw_sd_play(params.filename);
        }
    }
    playerTaskHandler = NULL;
    vTaskDelete(NULL);
}
#endif // ARDUINO

void hw_audio_init()
{
#ifdef ARDUINO
    playerQueue = xQueueCreate(2, sizeof(audio_params_t));
    playerEvent = xEventGroupCreate();
    recorderDoneSem = xSemaphoreCreateBinary();
    xTaskCreate(playerTask, "app/play", 8 * 1024, NULL, 12, &playerTaskHandler);
#endif
}

void hw_audio_deinit_task()
{
#ifdef ARDUINO
    if (playerTaskHandler) {
        vTaskDelete(playerTaskHandler);
        playerTaskHandler = NULL;
    }
#endif
}

// --- Recording: WAV / 16 kHz / 16-bit / mono → SD card ----------------

bool hw_mic_available()
{
#if defined(ARDUINO) && defined(USING_PDM_MICROPHONE)
    return true;
#elif defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    return (HW_CODEC_ONLINE & hw_get_device_online()) != 0;
#else
    return false;
#endif
}

#ifdef ARDUINO
static TaskHandle_t    recorderTaskHandler = NULL;
static volatile bool   recorder_stop_req  = false;
static volatile bool   recorder_running   = false;
static volatile uint32_t recorder_start_ms = 0;
static volatile uint32_t recorder_bytes   = 0;
static File            recFile;
static std::string     recPath;

static void wav_write_header(File &f, uint32_t data_bytes)
{
    uint32_t sample_rate = HW_REC_SAMPLE_RATE;
    uint16_t channels    = 1;
    uint16_t bits        = 16;
    uint32_t byte_rate   = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);
    uint32_t riff_size   = 36 + data_bytes;
    uint32_t fmt_size    = 16;
    uint16_t fmt_pcm     = 1;

    uint8_t h[44];
    memcpy(h +  0, "RIFF", 4);
    memcpy(h +  4, &riff_size, 4);
    memcpy(h +  8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt_pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &sample_rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    f.write(h, 44);
}

static void recorderTask(void *args)
{
    const int frame_samples = 512;
    // Match the codec's actual channel count so the sample rate in the file
    // header matches real time. Reading stereo-sized blocks from a codec
    // opened as mono decimates the audio and causes 2x-fast playback.
    const uint8_t in_channels = instance.getCodecInputChannels();
    const size_t in_bytes   = (size_t)frame_samples * in_channels * sizeof(int16_t);
    const size_t mono_bytes = (size_t)frame_samples * sizeof(int16_t);

    int16_t *in_buf   = (int16_t *)heap_caps_malloc(in_bytes,   MALLOC_CAP_SPIRAM);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(mono_bytes, MALLOC_CAP_SPIRAM);

    if (!in_buf || !mono_buf) {
        log_e("recorder: buffer alloc failed");
        if (in_buf) free(in_buf);
        if (mono_buf) free(mono_buf);
        {
            core::ScopedSpiLock lock;
            if (recFile) recFile.close();
        }
        recorder_running = false;
        recorderTaskHandler = NULL;
        if (recorderDoneSem) xSemaphoreGive(recorderDoneSem);
        vTaskDelete(NULL);
        return;
    }

#if defined(USING_AUDIO_CODEC)
    // Codec online state is hotplug-driven; latch once for the recording
    // session instead of probing the device bitmask on every frame.
    const bool codec_online = (HW_CODEC_ONLINE & hw_get_device_online()) != 0;
#endif

    while (!recorder_stop_req) {
        uint32_t elapsed = millis() - recorder_start_ms;
        if (elapsed >= HW_REC_MAX_MS) break;

#if defined(USING_PDM_MICROPHONE)
        instance.mic.readBytes((char *)in_buf, in_bytes);
#elif defined(USING_AUDIO_CODEC)
        if (!codec_online) break;
        int rret = instance.codec.read((uint8_t *)in_buf, in_bytes);
        if (rret != 0) {
            log_e("codec.read failed: 0x%X", rret);
            break;
        }
#else
        break;
#endif

        const uint8_t *write_src;
        size_t write_len;
        if (in_channels >= 2) {
            for (int i = 0; i < frame_samples; ++i) {
                int32_t s = (int32_t)in_buf[2 * i] + (int32_t)in_buf[2 * i + 1];
                mono_buf[i] = (int16_t)(s / 2);
            }
            write_src = (const uint8_t *)mono_buf;
            write_len = mono_bytes;
        } else {
            write_src = (const uint8_t *)in_buf;
            write_len = in_bytes;
        }

        size_t written;
        {
            core::ScopedSpiLock lock;
            written = recFile.write(write_src, write_len);
        }
        recorder_bytes += written;
        if (written != write_len) {
            log_e("recorder: short write %u/%u (SD full?)",
                  (unsigned)written, (unsigned)write_len);
            break;
        }
    }

    // Finalize WAV header with actual data size.
    {
        core::ScopedSpiLock lock;
        if (recFile) {
            recFile.seek(0);
            wav_write_header(recFile, recorder_bytes);
            recFile.close();
        }
    }

#if defined(USING_AUDIO_CODEC)
    if (codec_online) {
        core::ScopedI2cLock i2c;  // P7.45
        instance.codec.close();
    }
#endif

    free(in_buf);
    free(mono_buf);
    recorder_running = false;
    recorderTaskHandler = NULL;
    if (recorderDoneSem) xSemaphoreGive(recorderDoneSem);
    vTaskDelete(NULL);
}
#endif /*ARDUINO*/

bool hw_rec_start(const char *sd_path)
{
#ifdef ARDUINO
    if (recorder_running || hw_player_running()) return false;
    if (!hw_mic_available()) return false;
    if (!sd_path || !sd_path[0]) return false;

    {
        core::ScopedSpiLock lock;
        recFile = SD.open(sd_path, FILE_WRITE);
        if (!recFile) {
            log_e("recorder: SD.open(%s) failed", sd_path);
            return false;
        }
        wav_write_header(recFile, 0);
    }

#if defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        int ret;
        {
            core::ScopedI2cLock i2c;  // P7.45
            ret = instance.codec.open(16, instance.getCodecInputChannels(),
                                      HW_REC_SAMPLE_RATE);
        }
        if (ret < 0) {
            log_e("recorder: codec.open failed 0x%X", ret);
            core::ScopedSpiLock lock;
            recFile.close();
            SD.remove(sd_path);
            return false;
        }
    }
#endif

    recPath            = sd_path;
    recorder_stop_req  = false;
    recorder_bytes     = 0;
    recorder_start_ms  = millis();
    recorder_running   = true;

    // Clear any completion token left by a self-terminated prior recording so
    // hw_rec_stop() waits for *this* session, not a stale one.
    if (recorderDoneSem) xSemaphoreTake(recorderDoneSem, 0);

    // Pinned to core 0 (OPTIMIZATION_PHASE3.md P3.10): unpinned at prio 12 could
    // land on core 1 and preempt the lvgl task (prio 8), stalling frames for
    // codec-frame durations while recording.
    if (xTaskCreatePinnedToCore(recorderTask, "app/rec", 8 * 1024, NULL, 12,
                    &recorderTaskHandler, 0) != pdPASS) {
        log_e("recorder: task create failed");
        recorder_running = false;
#if defined(USING_AUDIO_CODEC)
        if (HW_CODEC_ONLINE & hw_get_device_online()) {
            {
                core::ScopedI2cLock i2c;  // P7.45
                instance.codec.close();
            }
        }
#endif
        {
            core::ScopedSpiLock lock;
            recFile.close();
            SD.remove(sd_path);
        }
        return false;
    }
    return true;
#else
    (void)sd_path;
    return false;
#endif
}

void hw_rec_stop()
{
#ifdef ARDUINO
    if (!recorder_running) return;
    recorder_stop_req = true;
    // Block (not busy-poll) until the recorder task has flushed its final SD
    // write, rewritten the WAV header and closed the file. Bounded so a stalled
    // SD write can't freeze the UI thread indefinitely; on timeout we fall back
    // to the old poll to preserve the synchronous "file is closed on return"
    // contract callers (finalize_recording) rely on.
    if (recorderDoneSem) {
        if (xSemaphoreTake(recorderDoneSem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            while (recorder_running) delay(5);
        }
    } else {
        while (recorder_running) delay(5);
    }
#endif
}

bool hw_rec_running()
{
#ifdef ARDUINO
    return recorder_running;
#else
    return false;
#endif
}

uint32_t hw_rec_elapsed_ms()
{
#ifdef ARDUINO
    if (!recorder_running) return 0;
    return (uint32_t)(millis() - recorder_start_ms);
#else
    return 0;
#endif
}

uint32_t hw_rec_bytes_written()
{
#ifdef ARDUINO
    return recorder_bytes;
#else
    return 0;
#endif
}

// --- Speaker / volume -------------------------------------------------

bool hw_get_speaker_enable() { return user_setting.speaker_enable; }
void hw_set_speaker_enable(bool en) {
    user_setting.speaker_enable = en;
#ifdef ARDUINO
    {
        core::ScopedI2cLock i2c;  // P7.45
        instance.powerControl(POWER_SPEAK, en);
    }
    delay(10);
#endif
}

void hw_set_volume(uint8_t volume)
{
#if defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        {
            core::ScopedI2cLock i2c;  // P7.45
            instance.codec.setVolume(volume);
        }
    } else {
        log_d("Audio codec not online!");
    }
#endif //USING_AUDIO_CODEC
}

uint8_t hw_get_volume()
{
#if defined(ARDUINO) && defined(USING_AUDIO_CODEC)
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        core::ScopedI2cLock i2c;  // P7.45
        return instance.codec.getVolume();
    } else {
        return 0;
    }
#else
    return 100;
#endif //USING_AUDIO_CODEC
}

// --- Playback control -------------------------------------------------

void hw_set_sd_music_play(const char *filename)
{
    log_d("hw_set_sd_music_play: %s", filename);
#ifdef ARDUINO
    audio_params_t params = {
        .event = APP_EVENT_PLAY,
        .filename = filename,
    };
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END | PLAYER_STOPPED);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        log_d("waiting for the player to stop");
        // Block on PLAYER_STOPPED rather than busy-polling; bounded so a wedged
        // player task can't freeze the UI thread.
        xEventGroupWaitBits(playerEvent, PLAYER_STOPPED, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
        log_d("player stopped");
    }
    xEventGroupSetBits(playerEvent, PLAYER_PLAY);
    xQueueSend(playerQueue, &params, portMAX_DELAY);
    log_d("hw_set_sd_music_play: queued");
#endif
}

void hw_set_play_stop()
{
#ifdef ARDUINO
    xEventGroupClearBits(playerEvent, PLAYER_PLAY | PLAYER_END | PLAYER_STOPPED);
    if (hw_player_running()) {
        xEventGroupSetBits(playerEvent, PLAYER_END);
        xEventGroupWaitBits(playerEvent, PLAYER_STOPPED, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
    }
#endif
}

bool hw_player_running()
{
#ifdef ARDUINO
    return xEventGroupGetBits(playerEvent) & PLAYER_RUNNING;
#endif
    return true;
}

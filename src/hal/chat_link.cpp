/**
 * @file      chat_link.cpp
 * @brief     chat_link_ask() — see chat_link.h.
 *
 * One chat turn over PagerLink: optionally announce a voice memo
 * (chat.audio.begin), stream it as DATA (hw_link_send_bytes), then run
 * chat.ask. Blocking; the caller runs it on a worker task/thread.
 */
#include "chat_link.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "link.h"
#include "link/link_json.h"
#include "link/link_xfer.h"
#include "storage.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

#ifdef ARDUINO
// hw_read_sd_stream() sink state: append into the caller's buffer.
struct AudioSink {
    uint8_t *buf;
    size_t cap;
    size_t filled;
};

// Copy one chunk in; refuse to run past the caller's buffer.
static bool audio_sink(void *user, const uint8_t *data, size_t len)
{
    AudioSink *s = static_cast<AudioSink *>(user);
    if (s->filled + len > s->cap) return false;
    memcpy(s->buf + s->filled, data, len);
    s->filled += len;
    return true;
}
#endif

// Read the whole file at `path` into a malloc'd buffer the caller frees with
// free(). Rejects an empty capture and an over-long memo before allocating.
static bool read_audio(const std::string &path, uint8_t *&buf, size_t &len, HalError &e,
                       std::string &err)
{
    buf = nullptr;
    len = 0;
    auto fail = [&e, &err](HalError code, const char *msg) {
        e = code;
        err = msg;
        return false;
    };

#ifdef ARDUINO
    const size_t n = hw_get_file_size(path.c_str());
    if (n <= 44) return fail(HalError::InvalidArgument, "no audio captured");
    if (n > CHAT_AUDIO_MAX_BYTES) {
        return fail(HalError::InvalidArgument, "voice memo too long (max 15 s)");
    }

    uint8_t *b = static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
    if (!b) b = static_cast<uint8_t *>(malloc(n));
    if (!b) return fail(HalError::InternalError, "audio read failed");

    AudioSink sink = {b, n, 0};
    const bool ok = hw_read_sd_stream(path.c_str(), 4096, audio_sink, &sink) && sink.filled == n;
    if (!ok) {
        free(b);
        return fail(HalError::InternalError, "audio read failed");
    }
    buf = b;
    len = n;
    return true;
#else
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return fail(HalError::InternalError, "audio read failed");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return fail(HalError::InternalError, "audio read failed");
    }
    const long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return fail(HalError::InternalError, "audio read failed");
    }

    const size_t n = static_cast<size_t>(end);
    if (n <= 44) {
        fclose(f);
        return fail(HalError::InvalidArgument, "no audio captured");
    }
    if (n > CHAT_AUDIO_MAX_BYTES) {
        fclose(f);
        return fail(HalError::InvalidArgument, "voice memo too long (max 15 s)");
    }

    uint8_t *b = static_cast<uint8_t *>(malloc(n));
    if (!b) {
        fclose(f);
        return fail(HalError::InternalError, "audio read failed");
    }
    const size_t got = fread(b, 1, n, f);
    fclose(f);
    if (got != n) {
        free(b);
        return fail(HalError::InternalError, "audio read failed");
    }
    buf = b;
    len = n;
    return true;
#endif
}

HalResult<ChatReply> chat_link_ask(const std::string &text, const std::string &audio_path,
                                   std::string &err)
{
    err.clear();

    if (text.empty() && audio_path.empty()) {
        err = "nothing to send";
        return HalError::InvalidArgument;
    }

    if (!hw_link_connected()) {
        err = hal_error_string(HalError::LinkOffline);
        return HalError::LinkOffline;
    }

    // Optional voice memo: announce it, stream the bytes, then reference the
    // transfer id from chat.ask.
    bool have_audio = false;
    uint16_t audio_xfer = 0;
    if (!audio_path.empty()) {
        uint8_t *buf = nullptr;
        size_t len = 0;
        HalError aerr = HalError::InternalError;
        if (!read_audio(audio_path, buf, len, aerr, err)) return aerr;

        const uint32_t crc = pagerlink::crc32_update(0, buf, len);
        pagerlink::JsonWriter w;
        w.begin_object()
            .key("size")
            .uint_val(static_cast<uint64_t>(len))
            .key("crc32")
            .uint_val(crc)
            .key("fmt")
            .str_val("wav")
            .end_object();

        const HwLinkResult b = hw_link_call("chat.audio.begin", w.str());
        if (!b.ok) {
            free(buf);
            return hal_error_from_link(b, err);
        }

        pagerlink::JsonObject obj;
        int64_t xfer = 0;
        if (!obj.parse(b.json) || !obj.get_int("xfer", xfer) || xfer <= 0 || xfer > 0xFFFF) {
            free(buf);
            err = "phone: bad chat.audio.begin reply";
            return HalError::InternalError;
        }

        const bool sent = hw_link_send_bytes(static_cast<uint16_t>(xfer), buf, len);
        free(buf);
        if (!sent) {
            if (!hw_link_connected()) {
                err = hal_error_string(HalError::LinkOffline);
                return HalError::LinkOffline;
            }
            err = "phone: audio upload failed";
            return HalError::InternalError;
        }
        have_audio = true;
        audio_xfer = static_cast<uint16_t>(xfer);
    }

    pagerlink::JsonWriter w;
    w.begin_object();
    if (!text.empty()) w.key("text").str_val(text);
    if (have_audio) w.key("audio_xfer").uint_val(audio_xfer);
    w.end_object();

    const HwLinkResult r = hw_link_call("chat.ask", w.str(), CHAT_ASK_TIMEOUT_MS);
    if (!r.ok) return hal_error_from_link(r, err);

    pagerlink::JsonObject obj;
    if (!obj.parse(r.json)) {
        err = "phone: bad chat.ask reply";
        return HalError::InternalError;
    }
    ChatReply out;
    obj.get_string("transcript", out.transcript);
    obj.get_string("reply", out.reply);
    if (out.transcript.empty() && out.reply.empty()) {
        err = "phone returned no reply";
        return HalError::InternalError;
    }
    return out;
}

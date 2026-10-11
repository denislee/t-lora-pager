/**
 * @file      handlers_test.cpp
 * @brief     Emulator-only test hook so `pagerlink smoke` can drive
 *            pager-initiated calls end to end.
 *
 * Never compiled for hardware: it would let a phone make the pager call
 * arbitrary methods.
 */
#ifndef ARDUINO

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hal/chat_link.h"
#include "hal/link.h"
#include "hal/link/fs_volume.h"
#include "hal/link/link_json.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"
#include "hal/link/link_xfer.h"
#include "hal/notes_link.h"

namespace pagerlink {
namespace {

/** Default hw_link_call() timeout, matching Rpc::DEFAULT_TIMEOUT_MS. */
constexpr int TEST_DEFAULT_TIMEOUT_MS = 10000;
/** Largest `bytes` payload the sink step will generate (4 MiB). */
constexpr int TEST_MAX_BYTES = 4 * 1024 * 1024;

/**
 * Chat step of test.callback: run chat_link_ask() (text, or a synthetic WAV of
 * `pcm_bytes` PCM) against the phone and report {step:"chat", ok, code, msg,
 * reply, transcript, bytes} as evt.test.result. `bytes` is the WAV file size.
 */
void run_chat_step(const std::string &mode, const std::string &text, size_t pcm_bytes) {
    std::string path;
    size_t file_bytes = 0;
    bool wrote = true;
    if (mode == "voice") {
        char name[64];
        std::snprintf(name, sizeof(name), "/tmp/pagerlink_chat_%p.wav",
                      static_cast<const void *>(&path));
        path = name;
        /* 16 kHz / 16-bit / mono, like hw_rec_start(). */
        const uint32_t rate = 16000, data = static_cast<uint32_t>(pcm_bytes);
        auto le32 = [](std::vector<uint8_t> &v, uint32_t x) {
            for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
        };
        auto le16 = [](std::vector<uint8_t> &v, uint16_t x) {
            v.push_back(static_cast<uint8_t>(x));
            v.push_back(static_cast<uint8_t>(x >> 8));
        };
        std::vector<uint8_t> wav;
        wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
        le32(wav, 36 + data);
        wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        le32(wav, 16);
        le16(wav, 1);
        le16(wav, 1);
        le32(wav, rate);
        le32(wav, rate * 2);
        le16(wav, 2);
        le16(wav, 16);
        wav.insert(wav.end(), {'d', 'a', 't', 'a'});
        le32(wav, data);
        for (size_t i = 0; i < pcm_bytes; ++i) wav.push_back(static_cast<uint8_t>((i * 13 + 5) & 0xFF));
        file_bytes = wav.size();
        FILE *f = std::fopen(path.c_str(), "wb");
        wrote = f && std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
        if (f) std::fclose(f);
    }

    std::string emsg;
    HalResult<ChatReply> r = wrote ? chat_link_ask(mode == "text" ? text : std::string(), path, emsg)
                                   : HalResult<ChatReply>(HalError::InternalError);
    if (!wrote) emsg = "temp wav write failed";
    if (!path.empty()) std::remove(path.c_str());

    JsonWriter w;
    w.begin_object()
        .key("step")
        .str_val("chat")
        .key("ok")
        .bool_val(r.is_ok())
        .key("code")
        .str_val(r.is_ok() ? "" : hal_error_code(r.error()))
        .key("msg")
        .str_val(emsg)
        .key("reply")
        .str_val(r.is_ok() ? r.value().reply : std::string())
        .key("transcript")
        .str_val(r.is_ok() ? r.value().transcript : std::string())
        .key("bytes")
        .uint_val(static_cast<uint64_t>(file_bytes))
        .end_object();
    service_emit("evt.test.result", w.str());
}

/**
 * Notes step of test.callback: seed /notes on the int and sd volumes, then run
 * the notes sync (mode "github") or the copy-to-phone path (mode "phone")
 * against the phone and report {step:"notes", ok, code, msg, uploaded, already,
 * errors, seeded} as evt.test.result. `seeded` maps each seeded path to its
 * bytes in lowercase hex, so the caller can recompute the CRCs the phone sees.
 */
void run_notes_step(const std::string &mode) {
    /* One seeded file: its volume, its "<vol>/<name>" label for `seeded`, its
     * leaf name under /notes and the bytes written. */
    struct SeedFile {
        pagerlink::Vol vol;
        const char *label;
        const char *name;
        std::vector<uint8_t> data;
    };

    auto bytes_of = [](const char *s) {
        const std::string t(s);
        return std::vector<uint8_t>(t.begin(), t.end());
    };

    /* An encrypted note: OpenSSL `enc` "Salted__" magic + 56 random bytes. */
    std::vector<uint8_t> secret;
    secret.insert(secret.end(), {'S', 'a', 'l', 't', 'e', 'd', '_', '_'});
    {
        std::random_device rd;
        std::mt19937 g(rd());
        for (int i = 0; i < 56; ++i) secret.push_back(static_cast<uint8_t>(g() & 0xFF));
    }

    std::vector<SeedFile> files;
    files.push_back({pagerlink::Vol::Int, "int/a-plain.txt", "a-plain.txt",
                     bytes_of("hello from internal a\n")});
    files.push_back({pagerlink::Vol::Int, "int/b-shared.txt", "b-shared.txt",
                     bytes_of("internal copy of shared\n")});
    files.push_back(
        {pagerlink::Vol::Int, "int/c-secret.txt", "c-secret.txt", std::move(secret)});
    files.push_back({pagerlink::Vol::Sd, "sd/d-sdonly.txt", "d-sdonly.txt",
                     bytes_of("sd only note\n")});
    files.push_back({pagerlink::Vol::Sd, "sd/b-shared.txt", "b-shared.txt",
                     bytes_of("SD copy of shared (must lose)\n")});

    /* Fresh /notes on both volumes, then the five files. */
    bool seed_ok = true;
    for (const pagerlink::Vol v : {pagerlink::Vol::Int, pagerlink::Vol::Sd}) {
        (void)pagerlink::vol_remove(v, "/notes");
        if (!pagerlink::vol_mkdir(v, "/notes")) seed_ok = false;
    }
    for (const SeedFile &f : files) {
        const std::string path = std::string("/notes/") + f.name;
        if (!pagerlink::vol_write_at(f.vol, path, 0, f.data.data(), f.data.size())) {
            seed_ok = false;
        }
    }

    HalResult<NotesSyncSummary> r = HalResult<NotesSyncSummary>(HalError::InternalError);
    std::string emsg;
    if (seed_ok) {
        if (mode == "github") {
            std::vector<NoteEntry> manifest;
            if (!notes_link_manifest(false, manifest, emsg)) {
                r = HalError::InternalError;
            } else {
                r = notes_link_sync(manifest, {"github"}, nullptr, emsg);
            }
        } else {
            r = notes_link_copy_to_phone(emsg);
        }
    } else {
        emsg = "seed failed";
    }

    std::map<std::string, std::string> seeded;
    static const char kHex[] = "0123456789abcdef";
    for (const SeedFile &f : files) {
        std::string hex;
        hex.reserve(f.data.size() * 2);
        for (const uint8_t b : f.data) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 0x0F]);
        }
        seeded[f.label] = std::move(hex);
    }

    JsonWriter w;
    w.begin_object()
        .key("step")
        .str_val("notes")
        .key("ok")
        .bool_val(r.is_ok())
        .key("code")
        .str_val(r.is_ok() ? "" : hal_error_code(r.error()))
        .key("msg")
        .str_val(emsg)
        .key("uploaded")
        .int_val(r.is_ok() ? r.value().uploaded : 0)
        .key("already")
        .int_val(r.is_ok() ? r.value().already : 0)
        .key("errors")
        .int_val(r.is_ok() ? static_cast<int64_t>(r.value().errors.size()) : 0)
        .key("seeded")
        .begin_object();
    for (const SeedFile &f : files) w.key(f.label).str_val(seeded[f.label]);
    w.end_object().end_object();
    service_emit("evt.test.result", w.str());
}

/**
 * test.callback — drive one pager-initiated exchange from the phone and report
 * the outcome back as evt.test.result. Exactly one of:
 *   method + params + timeout_ms -> call `method` and report the result.
 *   sink + bytes                 -> call `sink` for {size,crc32} of a generated
 *                                   pattern, then send those bytes as DATA.
 *   chat:"text" + text           -> chat_link_ask(text) against the phone.
 *   chat:"voice" + bytes         -> write a WAV (44-byte header + `bytes` PCM)
 *                                   to a temp file and chat_link_ask() it.
 *   notes:"github"|"phone"       -> seed /notes on int+sd, run the notes sync
 *                                   (github) or copy-to-phone path, report
 *                                   counts + seeded bytes (hex).
 * The work runs on a detached thread so the RES can answer immediately; the
 * phone then observes the follow-up evt.test.result.
 */
bool handle_test_callback(const JsonObject &params, JsonWriter &result, RpcError &err) {
    (void)result; /* answered as RES {} before the thread's work happens */

    std::string method;
    std::string sink;
    const bool has_method = params.get_string("method", method);
    const bool has_sink = params.get_string("sink", sink);
    std::string chat;
    std::string chat_text;
    const bool has_chat = params.get_string("chat", chat);
    std::string notes;
    const bool has_notes = params.get_string("notes", notes);
    if (int(has_method) + int(has_sink) + int(has_chat) + int(has_notes) != 1) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "exactly one of method/sink/chat/notes";
        return false;
    }
    if (has_chat && chat != "text" && chat != "voice") {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "chat must be text or voice";
        return false;
    }
    if (has_notes && notes != "github" && notes != "phone") {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "notes must be github or phone";
        return false;
    }
    params.get_string("text", chat_text);
    if ((has_method && method.empty()) || (has_sink && sink.empty())) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "empty method/sink";
        return false;
    }

    int64_t timeout_ms = TEST_DEFAULT_TIMEOUT_MS;
    if (params.has("timeout_ms") &&
        (!params.get_int("timeout_ms", timeout_ms) || timeout_ms < 0)) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "bad timeout_ms";
        return false;
    }

    std::string call_params;
    if (params.has("params") && !params.get_raw("params", call_params)) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "bad params";
        return false;
    }

    int64_t bytes = 0;
    if (params.has("bytes") &&
        (!params.get_int("bytes", bytes) || bytes < 0 || bytes > TEST_MAX_BYTES)) {
        err.code = hal_error_code(HalError::InvalidArgument);
        err.msg = "bad bytes";
        return false;
    }

    const uint32_t timeout = static_cast<uint32_t>(timeout_ms);
    const size_t count = static_cast<size_t>(bytes);

    if (has_chat) {
        std::thread([chat, chat_text, count] { run_chat_step(chat, chat_text, count); }).detach();
        return true;
    }

    if (has_notes) {
        std::thread([notes] { run_notes_step(notes); }).detach();
        return true;
    }

    std::thread([has_method, method, call_params, sink, timeout, count] {
        if (has_method) {
            const HwLinkResult r = hw_link_call(method.c_str(), call_params, timeout);
            JsonWriter w;
            w.begin_object()
                .key("step")
                .str_val("call")
                .key("ok")
                .bool_val(r.ok)
                .key("code")
                .str_val(r.code)
                .key("msg")
                .str_val(r.msg)
                .key("json");
            if (r.ok) {
                w.raw_val(r.json.empty() ? "{}" : r.json.c_str());
            } else {
                w.null_val();
            }
            w.end_object();
            service_emit("evt.test.result", w.str());
            return;
        }

        /* Sink: stream a deterministic pattern the phone can CRC-check. */
        std::vector<uint8_t> buf(count);
        for (size_t i = 0; i < count; ++i) {
            buf[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
        }
        const uint32_t crc = crc32_update(0, buf.data(), buf.size());

        JsonWriter pw;
        pw.begin_object()
            .key("size")
            .uint_val(static_cast<uint64_t>(count))
            .key("crc32")
            .uint_val(crc)
            .end_object();
        const HwLinkResult b = hw_link_call(sink.c_str(), pw.str());

        bool ok = false;
        if (b.ok) {
            JsonObject obj;
            int64_t xfer = 0;
            if (obj.parse(b.json) && obj.get_int("xfer", xfer)) {
                ok = hw_link_send_bytes(static_cast<uint16_t>(xfer), buf.data(), buf.size());
            }
        }

        JsonWriter w;
        w.begin_object()
            .key("step")
            .str_val("sink")
            .key("ok")
            .bool_val(ok)
            .key("code")
            .str_val(b.code)
            .key("bytes")
            .uint_val(static_cast<uint64_t>(count))
            .key("crc32")
            .uint_val(crc)
            .end_object();
        service_emit("evt.test.result", w.str());
    }).detach();

    return true;
}

}  // namespace

void register_test_handlers(Rpc &rpc) {
    rpc.handle("test.callback", handle_test_callback);
}

}  // namespace pagerlink

#endif  // !ARDUINO

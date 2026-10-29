/**
 * @file      handlers_ota.cpp
 * @brief     PagerLink ota.status / ota.apply (PROTOCOL.md §6.7).
 *
 * The image arrives as an ordinary fs.write upload; ota.apply validates the
 * request on the link task, answers {"job"} and runs the job on its own
 * worker: pass 1 hashes the file (phase "verify") and checks the ESP image
 * magic, pass 2 streams it into the next slot while hashing again (phase
 * "write"), so a file that changed between the passes is refused rather than
 * flashed. Progress goes out as evt.ota (service_emit is safe from any task).
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../notes_path.h"
#include "../ota.h"
#include "../ota_state.h"
#include "../pbkdf2_sha256.h"
#include "../result.h"

#include "hal/link/fs_volume.h"
#include "hal/link/link_json.h"
#include "hal/link/link_log.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <chrono>
#include <thread>
#endif

namespace pagerlink {
namespace {

constexpr size_t kChunk = 4096;
constexpr uint32_t kProgressStep = 64 * 1024;

std::atomic<bool> s_running{false};
std::atomic<uint32_t> s_next_job{1};

struct Job {
    std::string id;
    Vol vol = Vol::Int;
    std::string path;
    uint32_t size = 0;
    uint8_t sha[32];
};

void emit(const Job &j, const char *phase, uint32_t done, const std::string &msg = std::string()) {
    JsonWriter w;
    w.begin_object()
        .key("job").str_val(j.id)
        .key("phase").str_val(phase)
        .key("done").uint_val(done)
        .key("total").uint_val(j.size);
    if (!msg.empty()) w.key("msg").str_val(msg);
    w.end_object();
    service_emit("evt.ota", w.str());
}

/** One pass over the file: hash it, and hand each chunk to `sink` (may be null). */
bool pass(const Job &j, const char *phase, bool (*sink)(const uint8_t *, size_t, std::string &),
          std::string &err) {
    VolFile f;
    if (!f.open_read(j.vol, j.path)) {
        err = "cannot open the image";
        return false;
    }
    hal::Sha256Stream h;
    hal::sha256_init(&h);
    std::vector<uint8_t> buf(kChunk);
    uint32_t off = 0, next_report = 0;
    while (off < j.size) {
        const size_t n = (j.size - off) < kChunk ? (j.size - off) : kChunk;
        if (!f.read_at(off, buf.data(), n)) {
            err = "read failed (file changed?)";
            return false;
        }
        if (off == 0 && !hal::ota_image_header_ok(buf.data(), n)) {
            err = "not an ESP32 app image";
            return false;
        }
        hal::sha256_update(&h, buf.data(), n);
        if (sink && !sink(buf.data(), n, err)) return false;
        off += static_cast<uint32_t>(n);
        if (off >= next_report || off == j.size) {
            emit(j, phase, off);
            next_report = off + kProgressStep;
        }
    }
    uint8_t got[32];
    hal::sha256_final(&h, got);
    if (std::memcmp(got, j.sha, 32) != 0) {
        err = "sha256 mismatch";
        return false;
    }
    return true;
}

void run(std::unique_ptr<Job> j) {
    std::string err;
    bool ok = pass(*j, "verify", nullptr, err);
    if (ok) ok = hw_ota_slot_begin(j->size, err);
    if (ok) {
        ok = pass(*j, "write", hw_ota_slot_write, err);
        if (ok) {
            ok = hw_ota_slot_finish(err);
        } else {
            hw_ota_slot_abort();
        }
    }
    if (!ok) {
        LINK_LOG("ota %s failed: %s", j->id.c_str(), err.c_str());
        emit(*j, "error", 0, err);
        s_running.store(false);
        return;
    }
#ifdef ARDUINO
    LINK_LOG("ota %s written; rebooting", j->id.c_str());
    emit(*j, "reboot", j->size);
    vTaskDelay(pdMS_TO_TICKS(1000));  // let evt.ota reach the phone
    hw_ota_reboot();
#else
    emit(*j, "installed", j->size);
#endif
    s_running.store(false);
}

#ifdef ARDUINO
void task_entry(void *arg) {
    run(std::unique_ptr<Job>(static_cast<Job *>(arg)));
    vTaskDelete(nullptr);
}
#endif

bool bad(RpcError &err, const char *msg) {
    err.code = hal_error_code(HalError::InvalidArgument);
    err.msg = msg;
    return false;
}

bool handle_ota_status(const JsonObject &, JsonWriter &result, RpcError &) {
    const HwOtaStatus st = hw_ota_status();
    result.begin_object()
        .key("running").str_val(st.running)
        .key("next").str_val(st.next)
        .key("pending").bool_val(st.pending)
        .key("boots").uint_val(st.boots)
        .key("max_size").uint_val(st.max_size)
        .end_object();
    return true;
}

/** Start the job's worker; on failure the phone learns it from evt.ota. */
void spawn(Job *raw) {
#ifdef ARDUINO
    if (xTaskCreate(task_entry, "ota_apply", 8192, raw, 2, nullptr) != pdPASS) {
        emit(*raw, "error", 0, "no task for the update");
        delete raw;
        s_running.store(false);
    }
#else
    std::thread([](Job *j) { run(std::unique_ptr<Job>(j)); }, raw).detach();
#endif
}

bool handle_ota_apply(Rpc &rpc, const JsonObject &params, JsonWriter &result, RpcError &err) {
    std::unique_ptr<Job> j(new Job());
    std::string vol, path, sha;
    int64_t size = -1;
    if (!params.get_string("vol", vol) || !vol_from_string(vol, j->vol)) return bad(err, "bad vol");
    char norm[256];
    if (!params.get_string("path", path) || !sandbox_path(path.c_str(), norm, sizeof(norm)) ||
        std::strcmp(norm, "/") == 0) {
        return bad(err, "bad path");
    }
    j->path = norm;
    if (!params.get_int("size", size) || size <= 0) return bad(err, "bad size");
    if (!params.get_string("sha256", sha) || !hal::sha256_hex_parse(sha, j->sha)) {
        return bad(err, "bad sha256");
    }
    if (!vol_available(j->vol)) return bad(err, "sd not mounted");
    VolEntry st;
    if (!vol_stat(j->vol, j->path, st) || st.dir) return bad(err, "no such file");
    if (st.size != static_cast<uint64_t>(size)) return bad(err, "size differs from the file");
    if (static_cast<uint64_t>(size) > hw_ota_status().max_size) return bad(err, "image larger than the slot");
    j->size = static_cast<uint32_t>(size);

    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) {
        err.code = "busy";
        err.msg = "an update is running";
        return false;
    }
    char id[16];
    std::snprintf(id, sizeof(id), "o%u", static_cast<unsigned>(s_next_job.fetch_add(1)));
    j->id = id;
    result.begin_object().key("job").str_val(j->id).end_object();
    // After the RES, so the phone knows the job id before its first evt.ota.
    Job *raw = j.release();
    rpc.defer([raw]() { spawn(raw); });
    return true;
}

}  // namespace

void register_ota_handlers(Rpc &rpc) {
    rpc.handle("ota.status", handle_ota_status);
    rpc.handle("ota.apply", [&rpc](const JsonObject &p, JsonWriter &r, RpcError &e) {
        return handle_ota_apply(rpc, p, r, e);
    });
}

}  // namespace pagerlink

/**
 * @file      handlers_fs.cpp
 * @brief     PagerLink v1 fs.* handlers and bulk transfers (PROTOCOL.md §4, §6).
 *
 * Raw on-disk bytes only: an encrypted note travels as its Salted__ ciphertext.
 * Writes land in <path>.part and are renamed on a CRC-matching commit, so a
 * dropped link never leaves a half-written file.
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../notes_path.h"
#include "../storage.h"
#include "../system.h"

#include "hal/link/fs_volume.h"
#include "hal/link/link_frame.h"
#include "hal/link/link_json.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"
#include "hal/link/link_xfer.h"

#ifdef ARDUINO
#include <esp32-hal-log.h>
#define LINK_LOG(...) log_i(__VA_ARGS__)
#else
#include <cstdio>
#define LINK_LOG(fmt, ...) printf("[link] " fmt "\n", ##__VA_ARGS__)
#endif

namespace pagerlink {
namespace {

/** A transfer idle for longer than this is abandoned by fs_tick(). */
constexpr uint32_t STALL_MS = 30000;
/** Concurrent bulk transfers (one read + one write is the common case). */
constexpr size_t MAX_XFERS = 2;
/** Sandboxed path buffer (notes_path.h sandbox_path). */
constexpr size_t PATH_CAP = 192;
/** fs.list stops adding entries once the reply would pass this many bytes. */
constexpr size_t LIST_BUDGET = 3800;

/** One live bulk transfer: a read we send, or a write we receive. */
struct Transfer {
    uint16_t id;
    bool upload;       /* true when DATA flows phone -> pager */
    Vol vol;
    std::string path;  /* final path */
    std::string part;  /* upload staging path ("<path>.part") */
    uint32_t crc;      /* CRC-32/ISO-HDLC of the whole file */
    bool overwrite;
    uint32_t last_ms;  /* now_ms() at the start or the last bulk message */
    std::unique_ptr<XferSender> tx;   /* reads */
    std::unique_ptr<XferReceiver> rx; /* writes */
};

Rpc *g_rpc = nullptr;
std::vector<Transfer> g_xfers;
uint16_t g_next_upload_id = 1;
uint32_t g_last_boost_ms = 0;

/** Monotonic milliseconds, wrapping at 2^32. Read directly rather than taken
 * from fs_tick(): the TCP transport only ticks when the socket is idle, so a
 * tick-fed clock would freeze for the length of a busy transfer. */
uint32_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/** Keep the CPU at full speed while bytes move (P6.11 busy vote), rate-limited. */
void boost() {
    const uint32_t now = now_ms();
    if (g_last_boost_ms == 0 || static_cast<uint32_t>(now - g_last_boost_ms) >= 500) {
        hw_cpu_boost_for_input();
        g_last_boost_ms = now;
    }
}

Transfer *find(uint16_t id) {
    for (Transfer &t : g_xfers) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

/** Erase a transfer without touching its files. */
void erase(uint16_t id) {
    for (size_t i = 0; i < g_xfers.size(); ++i) {
        if (g_xfers[i].id == id) {
            g_xfers.erase(g_xfers.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

/** Erase a transfer, removing an upload's staged .part (ignore failures). */
void drop(uint16_t id) {
    Transfer *t = find(id);
    if (!t) return;
    if (t->upload && !t->part.empty()) {
        vol_remove(t->vol, t->part);
    }
    erase(id);
}

/** True when `st` is a file whose first bytes are already Salted__ ciphertext. */
bool is_protected(Vol v, const std::string &path, const VolEntry &st) {
    if (st.dir || st.size < 8) return false;
    char magic[8];
    if (!vol_read_at(v, path, 0, reinterpret_cast<uint8_t *>(magic), sizeof(magic))) {
        return false;
    }
    return content_has_salted_magic(magic, sizeof(magic));
}

/** Fail with a mapped HalError code and a short lowercase reason. */
bool fail(RpcError &err, HalError e, const char *msg) {
    err.code = hal_error_code(e);
    err.msg = msg;
    return false;
}

/** Fail with a literal link-only code ("busy"). */
bool fail_code(RpcError &err, const char *code, const char *msg) {
    err.code = code;
    err.msg = msg;
    return false;
}

bool get_vol(const JsonObject &p, Vol &v, RpcError &err) {
    std::string s;
    if (!p.get_string("vol", s) || !vol_from_string(s, v)) {
        return fail(err, HalError::InvalidArgument, "bad vol");
    }
    if (!vol_available(v)) {
        return fail(err, HalError::InvalidArgument, "sd not mounted");
    }
    return true;
}

bool get_path(const JsonObject &p, const char *key, std::string &out, RpcError &err) {
    std::string s;
    if (!p.get_string(key, s)) {
        return fail(err, HalError::InvalidArgument, "bad path");
    }
    char buf[PATH_CAP];
    if (!sandbox_path(s.c_str(), buf, sizeof(buf))) {
        return fail(err, HalError::InvalidArgument, "bad path");
    }
    out = buf;
    return true;
}

/** Unsigned member: fails on a negative value or one past 32 bits. */
bool get_uint32(const JsonObject &p, const char *key, uint32_t &out) {
    int64_t v = 0;
    if (!p.get_int(key, v) || v < 0 || v > 0xFFFFFFFFLL) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

/** Transfer-id member: 1..65535. */
bool get_xfer_id(const JsonObject &p, const char *key, uint16_t &out) {
    int64_t v = 0;
    if (!p.get_int(key, v) || v < 1 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

/** CRC-32/ISO-HDLC over `size` bytes of a file, read in MAX_DATA_CHUNK chunks. */
bool file_crc(Vol v, const std::string &path, uint32_t size, uint32_t &out) {
    std::vector<uint8_t> buf(MAX_DATA_CHUNK);
    uint32_t crc = 0;
    uint32_t off = 0;
    while (off < size) {
        const size_t n = std::min<size_t>(MAX_DATA_CHUNK, size - off);
        if (!vol_read_at(v, path, off, buf.data(), n)) return false;
        crc = crc32_update(crc, buf.data(), n);
        off += static_cast<uint32_t>(n);
    }
    out = crc;
    return true;
}

/** Parent directory of an absolute sandbox path ("/" for a top-level entry). */
std::string parent_of(const std::string &path) {
    const size_t slash = path.rfind('/');
    std::string parent = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
    if (parent.empty()) parent = "/";
    return parent;
}

/** Next odd transfer id the pager owns, skipping any still active. */
uint16_t alloc_upload_id() {
    uint16_t id = g_next_upload_id;
    for (size_t i = 0; i < 32768 && (find(id) || service_accept_owns(id)); ++i) {
        id = (id == 65535) ? 1 : static_cast<uint16_t>(id + 2);
    }
    g_next_upload_id = (id == 65535) ? 1 : static_cast<uint16_t>(id + 2);
    return id;
}

/** Send whatever the credit window allows; finish the transfer when done. */
void pump_read(uint16_t id) {
    Transfer *t = find(id);
    if (!t || t->upload || !t->tx) return;
    const XferState state = t->tx->pump();
    if (state == XferState::Done || state == XferState::Failed) {
        LINK_LOG("fs.read %s id=%u path=%s size=%u",
                 state == XferState::Done ? "done" : "failed", static_cast<unsigned>(id),
                 t->path.c_str(), static_cast<unsigned>(t->tx->size()));
        drop(id);
    }
}

// --------------------------------------------------------------------------
// Handlers
// --------------------------------------------------------------------------

bool handle_list(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;

    uint32_t limit = 64;
    if (params.has("limit")) {
        if (!get_uint32(params, "limit", limit)) {
            return fail(err, HalError::InvalidArgument, "bad limit");
        }
        if (limit < 1) limit = 1;
        if (limit > 64) limit = 64;
    }
    uint32_t cursor = 0;
    if (params.has("cursor")) {
        if (!get_uint32(params, "cursor", cursor)) {
            return fail(err, HalError::InvalidArgument, "bad cursor");
        }
    }

    std::vector<VolEntry> entries;
    if (!vol_list(v, path, entries)) return fail(err, HalError::InvalidArgument, "not a directory");
    if (cursor > entries.size()) return fail(err, HalError::InvalidArgument, "bad cursor");

    result.begin_object().key("entries").begin_array();
    size_t used = result.str().size();
    uint32_t i = cursor;
    uint32_t emitted = 0;
    while (i < entries.size() && emitted < limit) {
        const VolEntry &e = entries[i];
        /* Estimate before appending so a big listing is paged, not truncated. */
        const size_t cost = e.name.size() + 64;
        if (used + cost > LIST_BUDGET) break;
        result.begin_object()
            .key("name").str_val(e.name)
            .key("dir").bool_val(e.dir)
            .key("size").uint_val(e.size)
            .key("mtime").uint_val(e.mtime)
            .end_object();
        used += cost;
        ++i;
        ++emitted;
    }
    result.end_array();
    if (i < entries.size()) result.key("cursor").uint_val(i);
    result.end_object();
    return true;
}

bool handle_stat(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;

    VolEntry st;
    if (!vol_stat(v, path, st)) return fail(err, HalError::InvalidArgument, "not found");

    result.begin_object()
        .key("dir").bool_val(st.dir)
        .key("size").uint_val(st.size)
        .key("mtime").uint_val(st.mtime)
        .key("protected").bool_val(is_protected(v, path, st))
        .end_object();
    return true;
}

bool handle_read(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;

    /* The phone receives the DATA, so it allocates the (even) transfer id. */
    uint16_t xfer = 0;
    if (!get_xfer_id(params, "xfer", xfer) || (xfer % 2) != 0 || find(xfer)) {
        return fail(err, HalError::InvalidArgument, "bad xfer");
    }
    if (g_xfers.size() >= MAX_XFERS) return fail_code(err, "busy", "too many transfers");

    VolEntry st;
    if (!vol_stat(v, path, st)) return fail(err, HalError::InvalidArgument, "not found");
    if (st.dir) return fail(err, HalError::InvalidArgument, "not a file");

    uint32_t offset = 0;
    if (params.has("offset")) {
        if (!get_uint32(params, "offset", offset)) {
            return fail(err, HalError::InvalidArgument, "bad offset");
        }
    }
    if (offset > st.size) return fail(err, HalError::InvalidArgument, "bad offset");

    uint32_t crc = 0;
    if (!file_crc(v, path, st.size, crc)) return fail(err, HalError::InternalError, "io error");
    const bool prot = is_protected(v, path, st);

    Transfer t;
    t.id = xfer;
    t.upload = false;
    t.vol = v;
    t.path = path;
    t.crc = crc;
    t.last_ms = now_ms();
    t.tx.reset(new XferSender(
        st.size, MAX_DATA_CHUNK,
        [v, path](uint32_t off, uint8_t *buf, size_t n) {
            return vol_read_at(v, path, off, buf, n);
        },
        [xfer](const std::string &payload) {
            return g_rpc && g_rpc->send_bulk(MsgType::Data, xfer, payload);
        },
        offset));
    g_xfers.push_back(std::move(t));

    result.begin_object()
        .key("xfer").uint_val(xfer)
        .key("size").uint_val(st.size)
        .key("crc32").uint_val(crc)
        .key("protected").bool_val(prot)
        .end_object();

    boost();
    LINK_LOG("fs.read start id=%u path=%s size=%u offset=%u", static_cast<unsigned>(xfer),
             path.c_str(), static_cast<unsigned>(st.size), static_cast<unsigned>(offset));
    /* The first DATA must follow the RES, hence defer(). */
    g_rpc->defer([xfer] { pump_read(xfer); });
    return true;
}

bool handle_write_begin(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;

    uint32_t size = 0;
    uint32_t crc = 0;
    if (!get_uint32(params, "size", size)) return fail(err, HalError::InvalidArgument, "bad size");
    if (!get_uint32(params, "crc32", crc)) return fail(err, HalError::InvalidArgument, "bad crc32");

    bool overwrite = false;
    if (params.has("overwrite") && !params.get_bool("overwrite", overwrite)) {
        return fail(err, HalError::InvalidArgument, "bad overwrite");
    }
    bool resume = false;
    if (params.has("resume") && !params.get_bool("resume", resume)) {
        return fail(err, HalError::InvalidArgument, "bad resume");
    }
    if (resume) return fail(err, HalError::NotSupported, "resume not supported");
    if (g_xfers.size() >= MAX_XFERS) return fail_code(err, "busy", "too many transfers");
    if (path == "/") return fail(err, HalError::InvalidArgument, "bad path");

    VolEntry st;
    if (vol_stat(v, path, st)) {
        if (st.dir) return fail(err, HalError::InvalidArgument, "not a file");
        if (!overwrite) return fail(err, HalError::InvalidArgument, "exists");
    }

    const std::string parent = parent_of(path);
    VolEntry pst;
    if (!vol_stat(v, parent, pst) || !pst.dir) {
        return fail(err, HalError::InvalidArgument, "not a directory");
    }

    uint64_t free_b = 0;
    if (vol_free_bytes(v, free_b) && static_cast<uint64_t>(size) > free_b) {
        return fail(err, HalError::StorageFull, "no space");
    }

    const std::string part = path + ".part";
    vol_remove(v, part); /* drop any stale staging file */
    if (!vol_write_at(v, part, 0, nullptr, 0)) {
        return fail(err, HalError::InternalError, "io error");
    }

    const uint16_t id = alloc_upload_id();
    Transfer t;
    t.id = id;
    t.upload = true;
    t.vol = v;
    t.path = path;
    t.part = part;
    t.crc = crc;
    t.overwrite = overwrite;
    t.last_ms = now_ms();
    t.rx.reset(new XferReceiver(
        size,
        [v, part](uint32_t off, const uint8_t *bytes, size_t n) {
            return vol_write_at(v, part, off, bytes, n);
        },
        [id](const std::string &payload) {
            return g_rpc && g_rpc->send_bulk(MsgType::Ack, id, payload);
        },
        INITIAL_CREDITS, 4, 0));
    g_xfers.push_back(std::move(t));

    result.begin_object().key("xfer").uint_val(id).key("offset").uint_val(0).end_object();
    boost();
    LINK_LOG("fs.write begin id=%u path=%s size=%u overwrite=%d", static_cast<unsigned>(id),
             path.c_str(), static_cast<unsigned>(size), overwrite ? 1 : 0);
    return true;
}

bool handle_write_commit(const JsonObject &params, JsonWriter &result, RpcError &err) {
    uint16_t id = 0;
    if (!get_xfer_id(params, "xfer", id)) {
        return fail(err, HalError::InvalidArgument, "no such transfer");
    }
    Transfer *t = find(id);
    if (!t || !t->upload || !t->rx) {
        return fail(err, HalError::InvalidArgument, "no such transfer");
    }

    /* Copy what drop() will invalidate before any early exit. */
    const Vol v = t->vol;
    const std::string path = t->path;
    const std::string part = t->part;
    const uint32_t want_crc = t->crc;
    const bool overwrite = t->overwrite;
    const uint32_t size = t->rx->size();

    if (t->rx->state() != XferState::Done) {
        drop(id);
        return fail(err, HalError::InvalidArgument, "incomplete");
    }

    uint32_t crc = 0;
    if (!file_crc(v, part, size, crc)) {
        drop(id);
        return fail(err, HalError::InternalError, "io error");
    }
    if (crc != want_crc) {
        LINK_LOG("fs.write commit id=%u path=%s crc mismatch", static_cast<unsigned>(id),
                 path.c_str());
        drop(id); /* removes the .part */
        return fail(err, HalError::InvalidArgument, "crc mismatch");
    }

    VolEntry st;
    if (vol_stat(v, path, st)) {
        if (!overwrite) {
            drop(id);
            return fail(err, HalError::InvalidArgument, "exists");
        }
        if (!vol_remove(v, path)) {
            drop(id);
            return fail(err, HalError::InternalError, "io error");
        }
    }
    if (!vol_rename(v, part, path)) {
        drop(id);
        return fail(err, HalError::InternalError, "io error");
    }

    erase(id); /* the .part is gone; nothing left to clean up */
    hw_set_filesystem_dirty(true);
    LINK_LOG("fs.write done id=%u path=%s size=%u", static_cast<unsigned>(id), path.c_str(),
             static_cast<unsigned>(size));
    result.begin_object().end_object();
    return true;
}

bool handle_write_abort(const JsonObject &params, JsonWriter &result, RpcError &err) {
    uint16_t id = 0;
    if (!get_xfer_id(params, "xfer", id) || !find(id)) {
        return fail(err, HalError::InvalidArgument, "no such transfer");
    }
    drop(id);
    result.begin_object().end_object();
    return true;
}

bool handle_delete(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;
    if (path == "/") return fail(err, HalError::InvalidArgument, "bad path");

    VolEntry st;
    if (!vol_stat(v, path, st)) return fail(err, HalError::InvalidArgument, "not found");
    if (!vol_remove(v, path)) return fail(err, HalError::InternalError, "io error");
    hw_set_filesystem_dirty(true);
    result.begin_object().end_object();
    return true;
}

bool handle_mkdir(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string path;
    if (!get_vol(params, v, err) || !get_path(params, "path", path, err)) return false;
    if (path == "/") return fail(err, HalError::InvalidArgument, "bad path");

    VolEntry pst;
    if (!vol_stat(v, parent_of(path), pst) || !pst.dir) {
        return fail(err, HalError::InvalidArgument, "not a directory");
    }
    VolEntry st;
    if (vol_stat(v, path, st) && !st.dir) {
        return fail(err, HalError::InvalidArgument, "exists");
    }
    if (!vol_mkdir(v, path)) return fail(err, HalError::InternalError, "io error");
    hw_set_filesystem_dirty(true);
    result.begin_object().end_object();
    return true;
}

bool handle_rename(const JsonObject &params, JsonWriter &result, RpcError &err) {
    Vol v;
    std::string from;
    std::string to;
    if (!get_vol(params, v, err) || !get_path(params, "from", from, err) ||
        !get_path(params, "to", to, err)) {
        return false;
    }
    if (from == "/" || to == "/" || from == to) {
        return fail(err, HalError::InvalidArgument, "bad path");
    }
    if (to.compare(0, from.size() + 1, from + "/") == 0) {
        return fail(err, HalError::InvalidArgument, "bad path"); /* into its own subtree */
    }

    VolEntry fst;
    if (!vol_stat(v, from, fst)) return fail(err, HalError::InvalidArgument, "not found");
    VolEntry tst;
    if (vol_stat(v, to, tst)) return fail(err, HalError::InvalidArgument, "exists");
    if (!vol_stat(v, parent_of(to), tst) || !tst.dir) {
        return fail(err, HalError::InvalidArgument, "not a directory");
    }
    if (!vol_rename(v, from, to)) return fail(err, HalError::InternalError, "io error");
    hw_set_filesystem_dirty(true);
    result.begin_object().end_object();
    return true;
}

}  // namespace

bool fs_owns_xfer(uint16_t xfer) { return find(xfer) != nullptr; }

bool fs_on_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
    Transfer *t = find(xfer);
    if (!t) {
        return false; /* not an fs transfer; the caller decides where it goes */
    }
    boost();
    t->last_ms = now_ms();

    if (type == MsgType::Ack && !t->upload && t->tx) {
        t->tx->on_ack(payload, len);
        pump_read(xfer); /* sends more DATA, or finishes the read */
    } else if (type == MsgType::Data && t->upload && t->rx) {
        if (t->rx->on_data(payload, len) == XferState::Failed) {
            LINK_LOG("fs.write failed id=%u path=%s", static_cast<unsigned>(xfer),
                     t->path.c_str());
            drop(xfer); /* a Done transfer stays until commit() */
        }
    }
    return true;
}

void fs_tick(uint32_t now) {
    for (size_t i = 0; i < g_xfers.size();) {
        if (static_cast<uint32_t>(now - g_xfers[i].last_ms) > STALL_MS) {
            LINK_LOG("fs stalled id=%u path=%s", static_cast<unsigned>(g_xfers[i].id),
                     g_xfers[i].path.c_str());
            drop(g_xfers[i].id); /* erases index i; do not advance */
        } else {
            ++i;
        }
    }
}

void fs_reset() {
    while (!g_xfers.empty()) {
        drop(g_xfers.front().id);
    }
    g_rpc = nullptr;
}

void register_fs_handlers(Rpc &rpc) {
    g_rpc = &rpc;
    rpc.handle("fs.list", handle_list);
    rpc.handle("fs.stat", handle_stat);
    rpc.handle("fs.read", handle_read);
    rpc.handle("fs.write.begin", handle_write_begin);
    rpc.handle("fs.write.commit", handle_write_commit);
    rpc.handle("fs.write.abort", handle_write_abort);
    rpc.handle("fs.delete", handle_delete);
    rpc.handle("fs.mkdir", handle_mkdir);
    rpc.handle("fs.rename", handle_rename);
}

}  // namespace pagerlink

/**
 * @file      ui_screen_capture.cpp
 * @brief     screen.capture (PROTOCOL.md §6.6): snapshot the screen on the
 *            LVGL thread, then send it to the phone from a worker.
 *
 * The pager renders in partial mode, so there is no full framebuffer to read
 * (src/emu_shots.cpp reads the emulator's direct-mode one). lv_snapshot_take
 * renders the active screen into an RGB565 buffer; the top layer (banners,
 * popups) is rendered as ARGB8888 and blended over it, so the capture shows
 * what the user sees. The pixels go out as `rgb565-rle` (hal/rle565.h) through
 * screen.data → DATA, the chat.audio.begin pattern.
 *
 * State: s_busy (one capture at a time) and the static OneShotJob; both
 * outlive any app.
 */
#include "ui_companion.h"

#include <lvgl.h>

#include <atomic>
#include <string>

#include "core/notify.h"
#include "core/one_shot_job.h"
#include "core/ui_post.h"
#include "hal/link.h"
#include "hal/link/link_json.h"
#include "hal/link/link_xfer.h"
#include "hal/rle565.h"

namespace {

std::atomic<bool> s_busy{false};

struct Capture {
    std::string job;
    uint32_t w = 0;
    uint32_t h = 0;
    std::string rle;
};

struct SendResult {
    bool ok = false;
    std::string err;
};

void send_done(SendResult &r) {
    s_busy.store(false);
    if (r.ok) return;
    core::notify::Notification n;
    n.icon = LV_SYMBOL_WARNING;
    n.title = "Screen capture";
    n.body = r.err;
    n.severity = core::notify::Severity::Error;
    core::notify::post(std::move(n));
}

core::OneShotJob<SendResult> s_send_job(send_done);

inline uint16_t blend565(uint16_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return dst;
    const uint32_t dr = ((dst >> 11) & 0x1F) * 255 / 31;
    const uint32_t dg = ((dst >> 5) & 0x3F) * 255 / 63;
    const uint32_t db = (dst & 0x1F) * 255 / 31;
    const uint32_t orr = (r * a + dr * (255 - a)) / 255;
    const uint32_t og = (g * a + dg * (255 - a)) / 255;
    const uint32_t ob = (b * a + db * (255 - a)) / 255;
    return static_cast<uint16_t>(((orr >> 3) << 11) | ((og >> 2) << 5) | (ob >> 3));
}

/** LVGL thread: render screen (+ top layer) and RLE-encode it. */
bool snapshot(Capture &c, std::string &err) {
    lv_obj_t *scr = lv_screen_active();
    lv_draw_buf_t *base = scr ? lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565) : nullptr;
    if (!base) {
        err = "snapshot failed (out of memory?)";
        return false;
    }
    c.w = base->header.w;
    c.h = base->header.h;

    lv_obj_t *top = lv_layer_top();
    lv_draw_buf_t *over = nullptr;
    if (top && lv_obj_get_child_count(top) > 0) over = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);

    std::vector<uint16_t> row(c.w);
    std::vector<uint16_t> px;
    px.reserve(static_cast<size_t>(c.w) * c.h);
    for (uint32_t y = 0; y < c.h; y++) {
        const uint16_t *src = reinterpret_cast<const uint16_t *>(base->data + y * base->header.stride);
        for (uint32_t x = 0; x < c.w; x++) row[x] = src[x];
        if (over && y < over->header.h) {
            const uint8_t *o = over->data + y * over->header.stride;
            const uint32_t ow = over->header.w < c.w ? over->header.w : c.w;
            for (uint32_t x = 0; x < ow; x++) {
                // lv_color32_t byte order: blue, green, red, alpha.
                row[x] = blend565(row[x], o[4 * x + 2], o[4 * x + 1], o[4 * x], o[4 * x + 3]);
            }
        }
        px.insert(px.end(), row.begin(), row.end());
    }
    lv_draw_buf_destroy(base);
    if (over) lv_draw_buf_destroy(over);

    hal::rle565_encode(px.data(), px.size(), c.rle);
    return true;
}

/** Worker: announce with screen.data, then stream the runs. */
SendResult *send(const Capture &c) {
    SendResult *r = new SendResult();
    const uint32_t crc = pagerlink::crc32_update(0, reinterpret_cast<const uint8_t *>(c.rle.data()),
                                                 c.rle.size());
    pagerlink::JsonWriter w;
    w.begin_object()
        .key("job").str_val(c.job)
        .key("w").uint_val(c.w)
        .key("h").uint_val(c.h)
        .key("fmt").str_val("rgb565-rle")
        .key("size").uint_val(c.rle.size())
        .key("crc32").uint_val(crc)
        .end_object();
    const HwLinkResult res = hw_link_call("screen.data", w.str());
    if (!res.ok) {
        hal_error_from_link(res, r->err);
        return r;
    }
    pagerlink::JsonObject o;
    int64_t xfer = 0;
    if (!o.parse(res.json) || !o.get_int("xfer", xfer) || xfer <= 0 || xfer > 0xFFFF) {
        r->err = "phone: bad screen.data reply";
        return r;
    }
    r->ok = hw_link_send_bytes(static_cast<uint16_t>(xfer),
                               reinterpret_cast<const uint8_t *>(c.rle.data()), c.rle.size());
    if (!r->ok) r->err = hw_link_connected() ? "upload failed" : "phone disconnected";
    return r;
}

}  // namespace

bool ui_screen_capture_start(const std::string &job) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return false;
    const bool posted = core::ui_post([job]() {
        Capture *c = new Capture();
        c->job = job;
        std::string err;
        if (!snapshot(*c, err)) {
            delete c;
            SendResult r;
            r.err = err;
            send_done(r);
            return;
        }
        auto st = s_send_job.start(
            [c]() -> SendResult * {
                SendResult *r = send(*c);
                delete c;
                return r;
            },
            6144, "screen_send", 2);
        if (st != core::OneShotJob<SendResult>::Start::Started) {
            delete c;
            SendResult r;
            r.err = "no worker for the upload";
            send_done(r);
        }
    });
    if (!posted) s_busy.store(false);
    return posted;
}

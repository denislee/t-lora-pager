/**
 * @file      emu_shots.cpp
 * @brief     Emulator-only screenshot sweep (see emu_shots.h). Drives the
 *            same AppManager / System entry points the menu tiles and the
 *            back button use, pumps LVGL on a fake clock, and dumps the
 *            display's direct-mode framebuffer as PPM.
 */
#ifndef ARDUINO

#include "emu_shots.h"
#include "ui_define.h"
#include "core/app_manager.h"
#include "core/system.h"
#include "core/one_shot_job.h"
#include "apps/kitchen.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <SDL2/SDL.h>

namespace {

// OneShotJob workers run on threads on the emulator too (DC2.4.12): wait for
// them in real time before each System::loop() drain, so their results land
// on the same fake-clock frame every run and the shots stay deterministic.
void wait_for_jobs()
{
    for (int i = 0; i < 5000 && core::one_shot_jobs_in_flight().load() > 0; i++) {
        usleep(1000);
    }
}

void pump(int ms)
{
    for (int t = 0; t < ms; t += 16) {
        lv_tick_inc(16);
        lv_timer_handler();
        wait_for_jobs();
        core::System::getInstance().loop();
    }
}

std::string slug(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// Dump the full-frame direct-mode buffer as binary PPM.
bool write_ppm(lv_display_t *disp, const std::string &path)
{
    lv_refr_now(disp);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!buf || !buf->data) return false;
    uint32_t w = lv_display_get_horizontal_resolution(disp);
    uint32_t h = lv_display_get_vertical_resolution(disp);
    uint32_t stride = buf->header.stride;
    lv_color_format_t cf = (lv_color_format_t)buf->header.cf;

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    std::vector<uint8_t> row(w * 3);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = buf->data + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (cf == LV_COLOR_FORMAT_RGB565) {
                uint16_t px = ((const uint16_t *)src)[x];
                r = (uint8_t)(((px >> 11) & 0x1f) * 255 / 31);
                g = (uint8_t)(((px >> 5) & 0x3f) * 255 / 63);
                b = (uint8_t)((px & 0x1f) * 255 / 31);
            } else if (cf == LV_COLOR_FORMAT_RGB888) {
                b = src[x * 3]; g = src[x * 3 + 1]; r = src[x * 3 + 2];
            } else {   // XRGB8888 / ARGB8888
                b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2];
            }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

lv_obj_t *find_menu(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_menu_class)) return obj;
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *m = find_menu(lv_obj_get_child(obj, i));
        if (m) return m;
    }
    return NULL;
}

struct Sweep {
    lv_display_t *disp;
    std::string dir;
    int seq = 0;
    int failures = 0;

    void shot(const std::string &name)
    {
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%02d-", seq++);
        std::string path = dir + "/" + prefix + slug(name) + ".ppm";
        if (!write_ppm(disp, path)) {
            fprintf(stderr, "[shots] FAILED %s\n", path.c_str());
            failures++;
        }
    }

    // Every lv_menu page that is not the root, loaded the way a tile click
    // loads it (lv_menu_set_page fires the VALUE_CHANGED the lazy builders
    // hang off), captured, then popped back to the root.
    void menu_pages(const std::string &app, lv_obj_t *menu)
    {
        lv_obj_t *root = lv_menu_get_cur_main_page(menu);
        lv_obj_t *storage = ((lv_menu_t *)menu)->storage;
        std::vector<lv_obj_t *> pages;
        for (uint32_t i = 0; i < lv_obj_get_child_count(storage); i++) {
            lv_obj_t *p = lv_obj_get_child(storage, i);
            if (p != root && lv_obj_check_type(p, &lv_menu_page_class)) pages.push_back(p);
        }
        for (size_t i = 0; i < pages.size(); i++) {
            lv_menu_set_page(menu, pages[i]);
            pump(300);
            char sub[16];
            snprintf(sub, sizeof(sub), "%s-page%02u", app.c_str(), (unsigned)i);
            shot(sub);
            lv_menu_set_page(menu, root);
            pump(100);
        }
    }
};

const char *const kStyleNames[] = {"classic", "pixel", "hotel", "beos"};

} // namespace

int emu_shots_run(lv_display_t *disp, const char *dir)
{
    auto &sys = core::System::getInstance();
    auto &am = core::AppManager::getInstance();

    // Snapshot the names first: the list is stable but switching apps runs
    // arbitrary code.
    std::vector<std::string> names;
    for (const auto &app : am.apps()) {
        if (app->getName() != "MainMenu") names.push_back(app->getName());
    }

    int failures = 0;
    for (int style = 0; style < 4; style++) {
        for (int dark = 0; dark < 2; dark++) {
            std::string combo = std::string(kStyleNames[style]) + (dark ? "-dark" : "-light");
            // UI_SHOTS_ONLY=<style>-<light|dark>[,...] narrows the sweep.
            const char *only = getenv("UI_SHOTS_ONLY");
            if (only && std::string(",") .append(only).append(",").find("," + combo + ",") == std::string::npos) {
                continue;
            }
            ui_style_set((ui_style_t)style);
            ui_dark_mode_set(dark != 0);
            sys.applyChromeStyle();

            Sweep sw;
            sw.disp = disp;
            sw.dir = std::string(dir) + "/" + combo;
            mkdir(dir, 0755);
            mkdir(sw.dir.c_str(), 0755);

            // UI_SHOTS_APPS=<App>[,...] narrows the sweep to those apps.
            const char *apps = getenv("UI_SHOTS_APPS");
            for (const auto &name : names) {
                if (apps && std::string(",").append(apps).append(",").find("," + name + ",") == std::string::npos) {
                    continue;
                }
                sys.enterApp(name.c_str());
                pump(400);
                sw.shot(name);
                if (lv_obj_t *menu = find_menu(sys.getAppPanel())) {
                    sw.menu_pages(name, menu);
                }
                if (name == "Kitchen") {   // seeded game states (KITCHEN_FPS_PLAN.md KF.2)
                    for (int i = 0; i < kitchen::shot_count(); i++) {
                        const char *st = kitchen::shot_setup(i);
                        if (!st) continue;
                        pump(100);
                        sw.shot(name + "-" + st);
                        if (getenv("KITCHEN_CAST_CHECK")) kitchen::cast_check();
                    }
                    kitchen::shot_release();
                }
            }
            sys.showMenu();
            pump(400);
            sw.shot("MainMenu");

            // The shared modals, over the home screen.
            static const char *btns[] = {"Cancel", "OK", ""};
            lv_obj_t *mb = create_msgbox(lv_screen_active(), "Delete note",
                                         "Remove this note from the device?",
                                         btns, NULL, NULL);
            pump(300);
            sw.shot("msgbox");
            destroy_msgbox(mb);
            ui_loading_t ld;
            ui_loading_open(&ld, "Syncing", "Uploading notes...");
            ui_loading_set_progress(&ld, 3, 8, "journal.txt");
            pump(300);
            sw.shot("loading");
            ui_loading_close(&ld);
            pump(100);
            failures += sw.failures;
            printf("[shots] %s: %d pages\n", sw.dir.c_str(), sw.seq);
        }
    }
    return failures;
}

namespace {
// Copy of the frame as it is on screen now.
std::vector<uint8_t> grab(lv_display_t *disp)
{
    lv_refr_now(disp);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!buf || !buf->data) return {};
    uint32_t h = lv_display_get_vertical_resolution(disp);
    return std::vector<uint8_t>(buf->data, buf->data + (size_t)h * buf->header.stride);
}
} // namespace

int emu_kitchen_bench(lv_display_t *disp)
{
    auto &sys = core::System::getInstance();
    sys.enterApp("Kitchen");
    pump(200);
    kitchen::bench_start();
    // Trail check (KF.6 / KF.8): every so often, the frame left by the
    // game's own invalidation must match a full redraw of the same state.
    // LVGL's tick follows SDL's clock here, so this runs in real time.
    int frames = 0, checks = 0, bad = 0, faint = 0;
    uint32_t t0 = SDL_GetTicks();
    uint32_t last_check = SDL_GetTicks();
    while (!kitchen::bench_done() && SDL_GetTicks() - t0 < 240000) {
        // KF.25a: run LVGL when its next timer is due rather than on a fixed sleep, so the bench holds the refresh period instead of landing at 36-48 ms.
        lv_tick_inc(16);
        uint32_t next = lv_timer_handler();
        wait_for_jobs();
        core::System::getInstance().loop();
        usleep((next > 16 ? 16 : (next < 1 ? 1 : next)) * 1000);
        frames++;
        if (SDL_GetTicks() - last_check < 130) continue;
        last_check = SDL_GetTicks();
        std::vector<uint8_t> live = grab(disp);
        kitchen::probe_mute(true);
        kitchen::debug_invalidate_all();
        std::vector<uint8_t> full = grab(disp);
        kitchen::probe_mute(false);
        checks++;
        if (live.size() != full.size()) {
            bad++;
            continue;
        }
        uint32_t stride = lv_display_get_buf_active(disp)->header.stride;
        // A trail is a part's colour left behind. Up to 2 LSBs per 565 channel
        // isn't one: a few antialiased pixels on a dirty rect's edge come out
        // that far off (emulator's LVGL 9.2, cause not found, can't be seen).
        const uint16_t *lp = (const uint16_t *)live.data(), *fp = (const uint16_t *)full.data();
        size_t diff = 0, first = 0, px_n = live.size() / 2, slight = 0;
        for (size_t i = 0; i < px_n; i++) {
            if (lp[i] == fp[i]) continue;
            int dr = abs((lp[i] >> 11) - (fp[i] >> 11)), dg = abs(((lp[i] >> 5) & 63) - ((fp[i] >> 5) & 63));
            int db = abs((lp[i] & 31) - (fp[i] & 31));
            if (dr <= 2 && dg <= 2 && db <= 2) {
                slight++;
            } else if (!diff++) {
                first = i;
            }
        }
        if (slight) faint++;
        if (diff) {
            bad++;
            printf("[kitchen] trail: frame %d, %zu px differ, first at (%zu, %zu)\n", frames, diff,
                   first % (stride / 2), first / (stride / 2));
        }
    }
    printf("[kitchen] trail check: %d of %d frames differ from a full redraw (%d more within 2 LSBs)\n", bad, checks, faint);
    if (!kitchen::bench_done()) {
        printf("[kitchen] bench: did not finish\n");
        return 1;
    }
    return bad ? 2 : 0;
}

#endif

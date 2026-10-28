/**
 * @file      settings_catalog.h
 * @brief     The settings catalog (IOS_SETTINGS_PLAN.md §3): every id, its
 *            type, range, group and board gate, with no store attached.
 *
 * Pure C++ like settings_registry.h, so the real catalog is unit-tested
 * (ranges, gating, schema paging). settings_table.cpp builds a BoardProfile
 * from the HAL, calls build_catalog() and binds each id to its store.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "settings_registry.h"

namespace settings {

/** What the connected board has; decides which ids exist and their ranges. */
struct BoardProfile {
    int64_t min_brightness = 0;
    int64_t max_brightness = 255;
    bool keyboard = false; /* display.kb_backlight */
    bool otg = false;      /* power.otg */
    bool ssh = false;      /* ssh.* (pager) */
    bool gps = false;      /* action time.sync_gps */
    bool nfc = false;      /* conn.nfc */
    /** power.charge_current_ma choices (mA), in the Settings slider's order. */
    std::vector<uint16_t> charge_ma;
    /** time.tz choices (the bundled TZ table). */
    std::vector<std::string> tz_names;
    /** home.visible.<app> rows: {appName, label}, in tile order. */
    std::vector<std::pair<std::string, std::string>> home_apps;
};

/** The font contexts, in Settings order: wire name and title. */
struct FontContext {
    const char *ctx;
    const char *title;
};
extern const FontContext kFontContexts[];
extern const size_t kFontContextCount;
/** Font face wire values, indexed like the blob's *_font_index. */
extern const char *const kFontFaces[];
extern const size_t kFontFaceCount;

/** Add every group, setting and action for `b` (no getters/setters bound). */
void build_catalog(Registry &r, const BoardProfile &b);

/** "http://..." / "https://..." with a host, or "" (clears). */
bool url_ok(const std::string &u);

}  // namespace settings

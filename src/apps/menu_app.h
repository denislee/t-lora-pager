/**
 * @file      menu_app.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "../core/app.h"

namespace apps {

class MenuApp : public core::App {
public:
    MenuApp();
    void onStart(lv_obj_t* parent) override;
    void onStop() override;
    bool persistentUi() const override { return true; }
};

// Home-screen tile registry, exposed so Settings » Home Apps can list and
// toggle each entry. Indices are stable for the lifetime of the build (the
// menu defines them inline) — Settings reads/writes by index here.
int          home_apps_count();
const char  *home_apps_label(int idx);
bool         home_apps_is_visible(int idx);   // default true when no NVS slot
void         home_apps_set_visible(int idx, bool on);

} // namespace apps

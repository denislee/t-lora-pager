/**
 * @file      app_manager.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "app_manager.h"
#include "input_profile.h"
#include "../hal/types.h"  // log_d/i/e macros (Arduino on hardware, printf on emulator)

namespace core {

AppManager& AppManager::getInstance() {
    static AppManager instance;
    return instance;
}

void AppManager::registerApp(std::shared_ptr<App> app) {
    if (app) {
        _appMap[app->getName()] = app;
        _apps.push_back(app);
        log_d("App registered: %s", app->getName().c_str());
    }
}

void AppManager::switchApp(const std::string& name, lv_obj_t* parent) {
    if (!parent) {
        log_e("Cannot switch to app '%s': parent object is null", name.c_str());
        return;
    }

    auto it = _appMap.find(name);
    if (it != _appMap.end()) {
        if (_activeApp == it->second) {
            log_d("App '%s' is already active, ignoring switch", name.c_str());
            return;
        }

        INPUT_PROF_SPAN("switchApp");
        if (_activeApp) {
            log_d("Stopping active app: %s", _activeApp->getName().c_str());
            INPUT_PROF_SPAN("switchApp.onStop");
            _activeApp->onStop();
        }

        _activeApp = it->second;
        log_i("Switching to app: %s", _activeApp->getName().c_str());
        
        // Clean the parent for the new app unless it keeps its UI (home, IS.3).
        // Non-persistent apps get a fresh full-size root (VB.1): the panel is
        // shared, so styles/flags/event callbacks an app puts on its parent
        // would otherwise leak into every app opened after it.
        lv_obj_t* root = parent;
        if (!_activeApp->persistentUi()) {
            INPUT_PROF_SPAN("switchApp.clean");
            lv_obj_clean(parent);
            root = createAppRoot(parent);
        }
        _activeApp->setRoot(root);
        {
            INPUT_PROF_SPAN("switchApp.onStart");
            _activeApp->onStart(root);
        }
        INPUT_PROF(prof::dump_group(name.c_str()));
    } else {
        log_e("App not found: %s", name.c_str());
    }
}

lv_obj_t* AppManager::createAppRoot(lv_obj_t* parent) {
    // Baseline every app starts from: a 4 px inset (what most apps used to
    // inherit by accident from a previously opened Chat/Remote), borderless,
    // transparent (the panel underneath paints the themed background),
    // scrollable like the bare tile it replaces. Apps may restyle it freely.
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_shadow_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 4, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_update_layout(root);  // apps read lv_obj_get_width(parent) in onStart
    return root;
}

void AppManager::queueSwitchApp(const std::string& name, lv_obj_t* parent) {
    _hasPendingSwitch = true;
    _pendingName = name;
    _pendingParent = parent;
}

void AppManager::update() {
    if (_hasPendingSwitch) {
        _hasPendingSwitch = false;
        std::string name = std::move(_pendingName);
        lv_obj_t* parent = _pendingParent;
        _pendingParent = nullptr;
        switchApp(name, parent);
    }
}

} // namespace core

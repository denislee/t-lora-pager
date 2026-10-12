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
        
        // Ensure root is cleaned and set to the new app
        {
            INPUT_PROF_SPAN("switchApp.clean");
            lv_obj_clean(parent);
        }
        _activeApp->setRoot(parent);
        {
            INPUT_PROF_SPAN("switchApp.onStart");
            _activeApp->onStart(parent);
        }
        INPUT_PROF(prof::dump_group(name.c_str()));
    } else {
        log_e("App not found: %s", name.c_str());
    }
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

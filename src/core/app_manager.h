/**
 * @file      app_manager.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "app.h"
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace core {

/**
 * @brief Manages the lifecycle of apps and switching between them.
 */
class AppManager {
public:
    static AppManager& getInstance();

    /**
     * @brief Register a new app.
     * @param app The app to register.
     */
    void registerApp(std::shared_ptr<App> app);

    /**
     * @brief Switch to an app by its name.
     * @param name The name of the app.
     * @param parent The parent object to create the UI on.
     */
    void switchApp(const std::string& name, lv_obj_t* parent);

    /**
     * @brief Queue an app switch to run after the current LVGL event dispatch
     * unwinds. Safe to call from inside LV_EVENT_CLICKED and other handlers
     * whose target is a descendant of the UI the destination app will tear
     * down. The switch runs from update(), called once per main-loop tick.
     */
    void queueSwitchApp(const std::string& name, lv_obj_t* parent);

    // True while a switch queued by queueSwitchApp() has not run yet.
    bool hasPendingSwitch() const { return _hasPendingSwitch; }

    /**
     * @brief Run any switch queued by queueSwitchApp(). Called once per
     *        main-loop tick.
     */
    void update();

    /**
     * @brief Every registered app, in registration order.
     */
    const std::vector<std::shared_ptr<App>>& apps() const { return _apps; }

private:
    AppManager() = default;
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    std::map<std::string, std::shared_ptr<App>> _appMap;
    std::vector<std::shared_ptr<App>> _apps;
    std::shared_ptr<App> _activeApp;

    bool _hasPendingSwitch = false;
    std::string _pendingName;
    lv_obj_t* _pendingParent = nullptr;
};

} // namespace core

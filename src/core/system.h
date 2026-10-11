/**
 * @file      system.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "app_manager.h"
#include <lvgl.h>

namespace core {

/**
 * @brief Singleton class that orchestrates the entire system.
 */
class System {
public:
    static System& getInstance();

    void init();
    void loop();

    lv_obj_t* getAppPanel() { return _appPanel; }

    void showMenu();
    void hideMenu();

    lv_obj_t* showBackButton(lv_event_cb_t cb);
    void hideBackButton();
    void applyChromeStyle();
    lv_obj_t* getBackButton() const { return _statBackBtn; }
    lv_event_cb_t getBackButtonCb() const { return _backBtnCb; }

private:
    System() = default;

    lv_obj_t* _mainScreen = nullptr;
    lv_obj_t* _menuPanel = nullptr;
    lv_obj_t* _appPanel = nullptr;
    lv_obj_t* _statusBar = nullptr;

    lv_obj_t* _statTimeLabel = nullptr;
    lv_obj_t* _statBattLabel = nullptr;
    lv_obj_t* _statMemLabel = nullptr;
    lv_obj_t* _statSDLabel = nullptr;
    lv_obj_t* _statUSBLabel = nullptr;
    lv_obj_t* _statBTLabel = nullptr;
    lv_obj_t* _statWifiLabel = nullptr;
    lv_obj_t* _statHubLabel = nullptr;
    lv_obj_t* _statFileCountLabel = nullptr;
    lv_obj_t* _statRightCont = nullptr;
    lv_obj_t* _statBackBtn = nullptr;
    lv_event_cb_t _backBtnCb = nullptr;
    lv_obj_t* _usbWarningBox = nullptr;

    lv_group_t* _menuGroup = nullptr;
    
    void setupGlobalUI();
};

} // namespace core

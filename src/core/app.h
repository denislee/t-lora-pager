/**
 * @file      app.h
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <lvgl.h>
#include <string>

namespace core {

/**
 * @brief Abstract base class for all applications/screens.
 */
class App {
public:
    App(const std::string& name) : _name(name), _root(nullptr) {}
    virtual ~App() = default;

    /**
     * @brief Called when the app is created and should set up its UI.
     * @param parent The parent object to create the UI on.
     */
    virtual void onStart(lv_obj_t* parent) = 0;

    /**
     * @brief Called when the app is being closed to clean up resources.
     */
    virtual void onStop() {
        if (_root) {
            lv_obj_clean(_root);
            _root = nullptr;
        }
    }

    /**
     * @brief Get the name of the app.
     */
    const std::string& getName() const { return _name; }

    /**
     * @brief Set the root object of the app.
     */
    void setRoot(lv_obj_t* root) { _root = root; }

    /**
     * @brief Get the root object of the app.
     */
    lv_obj_t* getRoot() const { return _root; }

protected:
    std::string _name;
    lv_obj_t* _root;
};

} // namespace core

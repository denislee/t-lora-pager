/**
 * @file      input_focus.cpp
 */
#include "input_focus.h"

namespace core {

lv_obj_t *focusedTextarea()
{
    lv_indev_t *indev = nullptr;
    while ((indev = lv_indev_get_next(indev)) != nullptr) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) continue;
        lv_group_t *g = lv_indev_get_group(indev);
        if (!g) continue;
        lv_obj_t *focused = lv_group_get_focused(g);
        if (focused && lv_obj_has_class(focused, &lv_textarea_class)) {
            return focused;
        }
    }
    return nullptr;
}

bool isTextInputFocused()
{
    return focusedTextarea() != nullptr;
}

} // namespace core


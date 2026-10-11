/**
 * @file      ui/fonts.h
 * @brief     Per-context font getters.
 *
 * Each app/UI context calls its own getter — the user-configured face/size
 * pair from settings is applied centrally so this header stays declaration-
 * only. Implementations live in src/ui_tools.cpp.
 */
#pragma once

#include <lvgl.h>

const lv_font_t *get_editor_font();
const lv_font_t *get_small_font();
const lv_font_t *get_journal_font();
const lv_font_t *get_header_font();
const lv_font_t *get_home_font();
const lv_font_t *get_system_font();
const lv_font_t *get_weather_font();
const lv_font_t *get_telegram_font();
const lv_font_t *get_telegram_list_font();
const lv_font_t *get_ssh_font();
const lv_font_t *get_chat_font();

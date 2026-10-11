/**
 * @file      ui_msg.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

static lv_obj_t *msgbox = NULL;

static void msgbox_event(lv_event_t *)
{
    printf("msgbox event destroy\n");
    destroy_msgbox(msgbox);
    msgbox = NULL;
}

void ui_msg_pop_up(const char *title_txt, const char *msg_txt)
{
    static const char *btns[] = {"Close", ""};
    if (msgbox) {
        printf("msg box has create!\n");
        return;
    }
    printf("create_msgbox..\n");
    msgbox = create_msgbox(lv_scr_act(), title_txt,
                           msg_txt, btns,
                           msgbox_event, NULL);
}

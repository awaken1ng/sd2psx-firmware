#pragma once

#include "lvgl.h"

void input_init(void);
void input_task(void);
void input_update_display(lv_obj_t *line);
int input_get_pressed(void);
int input_is_down_raw(int idx);
void input_flush(void);
int input_is_any_down(void);

enum {
    INPUT_KEY_NEXT = 50,
    INPUT_KEY_PREV = 51,
    INPUT_KEY_ENTER = 52,
    INPUT_KEY_BACK = 53,
    INPUT_KEY_MENU = 54,
};

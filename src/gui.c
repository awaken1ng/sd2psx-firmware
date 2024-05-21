#include "gui.h"

#include <game_names/game_names.h>
#include <ps2/history_tracker/ps2_history_tracker.h>
#include <stdio.h>

#include "config.h"
#include "input.h"
#include "keystore.h"
#include "oled.h"
#include "ps1/ps1_cardman.h"
#include "ps1/ps1_memory_card.h"
#include "ps2/card_emu/ps2_memory_card.h"
#include "ps2/ps2_cardman.h"
#include "ps2/ps2_dirty.h"
#include "settings.h"
#include "ui_menu.h"
#include "ui_theme_mono.h"
#include "version/version.h"

/* Displays the line at the bottom for long pressing buttons */
static lv_obj_t *g_navbar, *g_progress_bar, *g_progress_text, *g_activity_frame;

static lv_obj_t *scr_switch_nag, *scr_card_switch, *scr_main, *scr_menu, *scr_freepsxboot, *menu, *main_page;
static lv_style_t style_inv;
static lv_obj_t *scr_main_idx_lbl, *scr_main_channel_lbl, *src_main_title_lbl, *lbl_civ_err, *lbl_autoboot, *lbl_channel, *auto_off_lbl, *contrast_lbl, *vcomh_lbl;

static struct {
    uint8_t value;
    lv_obj_t *selection_lbl;
} auto_off_options[6];

static struct {
    uint8_t value;
    uint8_t label_value;
    lv_obj_t *selection_lbl;
} contrast_options[10];

static struct {
    uint8_t value;
    char label_text[4];
    char selection_text[16];
    lv_obj_t *selection_lbl;
} vcomh_options[3];

static int have_oled;
static int switching_card;
static uint64_t switching_card_timeout;
static int terminated;
static bool refresh_gui;
static bool installing_exploit;

#define COLOR_FG lv_color_white()
#define COLOR_BG lv_color_black()

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static lv_obj_t *ui_scr_create(void) {
    lv_obj_t *obj = lv_obj_create(NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_group_add_obj(lv_group_get_default(), obj);
    return obj;
}

/* create a navigatable UI menu container, so that the item (label) inside can be selected and clicked */
static lv_obj_t *ui_menu_cont_create_nav(lv_obj_t *parent) {
    lv_obj_t *cont = ui_menu_cont_create(parent);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_group_add_obj(lv_group_get_default(), cont);
    return cont;
}

static lv_obj_t *ui_menu_subpage_create(lv_obj_t *menu, const char *title) {
    lv_obj_t *page = ui_menu_page_create(menu, title);
    lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE); // handled ourselves in `evt_menu_page`
    lv_group_add_obj(lv_group_get_default(), page);
    lv_obj_add_event_cb(page, evt_menu_page, LV_EVENT_ALL, page);
    return page;
}

static lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *ui_label_create_at(lv_obj_t *parent, int x, int y, const char *text) {
    lv_obj_t *label = ui_label_create(parent, text);
    lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *ui_label_create_grow(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = ui_label_create(parent, text);
    lv_obj_set_flex_grow(label, 1);
    return label;
}

static void scrollable_label(lv_event_t *event) {
    if (event->code == LV_EVENT_FOCUSED)
        lv_label_set_long_mode(event->user_data, LV_LABEL_LONG_SCROLL);
    else
        lv_label_set_long_mode(event->user_data, LV_LABEL_LONG_CLIP);
}

static void ui_make_scrollable(lv_obj_t *cont, lv_obj_t *label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_add_event_cb(cont, scrollable_label, LV_EVENT_FOCUSED, label);
    lv_obj_add_event_cb(cont, scrollable_label, LV_EVENT_DEFOCUSED, label);
}

static lv_obj_t *ui_label_create_grow_scroll(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = ui_label_create_grow(parent, text);
    ui_make_scrollable(parent, label);
    return label;
}

static lv_obj_t *ui_header_create(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_align(lbl, LV_ALIGN_TOP_MID);
    lv_obj_add_style(lbl, &style_inv, 0);
    lv_obj_set_width(lbl, DISPLAY_WIDTH);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

static void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    if (have_oled) {
        oled_clear();

        for (int y = area->y1; y <= area->y2; y++) {
            for (int x = area->x1; x <= area->x2; x++) {
                if (color_p->full)
                    oled_draw_pixel(x, y);
                color_p++;
            }
        }

        oled_show();
    }
    lv_disp_flush_ready(disp_drv);
}

static void keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int pressed;

    if (!oled_is_powered_on())
        return;

    data->state = LV_INDEV_STATE_RELEASED;

    pressed = input_get_pressed();
    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = pressed;
    }
}

static void create_nav(void) {
    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, 1);
    lv_style_set_line_color(&style_line, lv_palette_main(LV_PALETTE_BLUE));

    g_navbar = lv_line_create(lv_layer_top());
    lv_obj_add_style(g_navbar, &style_line, 0);

    static lv_style_t style_frame;
    lv_style_init(&style_frame);
    lv_style_set_line_width(&style_frame, 20);
    lv_style_set_line_color(&style_frame, lv_palette_main(LV_PALETTE_BLUE));

    g_activity_frame = lv_line_create(lv_layer_top());
    lv_obj_add_style(g_activity_frame, &style_frame, 0);

    static lv_point_t line_points[5] = {{0, 0}, {DISPLAY_WIDTH, 0}, {DISPLAY_WIDTH, DISPLAY_HEIGHT}, {0, DISPLAY_HEIGHT}, {0, 0}};
    lv_line_set_points(g_activity_frame, line_points, 5);

    lv_obj_add_flag(g_activity_frame, LV_OBJ_FLAG_HIDDEN);
}

static void gui_tick(void) {
    static uint64_t prev_time;
    if (!prev_time)
        prev_time = time_us_64();
    uint64_t now_time = time_us_64();
    uint64_t diff_ms = (now_time - prev_time) / 1000;

    if (diff_ms) {
        prev_time += diff_ms * 1000;
        lv_tick_inc(diff_ms);
        lv_timer_handler();
    }
}

static void reload_card_cb(int progress) {
    static lv_point_t line_points[2] = {{0, DISPLAY_HEIGHT / 2}, {0, DISPLAY_HEIGHT / 2}};
    static int prev_progress;
    progress += 5;
    if (progress / 5 == prev_progress / 5)
        return;
    prev_progress = progress;
    line_points[1].x = DISPLAY_WIDTH * progress / 100;
    lv_line_set_points(g_progress_bar, line_points, 2);

    lv_label_set_text(g_progress_text, ps2_cardman_get_progress_text());

    gui_tick();
}

static void ui_set_display_timeout(uint8_t display_timeout) {
    char text[8];

    if (auto_off_options[0].value == display_timeout) {
        sprintf(text, "Off >"),
        lv_label_set_text(auto_off_lbl, text);

        sprintf(text, "> Off"),
        lv_label_set_text(auto_off_options[0].selection_lbl, text);
    } else {
        sprintf(text, "  Off"),
        lv_label_set_text(auto_off_options[0].selection_lbl, text);
    }

    for (size_t i = 1; i < ARRAY_SIZE(auto_off_options); i++) {
        if (auto_off_options[i].value == display_timeout) {
            sprintf(text, "%hhus >", auto_off_options[i].value),
            lv_label_set_text(auto_off_lbl, text);

            sprintf(text, "> %hhus", auto_off_options[i].value),
            lv_label_set_text(auto_off_options[i].selection_lbl, text);
        } else {
            sprintf(text, "  %hhus", auto_off_options[i].value),
            lv_label_set_text(auto_off_options[i].selection_lbl, text);
        }
    }
}

static void ui_set_display_contrast(uint8_t display_contrast) {
    char text[8];

    for (size_t i = 0; i < ARRAY_SIZE(contrast_options); i++) {
        if (contrast_options[i].value == display_contrast) {
            sprintf(text, "%hhu%% >", contrast_options[i].label_value),
            lv_label_set_text(contrast_lbl, text);

            sprintf(text, "> %hhu%%", contrast_options[i].label_value),
            lv_label_set_text(contrast_options[i].selection_lbl, text);
        } else {
            sprintf(text, "  %hhu%%", contrast_options[i].label_value),
            lv_label_set_text(contrast_options[i].selection_lbl, text);
        }
    }
}

static void ui_set_display_vcomh(uint8_t display_vcomh) {
    char text[16];

    for (size_t i = 0; i < ARRAY_SIZE(vcomh_options); i++) {
        if (vcomh_options[i].value == display_vcomh) {
            sprintf(text, "%s >", vcomh_options[i].label_text),
            lv_label_set_text(vcomh_lbl, text);

            sprintf(text, "> %s", vcomh_options[i].selection_text),
            lv_label_set_text(vcomh_options[i].selection_lbl, text);
        } else {
            sprintf(text, "  %s", vcomh_options[i].selection_text),
            lv_label_set_text(vcomh_options[i].selection_lbl, text);
        }
    }
}

static void evt_scr_main(lv_event_t *event) {
    if (event->code == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        printf("main screen got key %d\n", (int)key);
        if (key == INPUT_KEY_MENU) {
            printf("activate menu!\n");
            lv_scr_load(scr_menu);
            ui_menu_set_page(menu, NULL);
            ui_menu_set_page(menu, main_page);
            lv_obj_t *first = lv_obj_get_child(main_page, 0);
            lv_group_focus_obj(first);
            lv_event_stop_bubbling(event);
        }

        // TODO: if there was a card op recently (1s timeout?), should refuse to switch
        // TODO: ps1 support here
        if (key == INPUT_KEY_PREV || key == INPUT_KEY_NEXT || key == INPUT_KEY_BACK || key == INPUT_KEY_ENTER) {
            uint8_t prevChannel, prevIdx;
            if (settings_get_mode() == MODE_PS1) {
                prevChannel = ps1_cardman_get_channel();
                prevIdx = ps1_cardman_get_idx();

                switch (key) {
                    case INPUT_KEY_PREV: ps1_cardman_prev_channel(); break;
                    case INPUT_KEY_NEXT: ps1_cardman_next_channel(); break;
                    case INPUT_KEY_BACK: ps1_cardman_prev_idx(); break;
                    case INPUT_KEY_ENTER: ps1_cardman_next_idx(); break;
                }
                if ((prevChannel != ps1_cardman_get_channel()) || (prevIdx != ps1_cardman_get_idx())) {
                    ps1_memory_card_exit();
                    ps1_cardman_close();
                    switching_card = 1;
                    printf("new PS1 card=%d chan=%d\n", ps1_cardman_get_idx(), ps1_cardman_get_channel());
                }
            } else {
                ps2_cardman_state_t prevState = ps2_cardman_get_state();
                prevChannel = ps2_cardman_get_channel();
                prevIdx = ps2_cardman_get_idx();

                switch (key) {
                    case INPUT_KEY_PREV: ps2_cardman_prev_channel(); break;
                    case INPUT_KEY_NEXT: ps2_cardman_next_channel(); break;
                    case INPUT_KEY_BACK: ps2_cardman_prev_idx(); break;
                    case INPUT_KEY_ENTER: ps2_cardman_next_idx(); break;
                }

                if ((prevChannel != ps2_cardman_get_channel()) || (prevIdx != ps2_cardman_get_idx()) || (prevState != ps2_cardman_get_state())) {
                    ps2_memory_card_exit();
                    ps2_cardman_close();
                    switching_card = 1;
                    printf("new PS2 card=%d chan=%d\n", ps2_cardman_get_idx(), ps2_cardman_get_channel());
                }
            }

            if (switching_card == 1) {
                switching_card_timeout = time_us_64() + 1500 * 1000;
            }
        }
    }
}

static void evt_scr_freepsxboot(lv_event_t *event) {
    if (event->code == LV_EVENT_KEY) {
        // uint32_t key = lv_indev_get_key(lv_indev_get_act());
        UI_GOTO_SCREEN(scr_main);
        lv_event_stop_bubbling(event);
    }
}

static void evt_scr_menu(lv_event_t *event) {
    if (event->code == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        printf("menu screen got key %d\n", (int)key);
        if (key == INPUT_KEY_BACK || key == INPUT_KEY_MENU) {
            UI_GOTO_SCREEN(scr_main);
            lv_event_stop_bubbling(event);
        }
    }
}

void evt_menu_page(lv_event_t *event) {
    if (event->code == LV_EVENT_KEY) {
        lv_obj_t *page = event->user_data;
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        lv_obj_t *cur = lv_group_get_focused(lv_group_get_default());
        if (lv_obj_get_parent(cur) != page)
            return;
        uint32_t idx = lv_obj_get_index(cur);
        uint32_t count = lv_obj_get_child_cnt(page);
        if (key == INPUT_KEY_NEXT) {
            int next_idx = (idx + 1) % count;
            lv_obj_t *next = ui_menu_find_next_focusable(page, next_idx);
            lv_group_focus_obj(next);
            lv_event_stop_bubbling(event);

            int32_t page_h = lv_obj_get_height(page);
            int32_t obj_h = lv_obj_get_height(next);
            int32_t view_y1 = lv_obj_get_scroll_y(page);
            int32_t view_y2 = view_y1 + page_h;
            int32_t obj_y1 = obj_h * (next_idx);
            int32_t obj_y2 = obj_y1 + obj_h;
            if (obj_y2 > view_y2)
                lv_obj_scroll_to_y(page, obj_y2 - page_h, false); // scroll down
            else if (view_y1 > obj_y1)
                lv_obj_scroll_to_y(page, obj_y1, false); // wrap around
        } else if (key == INPUT_KEY_PREV) {
            int prev_idx = (idx + count - 1) % count;
            lv_obj_t *prev = ui_menu_find_prev_focusable(page, prev_idx);
            lv_group_focus_obj(prev);
            lv_event_stop_bubbling(event);

            int32_t page_h = lv_obj_get_height(page);
            int32_t obj_h = lv_obj_get_height(prev);
            int32_t view_y1 = lv_obj_get_scroll_y(page);
            int32_t view_y2 = view_y1 + page_h;
            int32_t obj_y1 = obj_h * (prev_idx);
            int32_t obj_y2 = obj_y1 + obj_h;
            if (obj_y2 == view_y1)
                lv_obj_scroll_to_y(page, obj_y1, false); // scroll up
            else if (obj_y2 > view_y2)
                lv_obj_scroll_to_y(page, obj_y2 - page_h, false); // wrap around
        } else if (key == INPUT_KEY_ENTER) {
            lv_event_send(cur, LV_EVENT_CLICKED, NULL);
            lv_event_stop_bubbling(event);
        } else if (key == INPUT_KEY_BACK) {
            /* going back from the root page - let it handle in evt_scr_menu */
            if (ui_menu_get_cur_main_page(menu) == main_page)
                return;
            ui_menu_go_back(menu);
            lv_obj_scroll_to_y(page, 0, false); // reset scroll on the way out
            lv_event_stop_bubbling(event);
        }
    }
}

static void evt_go_back(lv_event_t *event) {
    ui_menu_go_back(menu);
    lv_event_stop_bubbling(event);
}

static void evt_ps2_autoboot(lv_event_t *event) {
    bool current = settings_get_ps2_autoboot();
    settings_set_ps2_autoboot(!current);
    lv_label_set_text(lbl_autoboot, !current ? "Yes" : "No");
    lv_event_stop_bubbling(event);
}

static void evt_do_civ_deploy(lv_event_t *event) {
    (void)event;

    int ret = keystore_deploy();
    if (ret == 0) {
        lv_label_set_text(lbl_civ_err, "Success!\n");
    } else {
        lv_label_set_text(lbl_civ_err, keystore_error(ret));
    }
}

static void evt_switch_to_ps1(lv_event_t *event) {
    (void)event;

    settings_set_mode(MODE_PS1);

    UI_GOTO_SCREEN(scr_switch_nag);
    terminated = 1;
}

static void evt_switch_to_ps2(lv_event_t *event) {
    (void)event;

    settings_set_mode(MODE_PS2);

    UI_GOTO_SCREEN(scr_switch_nag);
    terminated = 1;
}

static void evt_set_display_timeout(lv_event_t *event) {
    uint8_t display_timeout = (intptr_t)event->user_data;
    settings_set_display_timeout(display_timeout);
    ui_set_display_timeout(display_timeout);
}

static void evt_set_display_contrast(lv_event_t *event) {
    uint8_t display_contrast = (intptr_t)event->user_data;
    settings_set_display_contrast(display_contrast);
    oled_set_contrast(display_contrast);
    ui_set_display_contrast(display_contrast);
}

static void evt_set_display_vcomh(lv_event_t *event) {
    uint8_t display_vcomh = (intptr_t)event->user_data;
    settings_set_display_vcomh(display_vcomh);
    oled_set_vcomh(display_vcomh);
    ui_set_display_vcomh(display_vcomh);
}

static void create_main_screen(void) {
    lv_obj_t *lbl;

    /* Main screen listing current memcard, status, etc */
    scr_main = ui_scr_create();
    lv_obj_add_event_cb(scr_main, evt_scr_main, LV_EVENT_ALL, NULL);

    if (settings_get_mode() == MODE_PS1) {
        ui_header_create(scr_main, "PS1 Memory Card");
    } else {
        if (!ps2_magicgate)
            ui_header_create(scr_main, "PS2: No CIV!");
        else
            ui_header_create(scr_main, "PS2 Memory Card");
    }

    ui_label_create_at(scr_main, 0, 24, "Card");

    scr_main_idx_lbl = ui_label_create_at(scr_main, 0, 24, "");
    lv_obj_set_align(scr_main_idx_lbl, LV_ALIGN_TOP_RIGHT);

    lbl_channel = ui_label_create_at(scr_main, 0, 32, "Channel");

    scr_main_channel_lbl = ui_label_create_at(scr_main, 0, 32, "");
    lv_obj_set_align(scr_main_channel_lbl, LV_ALIGN_TOP_RIGHT);

    src_main_title_lbl = lv_label_create(scr_main);
    lv_obj_set_align(src_main_title_lbl, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(src_main_title_lbl, 0, 40);
    lv_label_set_text(src_main_title_lbl, "");
    lv_label_set_long_mode(src_main_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(src_main_title_lbl, 128);

    lbl = lv_label_create(scr_main);
    lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(lbl, 0, -2);
    lv_label_set_text(lbl, "<");
    lv_obj_add_style(lbl, &style_inv, 0);

    lbl = lv_label_create(scr_main);
    lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(lbl, 0, -2);
    lv_label_set_text(lbl, "Menu");
    lv_obj_set_width(lbl, 4 * 8 + 2);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(lbl, &style_inv, 0);

    lbl = lv_label_create(scr_main);
    lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(lbl, 0, -2);
    lv_label_set_text(lbl, ">");
    lv_obj_add_style(lbl, &style_inv, 0);
}

static void create_freepsxboot_screen(void) {
    lv_obj_t *lbl;

    scr_freepsxboot = ui_scr_create();
    lv_obj_add_event_cb(scr_freepsxboot, evt_scr_freepsxboot, LV_EVENT_ALL, NULL);

    ui_header_create(scr_freepsxboot, "FreePSXBoot");

    lbl = lv_label_create(scr_freepsxboot);
    lv_obj_set_align(lbl, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(lbl, 0, 24);
    lv_label_set_text(lbl, "Model");

    lbl = lv_label_create(scr_freepsxboot);
    lv_obj_set_align(lbl, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(lbl, 0, 24);
    lv_label_set_text(lbl, "1001v3");

    lbl = lv_label_create(scr_freepsxboot);
    lv_obj_set_align(lbl, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(lbl, 0, 32);
    lv_label_set_text(lbl, "Slot");

    lbl = lv_label_create(scr_freepsxboot);
    lv_obj_set_align(lbl, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(lbl, 0, 32);
    lv_label_set_text(lbl, "Slot 2");

    lbl = lv_label_create(scr_freepsxboot);
    lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
    lv_label_set_text(lbl, "Press any button to deactivate FreePSXBoot and return to the Memory Card mode");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, 128);
}

static void create_cardswitch_screen(void) {
    scr_card_switch = ui_scr_create();

    ui_header_create(scr_card_switch, "Loading card");

    static lv_style_t style_progress;
    lv_style_init(&style_progress);
    lv_style_set_line_width(&style_progress, 12);
    lv_style_set_line_color(&style_progress, lv_palette_main(LV_PALETTE_BLUE));

    g_progress_bar = lv_line_create(scr_card_switch);
    lv_obj_set_width(g_progress_bar, DISPLAY_WIDTH);
    lv_obj_add_style(g_progress_bar, &style_progress, 0);

    g_progress_text = lv_label_create(scr_card_switch);
    lv_obj_set_align(g_progress_text, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(g_progress_text, 0, DISPLAY_HEIGHT - 9);
    lv_label_set_text(g_progress_text, "Read XXX kB/s");
}

static void create_switch_nag_screen(void) {
    scr_switch_nag = ui_scr_create();

    ui_header_create(scr_switch_nag, "Mode switch");

    ui_label_create_at(scr_switch_nag, 0, 24, "Now unplug the");
    ui_label_create_at(scr_switch_nag, 0, 32, "card and then");
    ui_label_create_at(scr_switch_nag, 0, 40, "plug it back in");
}

static void create_menu_screen(void) {
    /* Menu screen accessible by pressing the menu button at main */
    scr_menu = ui_scr_create();
    lv_obj_add_event_cb(scr_menu, evt_scr_menu, LV_EVENT_ALL, NULL);

    menu = ui_menu_create(scr_menu);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(menu, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL) - 2);

    lv_obj_t *cont;

    /* deploy submenu */
    lv_obj_t *mode_page = ui_menu_subpage_create(menu, "Mode");
    {
        lv_obj_t *ps2_switch_warn = ui_menu_subpage_create(menu, NULL);
        {
            cont = ui_menu_cont_create(ps2_switch_warn);
            ui_label_create(cont, "Do not insert");
            cont = ui_menu_cont_create(ps2_switch_warn);
            ui_label_create(cont, "into PS1 when");
            cont = ui_menu_cont_create(ps2_switch_warn);
            ui_label_create(cont, "set to PS2 mode!");
            cont = ui_menu_cont_create(ps2_switch_warn);
            ui_label_create(cont, "It may damage");
            cont = ui_menu_cont_create(ps2_switch_warn);
            ui_label_create(cont, "card and console");

            cont = ui_menu_cont_create_nav(ps2_switch_warn);
            ui_label_create(cont, "Confirm");
            lv_obj_add_event_cb(cont, evt_switch_to_ps2, LV_EVENT_CLICKED, NULL);

            cont = ui_menu_cont_create_nav(ps2_switch_warn);
            ui_label_create(cont, "Back");
            lv_obj_add_event_cb(cont, evt_go_back, LV_EVENT_CLICKED, NULL);
        }

        cont = ui_menu_cont_create_nav(mode_page);
        ui_label_create(cont, "PS1");
        lv_obj_add_event_cb(cont, evt_switch_to_ps1, LV_EVENT_CLICKED, NULL);

        cont = ui_menu_cont_create_nav(mode_page);
        ui_label_create(cont, "PS2");
        ui_menu_set_load_page_event(menu, cont, ps2_switch_warn);
    }

    /* display / auto off submenu */
    lv_obj_t *auto_off_page = ui_menu_subpage_create(menu, "Auto off");
    {
        auto_off_options[0].value = 0;
        auto_off_options[1].value = 5;
        auto_off_options[2].value = 15;
        auto_off_options[3].value = 30;
        auto_off_options[4].value = 60;
        auto_off_options[5].value = 120;

        for (size_t i = 0; i < ARRAY_SIZE(auto_off_options); i++) {
            uint8_t value = auto_off_options[i].value;

            cont = ui_menu_cont_create_nav(auto_off_page);
            auto_off_options[i].selection_lbl = ui_label_create_grow(cont, NULL);
            lv_obj_add_event_cb(cont, evt_set_display_timeout, LV_EVENT_CLICKED, (void*)(intptr_t)value);
        }
    }

    /* display / contrast submenu */
    lv_obj_t *contrast_page = ui_menu_subpage_create(menu, "Contrast");
    {
        char text[8];

        for (size_t i = 0; i < ARRAY_SIZE(contrast_options); i++) {
            uint8_t percentage = (i + 1) * 10;
            uint8_t value = (uint16_t)(255 * percentage) / 100;
            contrast_options[i].value = value;
            contrast_options[i].label_value = percentage;
            sprintf(text, " %hhu%%", percentage);

            cont = ui_menu_cont_create_nav(contrast_page);
            contrast_options[i].selection_lbl = ui_label_create_grow(cont, text);
            lv_obj_add_event_cb(cont, evt_set_display_contrast, LV_EVENT_CLICKED, (void*)(intptr_t)value);
        }
    }

    /* display / vcomh submenu */
    lv_obj_t *vcomh_page = ui_menu_subpage_create(menu, "VCOMH");
    {
        vcomh_options[0].value = 0x00;
        vcomh_options[1].value = 0x20;
        vcomh_options[2].value = 0x30;

        strcpy(vcomh_options[0].label_text, "00h");
        strcpy(vcomh_options[1].label_text, "20h");
        strcpy(vcomh_options[2].label_text, "30h");

        strcpy(vcomh_options[0].selection_text, "0.65 x VCC");
        strcpy(vcomh_options[1].selection_text, "0.77 x VCC");
        strcpy(vcomh_options[2].selection_text, "0.83 x VCC");

        for (size_t i = 0; i < ARRAY_SIZE(vcomh_options); i++) {
            cont = ui_menu_cont_create_nav(vcomh_page);
            vcomh_options[i].selection_lbl = ui_label_create_grow(cont, vcomh_options[i].label_text);
            lv_obj_add_event_cb(cont, evt_set_display_vcomh, LV_EVENT_CLICKED, (void*)(intptr_t)vcomh_options[i].value);
        }
    }

    /* display config */
    lv_obj_t *display_page = ui_menu_subpage_create(menu, "Display");
    {
        cont = ui_menu_cont_create_nav(display_page);
        ui_label_create_grow(cont, "Auto off");
        auto_off_lbl = ui_label_create(cont, NULL);
        ui_menu_set_load_page_event(menu, cont, auto_off_page);
        ui_set_display_timeout(settings_get_display_timeout());

        cont = ui_menu_cont_create_nav(display_page);
        ui_label_create_grow(cont, "Contrast");
        contrast_lbl = ui_label_create(cont, NULL);
        ui_menu_set_load_page_event(menu, cont, contrast_page);
        ui_set_display_contrast(settings_get_display_contrast());

        cont = ui_menu_cont_create_nav(display_page);
        ui_label_create_grow(cont, "VCOMH");
        vcomh_lbl = ui_label_create(cont, NULL);
        ui_menu_set_load_page_event(menu, cont, vcomh_page);
        ui_set_display_vcomh(settings_get_display_vcomh());
    }

    /* ps2 */
    lv_obj_t *ps2_page = ui_menu_subpage_create(menu, "PS2 Settings");
    {
        /* deploy submenu */
        lv_obj_t *civ_page = ui_menu_subpage_create(menu, "Deploy CIV.bin");
        {
            cont = ui_menu_cont_create(civ_page);
            ui_label_create(cont, "");
            cont = ui_menu_cont_create(civ_page);
            lbl_civ_err = ui_label_create(cont, "");

            cont = ui_menu_cont_create_nav(civ_page);
            ui_label_create(cont, "Back");
            lv_obj_add_event_cb(cont, evt_go_back, LV_EVENT_CLICKED, NULL);
        }

        cont = ui_menu_cont_create_nav(ps2_page);
        ui_label_create_grow_scroll(cont, "Autoboot");
        lbl_autoboot = ui_label_create(cont, settings_get_ps2_autoboot() ? " Yes" : " No");
        lv_obj_add_event_cb(cont, evt_ps2_autoboot, LV_EVENT_CLICKED, NULL);

        cont = ui_menu_cont_create_nav(ps2_page);
        ui_label_create_grow(cont, "Deploy CIV.bin");
        ui_label_create(cont, ">");
        ui_menu_set_load_page_event(menu, cont, civ_page);
        lv_obj_add_event_cb(cont, evt_do_civ_deploy, LV_EVENT_CLICKED, NULL);
    }

    /* Info submenu */
    lv_obj_t *info_page = ui_menu_subpage_create(menu, "Info");
    {
        cont = ui_menu_cont_create_nav(info_page);
        ui_label_create_grow_scroll(cont, "Version");
        ui_label_create(cont, sd2psx_version);

        cont = ui_menu_cont_create_nav(info_page);
        ui_label_create_grow_scroll(cont, "Commit");
        ui_label_create(cont, sd2psx_commit);

        cont = ui_menu_cont_create_nav(info_page);
        ui_label_create_grow_scroll(cont, "Debug");
#ifdef DEBUG_USB_UART
        ui_label_create(cont, "Yes");
#else
        ui_label_create(cont, "No");
#endif
    }

    /* Main menu */
    main_page = ui_menu_subpage_create(menu, NULL);
    {
        cont = ui_menu_cont_create_nav(main_page);
        ui_label_create_grow(cont, "Mode");
        ui_label_create(cont, (settings_get_mode() == MODE_PS1) ? "PS1" : "PS2");
        ui_menu_set_load_page_event(menu, cont, mode_page);

        cont = ui_menu_cont_create_nav(main_page);
        ui_label_create_grow(cont, "PS2 Settings");
        ui_label_create(cont, ">");
        ui_menu_set_load_page_event(menu, cont, ps2_page);

        cont = ui_menu_cont_create_nav(main_page);
        ui_label_create_grow(cont, "Display");
        ui_label_create(cont, ">");
        ui_menu_set_load_page_event(menu, cont, display_page);

        cont = ui_menu_cont_create_nav(main_page);
        ui_label_create_grow_scroll(cont, "Info");
        ui_label_create(cont, ">");
        ui_menu_set_load_page_event(menu, cont, info_page);
    }

    ui_menu_set_page(menu, main_page);
}

static void create_ui(void) {
    lv_style_init(&style_inv);
    lv_style_set_bg_opa(&style_inv, LV_OPA_COVER);
    lv_style_set_bg_color(&style_inv, COLOR_FG);
    lv_style_set_border_color(&style_inv, COLOR_BG);
    lv_style_set_line_color(&style_inv, COLOR_BG);
    lv_style_set_arc_color(&style_inv, COLOR_BG);
    lv_style_set_text_color(&style_inv, COLOR_BG);
    lv_style_set_outline_color(&style_inv, COLOR_BG);

    create_nav();
    create_main_screen();
    create_menu_screen();
    create_cardswitch_screen();
    create_switch_nag_screen();
    create_freepsxboot_screen();

    /* start at the main screen - TODO - or freepsxboot */
    // UI_GOTO_SCREEN(scr_freepsxboot);
    UI_GOTO_SCREEN(scr_main);
}

void gui_init(void) {
    if ((have_oled = oled_init())) {
        oled_clear();
        oled_show();
    }

    lv_init();

    static lv_disp_draw_buf_t disp_buf;
    static lv_color_t buf_1[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    lv_disp_draw_buf_init(&disp_buf, buf_1, NULL, DISPLAY_WIDTH * DISPLAY_HEIGHT);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = flush_cb;
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.direct_mode = disp_drv.full_refresh = 1;

    lv_disp_t *disp;
    disp = lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = keypad_read;

    lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
    lv_group_t *g = lv_group_create();
    lv_group_set_default(g);
    lv_indev_set_group(indev, g);

    lv_theme_t *th = ui_theme_mono_init(disp, 1, LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, th);

    create_ui();
    refresh_gui = false;
    installing_exploit = false;
}

void gui_request_refresh(void) {
    refresh_gui = true;
    oled_update_last_action_time();
}

void gui_do_ps1_card_switch(void) {
    printf("switching the card now!\n");

    oled_update_last_action_time();

    uint64_t start = time_us_64();
    ps1_cardman_open();
    ps1_memory_card_enter();
    uint64_t end = time_us_64();
    printf("full card switch took = %.2f s\n", (end - start) / 1e6);
}

void gui_do_ps2_card_switch(void) {
    printf("switching the card now!\n");
    UI_GOTO_SCREEN(scr_card_switch);

    oled_update_last_action_time();

    uint64_t start = time_us_64();
    ps2_cardman_set_progress_cb(reload_card_cb);
    ps2_cardman_open();
    ps2_history_tracker_card_changed();
    ps2_cardman_set_progress_cb(NULL);
    ps2_memory_card_enter();
    uint64_t end = time_us_64();

    printf("full card switch took = %.2f s\n", (end - start) / 1e6);

    UI_GOTO_SCREEN(scr_main);

    input_flush();
}

void gui_task(void) {
    input_update_display(g_navbar);

    char card_name[127];
    const char *folder_name = NULL;

    if (settings_get_mode() == MODE_PS1) {
        static int displayed_card_idx = -1;
        static int displayed_card_channel = -1;
        static char card_idx_s[8];
        static char card_channel_s[8];

        if (displayed_card_idx != ps1_cardman_get_idx() || displayed_card_channel != ps1_cardman_get_channel() || refresh_gui) {
            displayed_card_idx = ps1_cardman_get_idx();
            displayed_card_channel = ps1_cardman_get_channel();
            folder_name = ps1_cardman_get_folder_name();

            snprintf(card_channel_s, sizeof(card_channel_s), "%d", displayed_card_channel);

            if (displayed_card_idx > 0) {
                snprintf(card_idx_s, sizeof(card_idx_s), "%d", displayed_card_idx);
                lv_label_set_text(scr_main_idx_lbl, card_idx_s);
            } else {
                lv_label_set_text(scr_main_idx_lbl, folder_name);
            }

            memset(card_name, 0, sizeof(card_name));
            game_names_get_name_by_folder(folder_name, card_name);

            if (card_name[0]) {
                lv_label_set_text(src_main_title_lbl, card_name);
            } else {
                lv_label_set_text(src_main_title_lbl, "");
            }

            lv_label_set_text(scr_main_channel_lbl, card_channel_s);
        }

        if (switching_card && switching_card_timeout < time_us_64() && !input_is_any_down()) {
            switching_card = 0;
            gui_do_ps1_card_switch();
        }
        refresh_gui = false;
    } else {
        static int displayed_card_idx = -1;
        static int displayed_card_channel = -1;
        static ps2_cardman_state_t cardman_state = PS2_CM_STATE_NORMAL;
        static char card_idx_s[8];
        static char card_channel_s[8];
        if (displayed_card_idx != ps2_cardman_get_idx() || displayed_card_channel != ps2_cardman_get_channel() || cardman_state != ps2_cardman_get_state() ||
            refresh_gui) {
            displayed_card_idx = ps2_cardman_get_idx();
            displayed_card_channel = ps2_cardman_get_channel();
            folder_name = ps2_cardman_get_folder_name();
            cardman_state = ps2_cardman_get_state();

            if (PS2_CM_STATE_BOOT == cardman_state) {
                snprintf(card_idx_s, sizeof(card_idx_s), "BOOT");
                snprintf(card_channel_s, sizeof(card_channel_s), " ");
                lv_label_set_text(lbl_channel, "");
                lv_label_set_text(scr_main_idx_lbl, card_idx_s);
            } else if (PS2_CM_STATE_GAMEID == cardman_state) {
                snprintf(card_channel_s, sizeof(card_channel_s), "%d", displayed_card_channel);
                lv_label_set_text(lbl_channel, "Channel");
                lv_label_set_text(scr_main_idx_lbl, folder_name);
            } else {
                snprintf(card_idx_s, sizeof(card_idx_s), "%d", displayed_card_idx);
                snprintf(card_channel_s, sizeof(card_channel_s), "%d", displayed_card_channel);
                lv_label_set_text(lbl_channel, "Channel");
                lv_label_set_text(scr_main_idx_lbl, card_idx_s);
            }

            memset(card_name, 0, sizeof(card_name));
            game_names_get_name_by_folder(folder_name, card_name);

            if (card_name[0]) {
                lv_label_set_text(src_main_title_lbl, card_name);
            } else {
                lv_label_set_text(src_main_title_lbl, "");
            }

            lv_label_set_text(scr_main_channel_lbl, card_channel_s);

            refresh_gui = false;
        }

        if (switching_card && switching_card_timeout < time_us_64() && !input_is_any_down()) {
            switching_card = 0;
            gui_do_ps2_card_switch();
        }

        if (ps2_dirty_activity) {
            input_flush();
            lv_obj_clear_flag(g_activity_frame, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_activity_frame, LV_OBJ_FLAG_HIDDEN);
        }
    }

    gui_tick();

    /* fatal error, or mode switch between ps1/ps2 - kill all input until replugged */
    if (terminated) {
        while (1) {
            gui_tick();
        }
    }
}

// main/app_todo.c —— 任务 Todo 页。
// 数据由手机 BLE 推送(TODO_CLEAR + TODO_ADD)。上/下移动选中行,OK 单击
// 勾选/取消勾选并向手机回传 TODO_TOGGLE(手机侧"后写者胜")。
#include "app_todo.h"

#include "app_pager.h"
#include "sync_ble.h"
#include "ui_font_cjk_16.h"
#include "ui_pixel.h"
#include "lvgl.h"

#include <stdio.h>

#define TODO_ROW_H    30
#define TODO_MAX_ROWS 6               // 一屏可见行数

typedef struct {
    lv_obj_t *box;        // 勾选框
    lv_obj_t *title;      // 标题
    lv_obj_t *row;        // 行背景
} todo_row_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_head_lbl;   // "完成 X / Y"
static lv_obj_t *s_foot_lbl;   // 连接/提示
static todo_row_t s_rows[TODO_MAX_ROWS];
static int s_sel;              // 选中行(store 下标)
static int s_win;              // 窗口起点(store 下标)

static unsigned done_count(const sync_store_t *st)
{
    unsigned n = 0;
    for (uint16_t i = 0; i < st->todo_count; i++) {
        if (st->todos[i].done) n++;
    }
    return n;
}

// 重建可见 6 行(数据变化/翻窗时调用;页面回调,锁已持有)。
static void rows_rebuild(void)
{
    const sync_store_t *st = app_store();
    int shown = st->todo_count - s_win;
    if (shown > TODO_MAX_ROWS) shown = TODO_MAX_ROWS;
    for (int i = 0; i < TODO_MAX_ROWS; i++) {
        todo_row_t *r = &s_rows[i];
        if (i < shown) {
            const sync_todo_item_t *it = sync_todo_at(st, (uint16_t)(s_win + i));
            lv_obj_set_style_bg_color(r->row,
                lv_color_hex(s_win + i == s_sel ? UI_YELLOW : UI_PAPER), 0);
            lv_obj_set_style_bg_color(r->box,
                lv_color_hex(it->done ? UI_GRASS_DARK : UI_PAPER), 0);
            lv_obj_set_style_border_color(r->box,
                lv_color_hex(it->done ? UI_GRASS_DARK : 0x8A9BA6), 0);
            lv_label_set_text(r->title,
                              it->title_len ? it->title : "(无标题)");
            lv_obj_set_style_text_color(r->title,
                lv_color_hex(it->done ? 0x8A9BA6 : UI_INK), 0);
            lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text_fmt(s_head_lbl,
                          st->todo_count ? "完成 %u / %u" : "暂无任务",
                          (unsigned)done_count(st), (unsigned)st->todo_count);
}

void app_todo_enter(void)
{
    s_scr = ui_pixel_screen_create("TODO");
    s_bat = app_battery_create(s_scr);

    s_head_lbl = ui_pixel_label(s_scr, "", &ui_font_cjk_16, 0x1779B2);
    lv_obj_set_pos(s_head_lbl, 18, 46);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 64, 216, 182, UI_PAPER);
    for (int i = 0; i < TODO_MAX_ROWS; i++) {
        todo_row_t *r = &s_rows[i];
        r->row = lv_obj_create(panel);
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(r->row, 6, 6 + i * TODO_ROW_H);
        lv_obj_set_size(r->row, 204, TODO_ROW_H - 6);
        lv_obj_set_style_radius(r->row, 0, 0);
        lv_obj_set_style_border_width(r->row, 0, 0);
        lv_obj_set_style_bg_color(r->row, lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);

        r->box = lv_obj_create(r->row);
        lv_obj_remove_flag(r->box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(r->box, 4, 4);
        lv_obj_set_size(r->box, 14, 14);
        lv_obj_set_style_radius(r->box, 2, 0);
        lv_obj_set_style_border_width(r->box, 2, 0);
        lv_obj_set_style_border_color(r->box, lv_color_hex(0x8A9BA6), 0);
        lv_obj_set_style_bg_color(r->box, lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_pad_all(r->box, 0, 0);

        r->title = ui_pixel_label(r->row, "", &ui_font_cjk_16, UI_INK);
        lv_obj_set_pos(r->title, 26, 0);
        lv_obj_set_width(r->title, 172);
        lv_obj_set_style_text_align(r->title, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(r->title, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(r->title, 0, 0);
    }
    s_foot_lbl = ui_pixel_label(s_scr, "", &ui_font_cjk_16, 0x1779B2);
    lv_obj_align(s_foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -26);

    s_sel = 0;
    s_win = 0;
    app_todo_refresh();
    lv_screen_load(s_scr);
}

void app_todo_refresh(void)
{
    if (!s_scr) return;
    const sync_store_t *st = app_store();
    if (st->todo_count == 0) {
        s_sel = 0;
        s_win = 0;
    } else {
        if (s_sel >= (int)st->todo_count) s_sel = (int)st->todo_count - 1;
        if (s_sel < s_win) s_win = s_sel;
        if (s_sel >= s_win + TODO_MAX_ROWS) s_win = s_sel - TODO_MAX_ROWS + 1;
    }
    rows_rebuild();
    lv_label_set_text(s_foot_lbl,
                      sync_ble_is_connected() ? "上下 选择 · 确定 勾选"
                                              : "等待手机连接");
}

void app_todo_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    const sync_store_t *st = app_store();
    if (st->todo_count == 0) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int n = (int)st->todo_count;
        s_sel = btn == BSP_BTN_UP ? (s_sel + n - 1) % n : (s_sel + 1) % n;
        if (s_sel < s_win) s_win = s_sel;
        if (s_sel >= s_win + TODO_MAX_ROWS) s_win = s_sel - TODO_MAX_ROWS + 1;
        rows_rebuild();
        return;
    }
    if (btn == BSP_BTN_OK) {
        const sync_todo_item_t *it = sync_todo_at(st, (uint16_t)s_sel);
        if (!it) return;
        uint8_t done = it->done ? 0 : 1;
        if (!sync_todo_set_done(app_store(), it->id, done)) return;
        uint8_t frame[SYNC_FRAME_MAX];
        size_t n = sync_proto_build_todo_toggle(frame, sizeof(frame), it->id, done);
        if (n) sync_ble_send(frame, n);
        rows_rebuild();
    }
}

void app_todo_exit(void)
{
    if (s_scr) {
        app_battery_unregister(s_bat);
        s_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_head_lbl = s_foot_lbl = s_sel_bg = NULL;
    }
}
// main/app_todo.c — DayRing-style task list for three-button navigation.
// Data is synced over BLE. Up/down selects and OK toggles completion.
#include "app_todo.h"

#include "app_pager.h"
#include "sync_ble.h"
#include "ui_font_cjk_16.h"
#include "ui_pixel.h"
#include "lvgl.h"

#define TODO_ROW_H    38
#define TODO_MAX_ROWS 5

typedef struct {
    lv_obj_t *row;
    lv_obj_t *box;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *priority;
} todo_row_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_head_lbl;
static lv_obj_t *s_foot_lbl;
static todo_row_t s_rows[TODO_MAX_ROWS];
static int s_sel;
static int s_win;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     uint32_t color, int border, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(obj, border, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static unsigned done_count(const sync_store_t *st)
{
    unsigned count = 0;
    for (uint16_t i = 0; i < st->todo_count; i++) {
        if (st->todos[i].done) count++;
    }
    return count;
}

static void rows_rebuild(void)
{
    const sync_store_t *st = app_store();
    unsigned done = done_count(st);
    int shown = (int)st->todo_count - s_win;
    if (shown > TODO_MAX_ROWS) shown = TODO_MAX_ROWS;

    lv_label_set_text_fmt(s_head_lbl, "%02u OPEN",
                          (unsigned)st->todo_count - done);

    for (int i = 0; i < TODO_MAX_ROWS; i++) {
        todo_row_t *row = &s_rows[i];
        if (i >= shown) {
            lv_obj_add_flag(row->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const sync_todo_item_t *item = sync_todo_at(st, (uint16_t)(s_win + i));
        bool active = s_win + i == s_sel;
        uint32_t row_color = active ? UI_INK : UI_SURFACE;
        uint32_t main_color = active ? UI_SURFACE : (item->done ? UI_SUBTLE : UI_INK);
        uint32_t meta_color = active ? UI_LINE : UI_SUBTLE;
        uint32_t mark_color = active ? UI_SURFACE : UI_INK;

        lv_obj_set_style_bg_color(row->row, lv_color_hex(row_color), 0);
        lv_obj_set_style_text_color(row->title, lv_color_hex(main_color), 0);
        lv_obj_set_style_text_color(row->meta, lv_color_hex(meta_color), 0);
        lv_obj_set_style_border_color(row->box, lv_color_hex(mark_color), 0);
        lv_obj_set_style_bg_color(row->box,
                                  lv_color_hex(item->done ? mark_color : row_color), 0);
        lv_obj_set_style_border_color(row->priority, lv_color_hex(mark_color), 0);
        lv_obj_set_style_bg_color(row->priority,
                                  lv_color_hex(item->done ? row_color : mark_color), 0);

        lv_label_set_text(row->title, item->title_len ? item->title : "(无标题)");
        lv_label_set_text_fmt(row->meta, "%s  ·  TASK %02d",
                              item->done ? "DONE" : "OPEN", s_win + i + 1);
        lv_obj_remove_flag(row->box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row->priority, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
    }

    if (st->todo_count == 0) {
        todo_row_t *row = &s_rows[0];
        lv_obj_set_style_bg_color(row->row, lv_color_hex(UI_SURFACE), 0);
        lv_obj_set_style_text_color(row->title, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_text_color(row->meta, lv_color_hex(UI_SUBTLE), 0);
        lv_label_set_text(row->title, "暂无任务");
        lv_label_set_text(row->meta, "SYNC FROM PHONE");
        lv_obj_add_flag(row->box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row->priority, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
    }
}

void app_todo_enter(void)
{
    s_scr = ui_pixel_screen_create("TODO");
    s_bat = app_battery_create(s_scr);

    lv_obj_t *today = ui_pixel_label(s_scr, "TODAY", &lv_font_montserrat_14,
                                     UI_SUBTLE);
    lv_obj_set_width(today, 90);
    lv_obj_set_pos(today, 140, 18);
    lv_obj_set_style_text_align(today, LV_TEXT_ALIGN_RIGHT, 0);
    s_head_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_head_lbl, 90);
    lv_obj_set_pos(s_head_lbl, 140, 36);
    lv_obj_set_style_text_align(s_head_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 62, 220, 191, UI_SURFACE);
    lv_obj_set_style_pad_all(panel, 0, 0);
    for (int i = 0; i < TODO_MAX_ROWS; i++) {
        todo_row_t *row = &s_rows[i];
        row->row = box(panel, 0, i * TODO_ROW_H, 218, TODO_ROW_H,
                       UI_SURFACE, 0, 0);
        if (i > 0) box(row->row, 0, 0, 218, 1, UI_LINE, 0, 0);
        row->box = box(row->row, 8, 10, 15, 15, UI_SURFACE, 1,
                       LV_RADIUS_CIRCLE);
        row->title = ui_pixel_label(row->row, "", &ui_font_cjk_16, UI_INK);
        lv_obj_set_pos(row->title, 31, -1);
        lv_obj_set_size(row->title, 164, 22);
        lv_label_set_long_mode(row->title, LV_LABEL_LONG_WRAP);
        row->meta = ui_pixel_label(row->row, "", &lv_font_montserrat_14,
                                   UI_SUBTLE);
        lv_obj_set_pos(row->meta, 31, 20);
        row->priority = box(row->row, 201, 13, 8, 8, UI_INK, 1,
                            LV_RADIUS_CIRCLE);
    }

    s_foot_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_foot_lbl, 220);
    lv_obj_set_pos(s_foot_lbl, 10, 263);
    lv_obj_set_style_text_align(s_foot_lbl, LV_TEXT_ALIGN_CENTER, 0);
    ui_pixel_nav_create(s_scr, 2);

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
                      sync_ble_is_connected() ? "UP/DN  ·  OK DONE  ·  2X BACK"
                                              : "WAITING FOR PHONE");
}

void app_todo_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    const sync_store_t *st = app_store();
    if (st->todo_count == 0) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int count = (int)st->todo_count;
        s_sel = btn == BSP_BTN_UP ? (s_sel + count - 1) % count
                                  : (s_sel + 1) % count;
        if (s_sel < s_win) s_win = s_sel;
        if (s_sel >= s_win + TODO_MAX_ROWS) s_win = s_sel - TODO_MAX_ROWS + 1;
        if (s_sel == 0) s_win = 0;
        rows_rebuild();
        return;
    }

    if (btn == BSP_BTN_OK) {
        const sync_todo_item_t *item = sync_todo_at(st, (uint16_t)s_sel);
        if (!item) return;
        uint8_t done = item->done ? 0 : 1;
        if (!sync_todo_set_done(app_store(), item->id, done)) return;
        uint8_t frame[SYNC_FRAME_MAX];
        size_t size = sync_proto_build_todo_toggle(frame, sizeof(frame),
                                                   item->id, done);
        if (size) sync_ble_send(frame, size);
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
        s_head_lbl = s_foot_lbl = NULL;
    }
}

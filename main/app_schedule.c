// main/app_schedule.c — Paged list of imported calendar items.
// Up/down changes whole pages; the device keeps every visible item in RAM.
#include "app_schedule.h"

#include "app_pager.h"
#include "sync_ble.h"
#include "ui_font_cjk_16.h"
#include "ui_pixel.h"
#include "lvgl.h"

#include <stdio.h>

#define DAYS_ROW_H 47

typedef struct {
    lv_obj_t *row;
    lv_obj_t *badge;
    lv_obj_t *hour;
    lv_obj_t *minute;
    lv_obj_t *title;
    lv_obj_t *meta;
} day_row_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_count_lbl;
static lv_obj_t *s_foot_lbl;
static lv_obj_t *s_empty_title;
static lv_obj_t *s_empty_hint;
static day_row_t s_rows[SYNC_SCHED_PAGE_SIZE];
static uint16_t s_page;
static bool s_follow_default;

static const char *WEEK_EN[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

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

// Returns Sunday=0 ... Saturday=6. 1970-01-01 was a Thursday.
static int weekday_of(uint32_t unix_time, int16_t tz_min)
{
    int64_t days = sync_proto_local_day(unix_time, tz_min);
    return (int)((days + 4) % 7 + 7) % 7;
}

static void date_render(void)
{
    const sync_store_t *st = app_store();
    if (!st->time_set) {
        lv_label_set_text(s_date_lbl, "TIME NOT SET");
    } else {
        uint32_t now = 0;
        int month = 0;
        int day = 0;
        sync_ble_now(&now);
        sync_proto_local_time(now, st->tz_min, NULL, &month, &day, NULL, NULL);
        lv_label_set_text_fmt(s_date_lbl, "%02d/%02d %s", month, day,
                              WEEK_EN[weekday_of(now, st->tz_min)]);
    }
    lv_label_set_text_fmt(s_count_lbl, "%02u ITEMS %02u/%02u",
                          (unsigned)st->sched_count, (unsigned)s_page + 1,
                          (unsigned)sync_sched_page_count(st->sched_count));
}

static void rows_rebuild(void)
{
    const sync_store_t *st = app_store();
    uint16_t first = (uint16_t)(s_page * SYNC_SCHED_PAGE_SIZE);
    int shown = (int)st->sched_count - first;
    if (shown > SYNC_SCHED_PAGE_SIZE) shown = SYNC_SCHED_PAGE_SIZE;

    for (int i = 0; i < SYNC_SCHED_PAGE_SIZE; i++) {
        day_row_t *row = &s_rows[i];
        if (i >= shown) {
            lv_obj_add_flag(row->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const sync_sched_item_t *item = sync_sched_at(st, (uint16_t)(first + i));
        bool all_day = (item->flags & SYNC_SCHED_FLAG_ALL_DAY) != 0;
        lv_obj_set_style_bg_color(row->badge, lv_color_hex(UI_SURFACE), 0);
        lv_obj_set_style_text_color(row->hour, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_text_color(row->minute, lv_color_hex(UI_INK), 0);
        if (all_day) {
            lv_label_set_text(row->hour, "ALL");
            lv_label_set_text(row->minute, "DAY");
        } else {
            lv_label_set_text_fmt(row->hour, "%02u",
                                  (unsigned)(item->start_min / 60));
            lv_label_set_text_fmt(row->minute, "%02u",
                                  (unsigned)(item->start_min % 60));
        }
        lv_label_set_text(row->title, item->title_len ? item->title : "无标题日程");
        int month = 0, day = 0;
        sync_proto_date_from_day(item->epoch_day, NULL, &month, &day);
        if (all_day) {
            lv_label_set_text_fmt(row->meta, "%02d/%02d  ALL DAY", month, day);
        } else {
            lv_label_set_text_fmt(row->meta, "%02d/%02d  END %02u:%02u",
                                  month, day, (unsigned)(item->end_min / 60),
                                  (unsigned)(item->end_min % 60));
        }
        lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
    }

    if (st->sched_count == 0) {
        lv_obj_remove_flag(s_empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void app_schedule_enter(void)
{
    s_scr = ui_pixel_screen_create("DAYS");
    s_bat = app_battery_create(s_scr);

    s_date_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_width(s_date_lbl, 120);
    lv_obj_set_pos(s_date_lbl, 110, 18);
    lv_obj_set_style_text_align(s_date_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_count_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_count_lbl, 120);
    lv_obj_set_pos(s_count_lbl, 110, 36);
    lv_obj_set_style_text_align(s_count_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 62, 220, 190, UI_SURFACE);
    lv_obj_set_style_pad_all(panel, 0, 0);
    for (int i = 0; i < SYNC_SCHED_PAGE_SIZE; i++) {
        day_row_t *row = &s_rows[i];
        row->row = box(panel, 0, i * DAYS_ROW_H, 218, DAYS_ROW_H,
                       UI_SURFACE, 0, 0);
        if (i > 0) box(row->row, 0, 0, 218, 1, UI_LINE, 0, 0);
        row->badge = box(row->row, 6, 5, 46, 36, UI_SURFACE, 1, 6);
        row->hour = ui_pixel_label(row->badge, "", &lv_font_montserrat_14, UI_INK);
        lv_obj_set_width(row->hour, 46);
        lv_obj_set_pos(row->hour, 0, 0);
        lv_obj_set_style_text_align(row->hour, LV_TEXT_ALIGN_CENTER, 0);
        row->minute = ui_pixel_label(row->badge, "", &lv_font_montserrat_14, UI_INK);
        lv_obj_set_width(row->minute, 46);
        lv_obj_set_pos(row->minute, 0, 16);
        lv_obj_set_style_text_align(row->minute, LV_TEXT_ALIGN_CENTER, 0);
        row->title = ui_pixel_label(row->row, "", &ui_font_cjk_16, UI_INK);
        lv_obj_set_pos(row->title, 62, 3);
        lv_obj_set_size(row->title, 150, 22);
        lv_label_set_long_mode(row->title, LV_LABEL_LONG_MODE_DOTS);
        row->meta = ui_pixel_label(row->row, "", &lv_font_montserrat_14, UI_SUBTLE);
        lv_obj_set_pos(row->meta, 62, 24);
    }

    s_empty_title = ui_pixel_label(panel, "NO SCHEDULE", &lv_font_montserrat_20,
                                   UI_INK);
    lv_obj_set_width(s_empty_title, 218);
    lv_obj_set_pos(s_empty_title, 0, 64);
    lv_obj_set_style_text_align(s_empty_title, LV_TEXT_ALIGN_CENTER, 0);
    s_empty_hint = ui_pixel_label(panel, "SYNC FROM PHONE", &lv_font_montserrat_14,
                                  UI_SUBTLE);
    lv_obj_set_width(s_empty_hint, 218);
    lv_obj_set_pos(s_empty_hint, 0, 92);
    lv_obj_set_style_text_align(s_empty_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_foot_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_foot_lbl, 220);
    lv_obj_set_pos(s_foot_lbl, 10, 263);
    lv_obj_set_style_text_align(s_foot_lbl, LV_TEXT_ALIGN_CENTER, 0);
    ui_pixel_nav_create(s_scr, 1);

    s_page = 0;
    s_follow_default = true;
    app_schedule_refresh();
    lv_screen_load(s_scr);
}

void app_schedule_refresh(void)
{
    if (!s_scr) return;
    const sync_store_t *st = app_store();
    if (st->sched_count == 0) {
        s_page = 0;
    } else {
        uint16_t pages = sync_sched_page_count(st->sched_count);
        if (s_follow_default) {
            uint32_t now = st->unix_time;
            sync_ble_now(&now);
            s_page = sync_sched_default_page(
                st, sync_proto_local_day(now, st->tz_min));
        } else if (s_page >= pages) {
            s_page = pages - 1;
        }
    }
    date_render();
    rows_rebuild();
    lv_label_set_text(s_foot_lbl, "UP/DN PAGE  |  2X BACK");
}

void app_schedule_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    const sync_store_t *st = app_store();
    if (st->sched_count == 0) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        uint16_t pages = sync_sched_page_count(st->sched_count);
        uint16_t next = s_page;
        if (btn == BSP_BTN_UP && next > 0) next--;
        if (btn == BSP_BTN_DOWN && next + 1 < pages) next++;
        if (next != s_page) {
            s_page = next;
            s_follow_default = false;
            date_render();
            rows_rebuild();
        }
    }
}

void app_schedule_exit(void)
{
    if (s_scr) {
        app_battery_unregister(s_bat);
        s_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_date_lbl = s_count_lbl = s_foot_lbl = NULL;
        s_empty_title = s_empty_hint = NULL;
    }
}

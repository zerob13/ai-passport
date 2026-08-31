// main/app_schedule.c —— 当天日程页。
// 一条日程一屏(卡片):时间段、标题、位置 i/N、下一条预览。
// 上/下 = 上一条/下一条(循环);OK 单击 v1 无操作;数据由手机 BLE 推送。
#include "app_schedule.h"

#include "app_pager.h"
#include "sync_ble.h"
#include "ui_font_cjk_16.h"
#include "ui_pixel.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_date_lbl;    // 头部日期
static lv_obj_t *s_time_lbl;    // 时间段
static lv_obj_t *s_title_lbl;   // 标题(可换行)
static lv_obj_t *s_idx_lbl;     // i/N
static lv_obj_t *s_next_lbl;    // 下一条预览
static lv_obj_t *s_foot_lbl;    // 连接状态
static int s_idx;

static const char *WEEK_CN[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

// 0=周日..6=周六(1970-01-01 是周四)
static int weekday_of(uint32_t unix_time, int16_t tz_min)
{
    int64_t days = ((int64_t)unix_time + (int64_t)tz_min * 60) / 86400;
    return (int)((days + 4) % 7 + 7) % 7;
}

static void card_render(void)
{
    const sync_store_t *st = app_store();
    const sync_sched_item_t *it = sync_sched_at(st, (uint16_t)s_idx);

    if (!it) {
        lv_label_set_text(s_time_lbl, "--:--");
        lv_label_set_text(s_title_lbl, "暂无日程");
        lv_label_set_text(s_idx_lbl, "");
        lv_label_set_text(s_next_lbl, "等待手机 App 同步当天日程");
        return;
    }
    char t[32];
    snprintf(t, sizeof(t), "%02d:%02d - %02d:%02d",
             it->start_min / 60, it->start_min % 60,
             it->end_min / 60, it->end_min % 60);
    lv_label_set_text(s_time_lbl, t);
    lv_label_set_text(s_title_lbl,
                      it->title_len ? it->title : "无标题日程");
    lv_label_set_text_fmt(s_idx_lbl, "%d / %u", s_idx + 1, (unsigned)st->sched_count);

    const sync_sched_item_t *nx = sync_sched_at(st, (uint16_t)(s_idx + 1));
    if (nx) {
        static char nxt[96];
        snprintf(nxt, sizeof(nxt), "下一个: %02d:%02d  %s",
                 nx->start_min / 60, nx->start_min % 60,
                 nx->title_len ? nx->title : "");
        lv_label_set_text(s_next_lbl, nxt);
    } else if (st->sched_count > 1) {
        lv_label_set_text(s_next_lbl, "已到最后一条,继续按 下 回到开头");
    } else {
        lv_label_set_text(s_next_lbl, "今日仅此一条");
    }
}

static void date_render(void)
{
    const sync_store_t *st = app_store();
    if (!st->time_set) {
        lv_label_set_text(s_date_lbl, "今天 (等手机对时)");
        return;
    }
    uint32_t now = 0;
    sync_ble_now(&now);
    int y, mo, d;
    sync_proto_local_time(now, st->tz_min, &y, &mo, &d, NULL, NULL);
    int wd = weekday_of(now, st->tz_min);
    static char buf[48];
    snprintf(buf, sizeof(buf), "%d月%d日 %s", mo, d, WEEK_CN[wd]);
    lv_label_set_text(s_date_lbl, buf);
}

void app_schedule_enter(void)
{
    s_scr = ui_pixel_screen_create("SCHEDULE");
    s_bat = app_battery_create(s_scr);

    s_date_lbl = ui_pixel_label(s_scr, "", &ui_font_cjk_16, 0x1779B2);
    lv_obj_set_pos(s_date_lbl, 18, 46);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 64, 204, 172, UI_PAPER);

    s_time_lbl = ui_pixel_label(panel, "", &lv_font_montserrat_20, UI_INK);
    lv_obj_set_pos(s_time_lbl, 16, 18);

    s_title_lbl = ui_pixel_label(panel, "", &ui_font_cjk_16, UI_INK);
    lv_obj_set_width(s_title_lbl, 176);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_title_lbl, 16, 60);

    lv_obj_t *rule = lv_obj_create(panel);
    lv_obj_set_size(rule, 176, 2);
    lv_obj_set_pos(rule, 14, 118);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0xC9D6DC), 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    s_idx_lbl = ui_pixel_label(panel, "", &lv_font_montserrat_14, 0x5A6A73);
    lv_obj_set_pos(s_idx_lbl, 16, 130);

    s_next_lbl = ui_pixel_label(panel, "", &ui_font_cjk_16, 0x5A6A73);
    lv_obj_set_width(s_next_lbl, 176);
    lv_obj_set_style_text_align(s_next_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_next_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_next_lbl, 16, 146);

    s_foot_lbl = ui_pixel_label(s_scr, "", &ui_font_cjk_16, 0x1779B2);
    lv_obj_align(s_foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -26);

    s_idx = 0;
    date_render();
    card_render();
    lv_label_set_text(s_foot_lbl,
                      sync_ble_is_connected() ? "已连接手机" : "等待手机连接");
    lv_screen_load(s_scr);
}

void app_schedule_refresh(void)
{
    if (!s_scr) return;
    const sync_store_t *st = app_store();
    if (st->sched_count == 0) s_idx = 0;
    if (s_idx >= (int)st->sched_count) s_idx = (int)st->sched_count - 1;
    if (s_idx < 0) s_idx = 0;
    date_render();
    card_render();
    lv_label_set_text(s_foot_lbl,
                      sync_ble_is_connected() ? "已连接手机" : "等待手机连接");
}

void app_schedule_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    const sync_store_t *st = app_store();
    if (st->sched_count == 0) return;
    if (btn == BSP_BTN_UP) {
        s_idx = (s_idx + (int)st->sched_count - 1) % (int)st->sched_count;
        card_render();
    } else if (btn == BSP_BTN_DOWN) {
        s_idx = (s_idx + 1) % (int)st->sched_count;
        card_render();
    }
}

void app_schedule_exit(void)
{
    if (s_scr) {
        app_battery_unregister(s_bat);
        s_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_date_lbl = s_time_lbl = s_title_lbl = s_idx_lbl = s_next_lbl = s_foot_lbl = NULL;
    }
}
// main/app_pager.c —— 翻页模式 UI、模式切换、按键分发、共享电池指示。
#include "app_pager.h"

#include "app_record.h"
#include "app_schedule.h"
#include "app_todo.h"
#include "bsp_display.h"      // bsp_lvgl_lock/unlock
#include "bsp_battery.h"
#include "sync_ble.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "app_pager";

// 页面注册表(实现 app_pager.h 声明)。
const app_page_t APP_PAGES[PAGER_PAGE_COUNT] = {
    {
        .name = "RECORDING", .sub = "VOICE CAPTURE", .hint = "STREAM TO PHONE",
        .enter = app_record_enter, .exit = app_record_exit,
        .key = app_record_key, .refresh = app_record_refresh,
    },
    {
        .name = "DAYS", .sub = "DAILY SCHEDULE", .hint = "TODAY AT A GLANCE",
        .enter = app_schedule_enter, .exit = app_schedule_exit,
        .key = app_schedule_key, .refresh = app_schedule_refresh,
    },
    {
        .name = "TODO", .sub = "TASK LIST", .hint = "SYNCED WITH PHONE",
        .enter = app_todo_enter, .exit = app_todo_exit,
        .key = app_todo_key, .refresh = app_todo_refresh,
    },
};

// ---- 翻页模式 UI 状态 ----
static pager_t s_pager;
static lv_obj_t *s_scr;              // 翻页屏
static lv_obj_t *s_rows[PAGER_PAGE_COUNT];
static lv_obj_t *s_row_titles[PAGER_PAGE_COUNT];
static lv_obj_t *s_row_subs[PAGER_PAGE_COUNT];
static lv_obj_t *s_mode_label;
static lv_obj_t *s_link_label;
static lv_obj_t *s_nav;
static lv_obj_t *s_paging_bat;

// ---- 共享电池 ----
#define APP_BATTERY_MAX 4
static lv_obj_t *s_bat[APP_BATTERY_MAX];
static int s_bat_soc = -1;           // 最近一次读数(-1 未知)
static lv_timer_t *s_bat_timer;

// ============================================================================
// 电池指示
// ============================================================================

lv_obj_t *app_battery_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 164, 1);
    lv_obj_set_size(root, 68, 18);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    lv_obj_t *lbl = ui_pixel_label(root, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(lbl, 0, 0);

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(body, 43, 3);
    lv_obj_set_size(body, 21, 11);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_pad_all(body, 1, 0);
    lv_obj_t *cap = lv_obj_create(root);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(cap, 64, 6);
    lv_obj_set_size(cap, 2, 5);
    lv_obj_set_style_radius(cap, 0, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_pad_all(cap, 0, 0);
    lv_obj_t *fill = lv_obj_create(body);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(fill, 1, 1);
    lv_obj_set_size(fill, 15, 5);
    lv_obj_set_style_radius(fill, 1, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(UI_GRASS_DARK), 0);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_user_data(root, fill);

    app_battery_register(root);
    return root;
}

void app_battery_register(lv_obj_t *w)
{
    for (int i = 0; i < APP_BATTERY_MAX; i++) {
        if (s_bat[i] == NULL) { s_bat[i] = w; return; }
    }
}

void app_battery_unregister(lv_obj_t *w)
{
    for (int i = 0; i < APP_BATTERY_MAX; i++) {
        if (s_bat[i] == w) { s_bat[i] = NULL; return; }
    }
}

static void battery_draw_one(lv_obj_t *w)
{
    // root 的 user_data 存电量条对象
    lv_obj_t *fill = lv_obj_get_user_data(w);
    lv_obj_t *lbl = lv_obj_get_child(w, 0);
    if (s_bat_soc < 0) {
        lv_label_set_text(lbl, "--");
        lv_obj_set_style_bg_color(fill, lv_color_hex(0x9AA7AD), 0);
        return;
    }
    lv_label_set_text_fmt(lbl, "%d%%", s_bat_soc);
    uint32_t color = s_bat_soc > 50 ? UI_INK :
                     (s_bat_soc > 20 ? UI_YELLOW : UI_RED);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_set_width(fill, (15 * s_bat_soc) / 100);
}

static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    int soc = bsp_battery_soc();
    if (soc != s_bat_soc) {
        s_bat_soc = soc;
        for (int i = 0; i < APP_BATTERY_MAX; i++) {
            if (s_bat[i]) battery_draw_one(s_bat[i]);
        }
        // 电量变化 ≥5% 或首次读数时给手机推一条 STATUS
        static int last_status_soc = -100;
        if (soc < 0 || soc - last_status_soc >= 5 || last_status_soc - soc >= 5) {
            last_status_soc = soc;
            sync_ble_send_status_now();
        }
    }
}

// ============================================================================
// 翻页模式
// ============================================================================

static void card_refresh(void)
{
    for (int i = 0; i < PAGER_PAGE_COUNT; i++) {
        bool active = i == (int)s_pager.page;
        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex(active ? UI_INK : UI_SURFACE), 0);
        lv_obj_set_style_border_color(s_rows[i], lv_color_hex(UI_INK), 0);
        lv_obj_set_style_text_color(s_row_titles[i],
                                    lv_color_hex(active ? UI_SURFACE : UI_INK), 0);
        lv_obj_set_style_text_color(s_row_subs[i],
                                    lv_color_hex(active ? UI_LINE : UI_SUBTLE), 0);
    }
    lv_label_set_text_fmt(s_mode_label, "MODE %d / %d",
                          (int)s_pager.page + 1, PAGER_PAGE_COUNT);
    lv_label_set_text(s_link_label,
                      sync_ble_is_connected() ? "PHONE CONNECTED" : "WAITING FOR PHONE");
    if (s_nav) lv_obj_delete(s_nav);
    s_nav = ui_pixel_nav_create(s_scr, (int)s_pager.page);
}

static void paging_build(void)
{
    s_scr = ui_pixel_screen_create("PASSPORT");
    s_paging_bat = app_battery_create(s_scr);

    s_mode_label = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_width(s_mode_label, 100);
    lv_obj_set_pos(s_mode_label, 130, 30);
    lv_obj_set_style_text_align(s_mode_label, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < PAGER_PAGE_COUNT; i++) {
        s_rows[i] = ui_pixel_panel_create(s_scr, 10, 64 + i * 58, 220, 50,
                                          UI_SURFACE);
        lv_obj_set_style_pad_all(s_rows[i], 0, 0);
        s_row_titles[i] = ui_pixel_label(s_rows[i], APP_PAGES[i].name,
                                         &lv_font_montserrat_20, UI_INK);
        lv_obj_set_pos(s_row_titles[i], 10, 2);
        s_row_subs[i] = ui_pixel_label(s_rows[i], APP_PAGES[i].sub,
                                       &lv_font_montserrat_14, UI_SUBTLE);
        lv_obj_set_pos(s_row_subs[i], 10, 26);
        lv_obj_t *index = ui_pixel_label(s_rows[i], "", &lv_font_montserrat_14,
                                         UI_SUBTLE);
        lv_label_set_text_fmt(index, "0%d", i + 1);
        lv_obj_set_pos(index, 188, 13);
    }

    s_link_label = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_width(s_link_label, 220);
    lv_obj_set_pos(s_link_label, 10, 244);
    lv_obj_set_style_text_align(s_link_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint = ui_pixel_label(s_scr, "UP / DOWN MODE  ·  OK OPEN",
                                    &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(hint, 220);
    lv_obj_set_pos(hint, 10, 264);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    card_refresh();
    lv_screen_load(s_scr);
}

// BLE 状态变化:翻页屏只更新连接提示;页面在时通知页面刷新。
static void on_sync_evt(sync_ble_evt_t ev)
{
    if (!bsp_lvgl_lock(300)) return;
    if (s_pager.mode == PAGER_MODE_PAGING) {
        if (s_link_label) {
            lv_label_set_text(s_link_label,
                              sync_ble_is_connected() ? "PHONE CONNECTED" : "WAITING FOR PHONE");
        }
    } else if (APP_PAGES[s_pager.page].refresh) {
        APP_PAGES[s_pager.page].refresh();
    }
    (void)ev;
    bsp_lvgl_unlock();
}

// ============================================================================
// 按键分发(全局)
// ============================================================================

void app_key_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    if (s_scr == NULL && s_pager.mode == PAGER_MODE_PAGING) {
        // 翻页界面尚未建好(初始化早期),忽略本次按键
        bsp_lvgl_unlock();
        return;
    }

    // 事件 → pager 输入
    pager_ev_t pev;
    switch (ev) {
    case BSP_BTN_CLICK:
        pev = (btn == BSP_BTN_UP) ? PAGER_EV_UP :
              (btn == BSP_BTN_DOWN) ? PAGER_EV_DOWN : PAGER_EV_OK_CLICK;
        break;
    case BSP_BTN_DOUBLE:
        pev = PAGER_EV_OK_DOUBLE;
        break;
    case BSP_BTN_LONG:
        pev = PAGER_EV_OK_LONG;
        break;
    default:
        bsp_lvgl_unlock();
        return;
    }
    // 只有确定键的双击/长按有意义;上/下/确定之外的组合忽略
    if (ev == BSP_BTN_DOUBLE || ev == BSP_BTN_LONG) {
        if (btn != BSP_BTN_OK) { bsp_lvgl_unlock(); return; }
    }

    pager_act_t act = pager_handle(&s_pager, pev);
    switch (act) {
    case PAGER_ACT_FLIP:
        card_refresh();
        break;
    case PAGER_ACT_ENTER:
        app_battery_unregister(s_paging_bat);
        s_paging_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = s_mode_label = s_link_label = s_nav = NULL;
        memset(s_rows, 0, sizeof(s_rows));
        memset(s_row_titles, 0, sizeof(s_row_titles));
        memset(s_row_subs, 0, sizeof(s_row_subs));
        APP_PAGES[s_pager.page].enter();
        break;
    case PAGER_ACT_BACK:
        APP_PAGES[s_pager.page].exit();
        paging_build();
        break;
    case PAGER_ACT_PAGE_UP:
    case PAGER_ACT_PAGE_DOWN:
    case PAGER_ACT_PAGE_OK:
        if (s_pager.mode == PAGER_MODE_IN_PAGE &&
            APP_PAGES[s_pager.page].key) {
            APP_PAGES[s_pager.page].key(btn, ev);
        }
        break;
    default:
        break;
    }
    bsp_lvgl_unlock();
}

void app_pager_start(void)
{
    pager_init(&s_pager);
    sync_ble_start(on_sync_evt);
    if (!s_bat_timer) {
        s_bat_timer = lv_timer_create(battery_tick, 5000, NULL);
        lv_timer_ready(s_bat_timer);      // 立即读一次电量
    }
    paging_build();
    ESP_LOGI(TAG, "翻页模式就绪");
}

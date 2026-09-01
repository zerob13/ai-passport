// main/app_pager.h —— 翻页模式 UI 与页面注册表。
// 交互语义见 docs/software-design/passport-sync-app.md §1。
#pragma once

#include "bsp_button.h"
#include "lvgl.h"
#include "pager_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;              // 英文标题(标题板)
    const char *sub;               // 中文副标题(卡片)
    const char *hint;              // 卡片提示(中文)
    void (*enter)(void);           // 建页并载入
    void (*exit)(void);            // 停任务/删页
    void (*key)(bsp_btn_t, bsp_btn_ev_t);  // 页面内按键
    void (*refresh)(void);         // 数据/BLE 状态变化时刷新
} app_page_t;

// 页面注册表,顺序即翻页顺序(与 pager_core.h 的 PAGER_PAGE_* 对应)。
extern const app_page_t APP_PAGES[PAGER_PAGE_COUNT];

// 启动:初始化状态机并显示翻页界面(在 LVGL 锁内调用)。
void app_pager_start(void);

// 全局按键回调(注册给 bsp_button;运行于按键任务,内部自加 LVGL 锁)。
void app_key_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// ---- 共享电池指示(每个页面可挂一个,注册后由全局定时器刷新)----
// 返回根对象。页面退出删屏前必须先 app_battery_unregister()。
lv_obj_t *app_battery_create(lv_obj_t *parent);
void app_battery_register(lv_obj_t *w);
void app_battery_unregister(lv_obj_t *w);

#ifdef __cplusplus
}
#endif

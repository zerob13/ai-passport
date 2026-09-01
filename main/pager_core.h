// main/pager_core.h —— 翻页/页面两级模式状态机(纯 C,可主机测试)。
// 只描述"按键事件 → 动作",不碰 LVGL。UI 层根据动作渲染。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 页面注册表顺序即翻页顺序。
typedef enum {
    PAGER_PAGE_RECORD = 0,   // 录音
    PAGER_PAGE_SCHEDULE,     // 当天日程
    PAGER_PAGE_TODO,         // 任务 Todo
    PAGER_PAGE_MUSIC,        // 手机当前播放
    PAGER_PAGE_COUNT,
} pager_page_t;

typedef enum {
    PAGER_MODE_PAGING = 0,   // 翻页模式(开机默认)
    PAGER_MODE_IN_PAGE,      // 页面模式
} pager_mode_t;

// 输入事件。BSP 按键事件已保证单击/双击互斥。
typedef enum {
    PAGER_EV_UP = 0,         // 上键 单击
    PAGER_EV_DOWN,           // 下键 单击
    PAGER_EV_OK_CLICK,       // 确定键 单击
    PAGER_EV_OK_DOUBLE,      // 确定键 双击
    PAGER_EV_OK_LONG,        // 确定键 长按
} pager_ev_t;

// 输出动作。UI 层据此执行。
typedef enum {
    PAGER_ACT_NONE = 0,
    PAGER_ACT_FLIP,          // 翻页:当前页已改变,刷新卡片
    PAGER_ACT_ENTER,         // 进入当前页(翻页 → 页面)
    PAGER_ACT_BACK,          // 退回翻页模式(页面已退出)
    PAGER_ACT_SHUTDOWN,      // Paging-mode long OK: software shutdown
    PAGER_ACT_PAGE_UP,       // 上键交给页面
    PAGER_ACT_PAGE_DOWN,     // 下键交给页面
    PAGER_ACT_PAGE_OK,       // 确定单击交给页面
} pager_act_t;

typedef struct {
    pager_mode_t mode;
    pager_page_t page;       // 翻页模式:当前选中卡片;页面模式:当前所在页面
} pager_t;

// 初始化为翻页模式、第一页。
void pager_init(pager_t *p);

// Feed one key event and return the UI action. Semantics match
// docs/software-design/passport-sync-app.md section 1:
//   Paging: UP/DOWN flips, OK click enters, OK long shuts down, double is no-op.
//   In-page: UP/DOWN/OK click goes to the page; OK double/long returns.
pager_act_t pager_handle(pager_t *p, pager_ev_t ev);

#ifdef __cplusplus
}
#endif

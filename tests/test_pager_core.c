// tests/test_pager_core.c —— 翻页状态机主机测试。
#include <assert.h>
#include "pager_core.h"

int main(void)
{
    pager_t p;

    // 初始:翻页模式,第一页
    pager_init(&p);
    assert(p.mode == PAGER_MODE_PAGING);
    assert(p.page == PAGER_PAGE_RECORD);

    // 翻页模式:上/下翻页(循环)
    assert(pager_handle(&p, PAGER_EV_DOWN) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_DOWN) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_TODO);
    assert(pager_handle(&p, PAGER_EV_DOWN) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_RECORD);            // 回绕
    assert(pager_handle(&p, PAGER_EV_UP) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_TODO);              // 回绕
    assert(pager_handle(&p, PAGER_EV_UP) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_UP) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_RECORD);

    // Paging mode: OK double is a no-op; OK long requests software shutdown.
    assert(pager_handle(&p, PAGER_EV_OK_DOUBLE) == PAGER_ACT_NONE);
    assert(p.mode == PAGER_MODE_PAGING);
    assert(pager_handle(&p, PAGER_EV_OK_LONG) == PAGER_ACT_SHUTDOWN);
    assert(p.mode == PAGER_MODE_PAGING);

    // 进入页面:保留当前页,切到页面模式
    assert(p.page == PAGER_PAGE_RECORD);
    assert(pager_handle(&p, PAGER_EV_OK_CLICK) == PAGER_ACT_ENTER);
    assert(p.mode == PAGER_MODE_IN_PAGE);
    assert(p.page == PAGER_PAGE_RECORD);

    // 页面模式:上/下/确定单击交页面;双击/长按退回
    assert(pager_handle(&p, PAGER_EV_UP) == PAGER_ACT_PAGE_UP);
    assert(pager_handle(&p, PAGER_EV_DOWN) == PAGER_ACT_PAGE_DOWN);
    assert(pager_handle(&p, PAGER_EV_OK_CLICK) == PAGER_ACT_PAGE_OK);
    assert(p.mode == PAGER_MODE_IN_PAGE);
    assert(pager_handle(&p, PAGER_EV_OK_DOUBLE) == PAGER_ACT_BACK);
    assert(p.mode == PAGER_MODE_PAGING);
    assert(p.page == PAGER_PAGE_RECORD);

    // 再进一次,用长按退回
    assert(pager_handle(&p, PAGER_EV_DOWN) == PAGER_ACT_FLIP);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_OK_CLICK) == PAGER_ACT_ENTER);
    assert(p.mode == PAGER_MODE_IN_PAGE);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_OK_LONG) == PAGER_ACT_BACK);
    assert(p.mode == PAGER_MODE_PAGING);

    // 页面模式的页面键不会改变翻页位置
    assert(pager_handle(&p, PAGER_EV_OK_CLICK) == PAGER_ACT_ENTER);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_UP) == PAGER_ACT_PAGE_UP);
    assert(p.page == PAGER_PAGE_SCHEDULE);
    assert(pager_handle(&p, PAGER_EV_OK_DOUBLE) == PAGER_ACT_BACK);

    return 0;
}

// main/pager_core.c —— 翻页状态机实现(纯 C)。见 pager_core.h。
#include "pager_core.h"

void pager_init(pager_t *p)
{
    p->mode = PAGER_MODE_PAGING;
    p->page = PAGER_PAGE_RECORD;
}

pager_act_t pager_handle(pager_t *p, pager_ev_t ev)
{
    if (p->mode == PAGER_MODE_IN_PAGE) {
        switch (ev) {
        case PAGER_EV_OK_DOUBLE:
        case PAGER_EV_OK_LONG:
            p->mode = PAGER_MODE_PAGING;
            return PAGER_ACT_BACK;
        case PAGER_EV_UP:
            return PAGER_ACT_PAGE_UP;
        case PAGER_EV_DOWN:
            return PAGER_ACT_PAGE_DOWN;
        case PAGER_EV_OK_CLICK:
            return PAGER_ACT_PAGE_OK;
        default:
            return PAGER_ACT_NONE;
        }
    }

    // 翻页模式
    switch (ev) {
    case PAGER_EV_UP:
        p->page = (pager_page_t)((p->page + PAGER_PAGE_COUNT - 1) % PAGER_PAGE_COUNT);
        return PAGER_ACT_FLIP;
    case PAGER_EV_DOWN:
        p->page = (pager_page_t)((p->page + 1) % PAGER_PAGE_COUNT);
        return PAGER_ACT_FLIP;
    case PAGER_EV_OK_CLICK:
        p->mode = PAGER_MODE_IN_PAGE;
        return PAGER_ACT_ENTER;
    case PAGER_EV_OK_LONG:
        return PAGER_ACT_SHUTDOWN;
    default:
        return PAGER_ACT_NONE;
    }
}

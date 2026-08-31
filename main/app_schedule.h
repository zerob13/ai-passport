// main/app_schedule.h —— 当天日程页:一条一屏,上下翻看。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_schedule_enter(void);
void app_schedule_exit(void);
void app_schedule_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void app_schedule_refresh(void);

#ifdef __cplusplus
}
#endif
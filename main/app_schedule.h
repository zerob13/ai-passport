// main/app_schedule.h —— Imported schedule list with local page navigation.
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

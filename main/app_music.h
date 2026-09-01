#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_music_enter(void);
void app_music_exit(void);
void app_music_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void app_music_refresh(void);

#ifdef __cplusplus
}
#endif
